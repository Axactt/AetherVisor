#include "vmexit.h"
#include "npt_sandbox.h"
#include "branch_tracer.h"

void HandleBreakpoint(VcpuData* vcpu_data, GuestRegisters* guest_ctx)
{
    auto guest_rip = vcpu_data->guest_vmcb.save_state_area.Rip;

    DbgPrint("vcpu_data->guest_vmcb.save_state_area.Rip = %p \n", guest_rip);

    if (BranchTracer::initialized && guest_rip == BranchTracer::start_address && !BranchTracer::thread_id)
    {
        NPTHooks::ForEachHook(
            [](auto hook_entry, auto data)-> auto {

                if (hook_entry->address == data)
                {
                    UnsetHook(hook_entry);
                }

                return false;
            },
            (void*)guest_rip
        );

        /*  capture the ID of the target thread */

        BranchTracer::Start(vcpu_data);

        __debugbreak();

        BranchTracer::thread_id = PsGetCurrentThreadId();;
        __debugbreak();

        int processor_id = KeGetCurrentProcessorNumber();
        __debugbreak();

        KAFFINITY affinity = Utils::Exponent(2, processor_id);
        __debugbreak();


        KeSetSystemAffinityThread(affinity);      
        
        __debugbreak();

    }
    else
    {
        InjectException(vcpu_data, EXCEPTION_VECTOR::Breakpoint, FALSE, 0);
    }
}