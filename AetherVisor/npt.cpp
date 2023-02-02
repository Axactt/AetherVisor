#include "npt_sandbox.h"
#include "npt_hook.h"
#include "logging.h"
#include "hypervisor.h"
#include "paging_utils.h"
#include "branch_tracer.h"

#pragma optimize( "", off )

bool HandleSplitInstruction(VcpuData* vcpu, uintptr_t guest_rip, PHYSICAL_ADDRESS faulting_physical, bool is_hooked_page)
{
	PHYSICAL_ADDRESS ncr3; ncr3.QuadPart = vcpu->guest_vmcb.control_area.ncr3;

	bool switch_ncr3 = true;

	ZydisDecodedOperand operands[5];

	int insn_len = Disasm::Disassemble((uint8_t*)guest_rip, operands).length;

	/*	handle cases where an instruction is split across 2 pages (using SINGLE STEP is better here tbh)	*/

	if (PAGE_ALIGN(guest_rip + insn_len) != PAGE_ALIGN(guest_rip))
	{
		if (is_hooked_page)
		{
			Logger::Get()->LogJunk("instruction is split, entering hook page!\n");

			/*	if CPU is entering the page:	*/

			switch_ncr3 = true;

			ncr3.QuadPart = Hypervisor::Get()->ncr3_dirs[shadow];
		}
		else
		{
			Logger::Get()->LogJunk("instruction is split, leaving hook page! \n");

			/*	if CPU is leaving the page:	*/

			switch_ncr3 = false;
		}

		auto guest_cr3 = vcpu->guest_vmcb.save_state_area.cr3.Flags;

		auto page1_physical = faulting_physical.QuadPart;
		auto page2_physical = Utils::GetPte((void*)(guest_rip + insn_len), guest_cr3)->PageFrameNumber << PAGE_SHIFT;

		Utils::GetPte((void*)page1_physical, ncr3.QuadPart)->ExecuteDisable = 0;
		Utils::GetPte((void*)page2_physical, ncr3.QuadPart)->ExecuteDisable = 0;
	}

	return switch_ncr3;
}


void NestedPageFaultHandler(VcpuData* vcpu, GuestRegs* guest_registers)
{
	PHYSICAL_ADDRESS fault_physical; fault_physical.QuadPart = vcpu->guest_vmcb.control_area.exit_info2;

	NestedPageFaultInfo1 exit_info1; exit_info1.as_uint64 = vcpu->guest_vmcb.control_area.exit_info1;

	PHYSICAL_ADDRESS ncr3; ncr3.QuadPart = vcpu->guest_vmcb.control_area.ncr3;

	auto guest_rip = vcpu->guest_vmcb.save_state_area.rip;

	/*	clean ncr3 cache	*/

	vcpu->guest_vmcb.control_area.vmcb_clean &= 0xFFFFFFEF;
	vcpu->guest_vmcb.control_area.tlb_control = 1;

	Logger::Get()->LogJunk("[#NPF HANDLER] 	guest physical %p, guest RIP virtual %p \n", fault_physical.QuadPart, vcpu->guest_vmcb.save_state_area.rip);

	if (exit_info1.fields.valid == 0)
	{
		auto denied_read_page = Sandbox::ForEachHook(
			[](auto hook_entry, auto data) -> auto {

				if (data == hook_entry->guest_physical && hook_entry->unreadable)
				{
					return true;
				}
				else
				{
					return false;
				}
			}, PAGE_ALIGN(fault_physical.QuadPart)
		);

		if (denied_read_page && ncr3.QuadPart == Hypervisor::Get()->ncr3_dirs[sandbox])
		{
			DbgPrint("single stepping at guest_rip = %p \n", guest_rip);

			/*
				Single-step the read/write in the ncr3 that allows all pages to be executable.
				Single-stepping mode => single-step on every instruction
			*/

			BranchTracer::Pause(vcpu);

			vcpu->guest_vmcb.save_state_area.rflags.TrapFlag = 1;

			vcpu->guest_vmcb.control_area.ncr3 = Hypervisor::Get()->ncr3_dirs[sandbox_single_step];
		}
		else
		{
			/*	map in the missing memory (primary and hook NCR3 only)	*/

			auto pml4_base = (PML4E_64*)MmGetVirtualForPhysical(ncr3);

			auto pte = AssignNptEntry((PML4E_64*)pml4_base, fault_physical.QuadPart, PTEAccess{ true, true, true });
		}

		return;
	}

	if (exit_info1.fields.execute == 1)
	{
		if (guest_rip > BranchTracer::range_base && guest_rip < (BranchTracer::range_size + BranchTracer::range_base))
		{
			/*  Resume the branch tracer after an NCR3 switch, if the tracer is active.
				Single-stepping mode => only #DB on branches
			*/
			BranchTracer::Resume(vcpu);
		}

		if (vcpu->guest_vmcb.control_area.ncr3 == Hypervisor::Get()->ncr3_dirs[sandbox])
		{
			/*  call out of sandbox context and set RIP to the instrumentation hook for executes  */

			auto is_system_page = (vcpu->guest_vmcb.save_state_area.cr3.Flags == __readcr3()) ? true : false;

			Instrumentation::InvokeHook(vcpu, Instrumentation::sandbox_execute, is_system_page);
		}

		auto sandbox_npte = Utils::GetPte((void*)fault_physical.QuadPart, Hypervisor::Get()->ncr3_dirs[sandbox]);

		if (sandbox_npte->ExecuteDisable == FALSE)
		{
			/*  enter into the sandbox context	*/

			// DbgPrint("0x%p is a sandbox page! \n", faulting_physical.QuadPart);

			vcpu->guest_vmcb.control_area.ncr3 = Hypervisor::Get()->ncr3_dirs[sandbox];

			return;
		}

		auto npthooked_page = Utils::GetPte((void*)fault_physical.QuadPart, Hypervisor::Get()->ncr3_dirs[shadow]);

		/*	handle cases where an instruction is split across 2 pages	*/

		auto switch_ncr3 = HandleSplitInstruction(vcpu, guest_rip, fault_physical, !npthooked_page->ExecuteDisable);

		if (switch_ncr3)
		{
			if (!npthooked_page->ExecuteDisable)
			{
				/*  move into hooked page and switch to ncr3 with hooks mapped  */

				vcpu->guest_vmcb.control_area.ncr3 = Hypervisor::Get()->ncr3_dirs[shadow];
			}
			else
			{
				vcpu->guest_vmcb.control_area.ncr3 = Hypervisor::Get()->ncr3_dirs[primary];
			}
		}
	}
}

