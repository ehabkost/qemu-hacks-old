#include "config.h"
#include "config-host.h"

#include <string.h>

#include "hw/hw.h"
#include "qemu-kvm.h"
#include <libkvm.h>
#include <pthread.h>
#include <sys/utsname.h>


extern kvm_context_t kvm_context;

int kvm_arch_qemu_create_context(void)
{
    return 0;
}

void kvm_arch_load_regs(CPUState *env)
{
}


void kvm_arch_save_regs(CPUState *env)
{
}

int kvm_arch_qemu_init_env(CPUState *cenv)
{
    return 0;
}

int kvm_arch_halt(void *opaque, int vcpu)
{
    CPUState *env = cpu_single_env;
    env->hflags |= HF_HALTED_MASK;
    env->exception_index = EXCP_HLT;
    return 1;
}

void kvm_arch_pre_kvm_run(void *opaque, CPUState *env)
{
}

void kvm_arch_post_kvm_run(void *opaque, CPUState *env)
{
}

int kvm_arch_has_work(CPUState *env)
{
    return 1;
}

int kvm_arch_try_push_interrupts(void *opaque)
{
    return 1;
}

void kvm_arch_push_nmi(void *opaque)
{
}

void kvm_arch_update_regs_for_sipi(CPUState *env)
{
}

void kvm_save_mpstate(CPUState *env)
{
#ifdef KVM_CAP_MP_STATE
    int r;
    struct kvm_mp_state mp_state;

    r = kvm_get_mpstate(kvm_context, env->cpu_index, &mp_state);
    if (r < 0)
        env->mp_state = -1;
    else
        env->mp_state = mp_state.mp_state;
#endif
}

void kvm_load_mpstate(CPUState *env)
{
#ifdef KVM_CAP_MP_STATE
    struct kvm_mp_state mp_state = { .mp_state = env->mp_state };

    /*
     * -1 indicates that the host did not support GET_MP_STATE ioctl,
     *  so don't touch it.
     */
    if (env->mp_state != -1)
        kvm_set_mpstate(kvm_context, env->cpu_index, &mp_state);
#endif
}

void kvm_arch_cpu_reset(CPUState *env)
{
    if (kvm_irqchip_in_kernel(kvm_context)) {
#ifdef KVM_CAP_MP_STATE
	kvm_reset_mpstate(kvm_context, env->cpu_index);
#endif
    } else {
	env->interrupt_request &= ~CPU_INTERRUPT_HARD;
	env->halted = 1;
	env->exception_index = EXCP_HLT;
    }
}

void kvm_arch_do_ioperm(void *_data)
{
    struct ioperm_data *data = _data;
    ioperm(data->start_port, data->num, data->turn_on);
}