#pragma optimize( "", on )


/*	AllocateNewTable(): Allocate a new nested page table (nPML4, nPDPT, nPD, nPT) for a guest physical address translation
*/

void* AllocateNewTable(PT_ENTRY_64* page_entry)
{
	void* page_table = ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, 'ENON');

	page_entry->PageFrameNumber = MmGetPhysicalAddress(page_table).QuadPart >> PAGE_SHIFT;
	page_entry->Write = 1;
	page_entry->Supervisor = 1;
	page_entry->Present = 1;
	page_entry->ExecuteDisable = 0;

	return page_table;
}


int GetPhysicalMemoryRanges()
{
	int num_of_runs = 0;

	PPHYSICAL_MEMORY_RANGE memory_range = MmGetPhysicalMemoryRanges();

	for (num_of_runs = 0;
		(memory_range[num_of_runs].BaseAddress.QuadPart) || (memory_range[num_of_runs].NumberOfBytes.QuadPart);
		num_of_runs++)
	{
		Hypervisor::Get()->phys_mem_range[num_of_runs] = memory_range[num_of_runs];
	}

	return num_of_runs;
}


/*	assign a new NPT entry to an unmapped guest physical address	*/

PTE_64*	AssignNptEntry(PML4E_64* npml4, uintptr_t physical_addr, PTEAccess flags)
{
	AddressTranslationHelper address_bits;

	address_bits.as_int64 = physical_addr;

	PML4E_64* pml4e = &npml4[address_bits.AsIndex.pml4];
	PDPTE_64* pdpt;

	if (pml4e->Present == 0)
	{
		pdpt = (PDPTE_64*)AllocateNewTable(pml4e);
	}
	else
	{
		pdpt = (PDPTE_64*)Utils::PfnToVirtualAddr(pml4e->PageFrameNumber);
	}

	PDPTE_64* pdpte = &pdpt[address_bits.AsIndex.pdpt];
	PDE_64* pd;

	if (pdpte->Present == 0)
	{
		pd = (PDE_64*)AllocateNewTable((PML4E_64*)pdpte);
	}
	else
	{
		pd = (PDE_64*)Utils::PfnToVirtualAddr(pdpte->PageFrameNumber);
	}

	PDE_64* pde = &pd[address_bits.AsIndex.pd];
	PTE_64* pt;

	if (pde->Present == 0)
	{
		pt = (PTE_64*)AllocateNewTable((PML4E_64*)pde);
	}
	else
	{
		pt = (PTE_64*)Utils::PfnToVirtualAddr(pde->PageFrameNumber);
	}

	PTE_64* pte = &pt[address_bits.AsIndex.pt];

	pte->PageFrameNumber = static_cast<PFN_NUMBER>(physical_addr >> PAGE_SHIFT);
	pte->Supervisor = 1;
	pte->Write = flags.writable;
	pte->Present = flags.present;
	pte->ExecuteDisable = !flags.execute;

	return pte;
}

uintptr_t BuildNestedPagingTables(uintptr_t* ncr3, PTEAccess flags)
{
	auto run_count = GetPhysicalMemoryRanges();

	auto npml4_virtual = (PML4E_64*)ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, 'ENON');

	*ncr3 = MmGetPhysicalAddress(npml4_virtual).QuadPart;

	DbgPrint("[SETUP] npml4_virtual %p flags.present %i flags.write  %i flags.execute  %i \n", npml4_virtual, flags.present, flags.writable, flags.execute);

	/*	Create an 1:1 guest to host translation for each physical page address	*/

	for (int run = 0; run < run_count; ++run)
	{
		uintptr_t page_count = Hypervisor::Get()->phys_mem_range[run].NumberOfBytes.QuadPart / PAGE_SIZE;
		uintptr_t pages_base = Hypervisor::Get()->phys_mem_range[run].BaseAddress.QuadPart / PAGE_SIZE;

		for (PFN_NUMBER pfn = pages_base; pfn < pages_base + page_count; ++pfn)
		{
			AssignNptEntry(npml4_virtual, pfn << PAGE_SHIFT, flags);
		}
	}

	/*	APIC range isn't covered by system physical memory ranges, but it still needs to be visible	*/

	ApicBarMsr apic_bar;

	apic_bar.Flags = __readmsr(MSR::apic_bar);

	AssignNptEntry(npml4_virtual, apic_bar.apic_base << PAGE_SHIFT, flags);

	return *ncr3;
}