
#include "config.h"
#include "config-host.h"

#ifdef USE_KVM
 #define KVM_ALLOWED_DEFAULT 1
#else
 #define KVM_ALLOWED_DEFAULT 0
#endif

int kvm_allowed = KVM_ALLOWED_DEFAULT;
int lm_capable_kernel;
int kvm_irqchip = 1;

#ifdef USE_KVM

#include <string.h>
#include "vl.h"

#include "qemu-kvm.h"
#include <libkvm.h>
#include <pthread.h>
#include <sys/utsname.h>

extern void perror(const char *s);

kvm_context_t kvm_context;

extern int smp_cpus;

pthread_mutex_t qemu_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread CPUState *vcpu_env;

static sigset_t io_sigset, io_negsigset;

static int wait_hack;

#define SIG_IPI (SIGRTMIN+4)

struct vcpu_info {
    int sipi_needed;
    int init;
    pthread_t thread;
    int signalled;
    int stop;
    int stopped;
} vcpu_info[4];

static void sig_ipi_handler(int n)
{
}

void kvm_update_interrupt_request(CPUState *env)
{
    if (env && env != vcpu_env) {
	if (vcpu_info[env->cpu_index].signalled)
	    return;
	vcpu_info[env->cpu_index].signalled = 1;
	if (vcpu_info[env->cpu_index].thread)
	    pthread_kill(vcpu_info[env->cpu_index].thread, SIG_IPI);
    }
}

void kvm_update_after_sipi(CPUState *env)
{
    vcpu_info[env->cpu_index].sipi_needed = 1;
    kvm_update_interrupt_request(env);

    /*
     * the qemu bios waits using a busy loop that's much too short for
     * kvm.  add a wait after the first sipi.
     */
    {
	static int first_sipi = 1;

	if (first_sipi) {
	    wait_hack = 1;
	    first_sipi = 0;
	}
    }
}

void kvm_apic_init(CPUState *env)
{
    if (env->cpu_index != 0)
	vcpu_info[env->cpu_index].init = 1;
    kvm_update_interrupt_request(env);
}

#include <signal.h>

static int try_push_interrupts(void *opaque)
{
    CPUState *env = cpu_single_env;
    int r, irq;

    if (env->ready_for_interrupt_injection &&
        (env->interrupt_request & CPU_INTERRUPT_HARD) &&
        (env->eflags & IF_MASK)) {
            env->interrupt_request &= ~CPU_INTERRUPT_HARD;
	    irq = cpu_get_pic_interrupt(env);
	    if (irq >= 0) {
		r = kvm_inject_irq(kvm_context, env->cpu_index, irq);
		if (r < 0)
		    printf("cpu %d fail inject %x\n", env->cpu_index, irq);
	    }
    }

    return (env->interrupt_request & CPU_INTERRUPT_HARD) != 0;
}

static void post_kvm_run(void *opaque, int vcpu)
{
    CPUState *env = vcpu_env;

    pthread_mutex_lock(&qemu_mutex);
    cpu_single_env = env;
    env->eflags = kvm_get_interrupt_flag(kvm_context, vcpu)
	? env->eflags | IF_MASK : env->eflags & ~IF_MASK;
    env->ready_for_interrupt_injection
	= kvm_is_ready_for_interrupt_injection(kvm_context, vcpu);

    cpu_set_apic_tpr(env, kvm_get_cr8(kvm_context, vcpu));
    cpu_set_apic_base(env, kvm_get_apic_base(kvm_context, vcpu));
}

static int pre_kvm_run(void *opaque, int vcpu)
{
    CPUState *env = cpu_single_env;

    if (env->cpu_index == 0 && wait_hack) {
	int i;

	wait_hack = 0;

	pthread_mutex_unlock(&qemu_mutex);
	for (i = 0; i < 10; ++i)
	    usleep(1000);
	pthread_mutex_lock(&qemu_mutex);
    }

    if (!kvm_irqchip_in_kernel(kvm_context))
	kvm_set_cr8(kvm_context, vcpu, cpu_get_apic_tpr(env));
    if (env->interrupt_request & CPU_INTERRUPT_EXIT)
	return 1;
    pthread_mutex_unlock(&qemu_mutex);
    return 0;
}

void kvm_load_registers(CPUState *env)
{
    if (kvm_allowed)
	kvm_arch_load_regs(env);
}

void kvm_save_registers(CPUState *env)
{
    if (kvm_allowed)
	kvm_arch_save_regs(env);
}

int kvm_cpu_exec(CPUState *env)
{
    int r;

    r = kvm_run(kvm_context, env->cpu_index);
    if (r < 0) {
        printf("kvm_run returned %d\n", r);
        exit(1);
    }

    return 0;
}

extern int vm_running;

static int has_work(CPUState *env)
{
    if (!vm_running)
	return 0;
    if (!(env->hflags & HF_HALTED_MASK))
	return 1;
    if ((env->interrupt_request & (CPU_INTERRUPT_HARD | CPU_INTERRUPT_EXIT)) &&
	(env->eflags & IF_MASK))
	return 1;
    return 0;
}

static int kvm_eat_signal(CPUState *env, int timeout)
{
    struct timespec ts;
    int r, e, ret = 0;
    siginfo_t siginfo;
    struct sigaction sa;

    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (timeout % 1000) * 1000000;
    r = sigtimedwait(&io_sigset, &siginfo, &ts);
    if (r == -1 && (errno == EAGAIN || errno == EINTR) && !timeout)
	return 0;
    e = errno;
    pthread_mutex_lock(&qemu_mutex);
    cpu_single_env = vcpu_env;
    if (r == -1 && !(errno == EAGAIN || errno == EINTR)) {
	printf("sigtimedwait: %s\n", strerror(e));
	exit(1);
    }
    if (r != -1) {
	sigaction(siginfo.si_signo, NULL, &sa);
	sa.sa_handler(siginfo.si_signo);
	ret = 1;
    }
    pthread_mutex_unlock(&qemu_mutex);

    return ret;
}


static void kvm_eat_signals(CPUState *env, int timeout)
{
    int r = 0;

    while (kvm_eat_signal(env, 0))
	r = 1;
    if (!r && timeout) {
	r = kvm_eat_signal(env, timeout);
	if (r)
	    while (kvm_eat_signal(env, 0))
		;
    }
    /*
     * we call select() even if no signal was received, to account for
     * for which there is no signal handler installed.
     */
    pthread_mutex_lock(&qemu_mutex);
    cpu_single_env = vcpu_env;
    main_loop_wait(0);
    pthread_mutex_unlock(&qemu_mutex);
}

static void kvm_main_loop_wait(CPUState *env, int timeout)
{
    pthread_mutex_unlock(&qemu_mutex);
    if (env->cpu_index == 0)
	kvm_eat_signals(env, timeout);
    else {
	if (!kvm_irqchip_in_kernel(kvm_context) &&
	    (timeout || vcpu_info[env->cpu_index].stopped)) {
	    sigset_t set;
	    int n;

	paused:
	    sigemptyset(&set);
	    sigaddset(&set, SIG_IPI);
	    sigwait(&set, &n);
	} else {
	    struct timespec ts;
	    siginfo_t siginfo;
	    sigset_t set;

	    ts.tv_sec = 0;
	    ts.tv_nsec = 0;
	    sigemptyset(&set);
	    sigaddset(&set, SIG_IPI);
	    sigtimedwait(&set, &siginfo, &ts);
	}
	if (vcpu_info[env->cpu_index].stop) {
	    vcpu_info[env->cpu_index].stop = 0;
	    vcpu_info[env->cpu_index].stopped = 1;
	    pthread_kill(vcpu_info[0].thread, SIG_IPI);
	    goto paused;
	}
    }
    pthread_mutex_lock(&qemu_mutex);
    cpu_single_env = env;
    vcpu_info[env->cpu_index].signalled = 0;
}

static int all_threads_paused(void)
{
    int i;

    for (i = 1; i < smp_cpus; ++i)
	if (vcpu_info[i].stopped)
	    return 0;
    return 1;
}

static void pause_other_threads(void)
{
    int i;

    for (i = 1; i < smp_cpus; ++i) {
	vcpu_info[i].stop = 1;
	pthread_kill(vcpu_info[i].thread, SIG_IPI);
    }
    while (!all_threads_paused())
	kvm_eat_signals(vcpu_env, 0);
}

static void resume_other_threads(void)
{
    int i;

    for (i = 1; i < smp_cpus; ++i) {
	vcpu_info[i].stop = 0;
	vcpu_info[i].stopped = 0;
	pthread_kill(vcpu_info[i].thread, SIG_IPI);
    }
}

static void kvm_vm_state_change_handler(void *context, int running)
{
    if (running)
	resume_other_threads();
    else
	pause_other_threads();
}

static void update_regs_for_sipi(CPUState *env)
{
    SegmentCache cs = env->segs[R_CS];

    kvm_arch_save_regs(env);
    env->segs[R_CS] = cs;
    env->eip = 0;
    kvm_arch_load_regs(env);
    vcpu_info[env->cpu_index].sipi_needed = 0;
    vcpu_info[env->cpu_index].init = 0;
}

static void update_regs_for_init(CPUState *env)
{
    cpu_reset(env);
    kvm_arch_load_regs(env);
}

static void setup_kernel_sigmask(CPUState *env)
{
    sigset_t set;

    sigprocmask(SIG_BLOCK, NULL, &set);
    sigdelset(&set, SIG_IPI);
    if (env->cpu_index == 0)
	sigandset(&set, &set, &io_negsigset);
    
    kvm_set_signal_mask(kvm_context, env->cpu_index, &set);
}

static int kvm_main_loop_cpu(CPUState *env)
{
    struct vcpu_info *info = &vcpu_info[env->cpu_index];

    setup_kernel_sigmask(env);
    pthread_mutex_lock(&qemu_mutex);
    cpu_single_env = env;
    while (1) {
	while (!has_work(env))
	    kvm_main_loop_wait(env, 10);
	if (env->interrupt_request & CPU_INTERRUPT_HARD)
	    env->hflags &= ~HF_HALTED_MASK;
	if (!kvm_irqchip_in_kernel(kvm_context) && info->sipi_needed)
	    update_regs_for_sipi(env);
	if (!kvm_irqchip_in_kernel(kvm_context) && info->init)
	    update_regs_for_init(env);
	if (!(env->hflags & HF_HALTED_MASK) && !info->init)
	    kvm_cpu_exec(env);
	env->interrupt_request &= ~CPU_INTERRUPT_EXIT;
	kvm_main_loop_wait(env, 0);
	if (qemu_shutdown_requested())
	    break;
	else if (qemu_powerdown_requested())
	    qemu_system_powerdown();
	else if (qemu_reset_requested()) {
	    env->interrupt_request = 0;
	    qemu_system_reset();
	    kvm_arch_load_regs(env);
	}
    }
    pthread_mutex_unlock(&qemu_mutex);
    return 0;
}

static void *ap_main_loop(void *_env)
{
    CPUState *env = _env;
    sigset_t signals;

    vcpu_env = env;
    sigfillset(&signals);
    //sigdelset(&signals, SIG_IPI);
    sigprocmask(SIG_BLOCK, &signals, NULL);
    kvm_create_vcpu(kvm_context, env->cpu_index);
    kvm_qemu_init_env(env);
    if (kvm_irqchip_in_kernel(kvm_context))
	env->hflags &= ~HF_HALTED_MASK;
    kvm_main_loop_cpu(env);
    return NULL;
}

static void kvm_add_signal(int signum)
{
    sigaddset(&io_sigset, signum);
    sigdelset(&io_negsigset, signum);
    sigprocmask(SIG_BLOCK,  &io_sigset, NULL);
}

int kvm_init_ap(void)
{
    CPUState *env = first_cpu->next_cpu;
    int i;

    qemu_add_vm_change_state_handler(kvm_vm_state_change_handler, NULL);
    sigemptyset(&io_sigset);
    sigfillset(&io_negsigset);
    kvm_add_signal(SIGIO);
    kvm_add_signal(SIGALRM);
    kvm_add_signal(SIGUSR2);
    if (!kvm_irqchip_in_kernel(kvm_context))
        kvm_add_signal(SIG_IPI);

    vcpu_env = first_cpu;
    signal(SIG_IPI, sig_ipi_handler);
    for (i = 1; i < smp_cpus; ++i) {
	pthread_create(&vcpu_info[i].thread, NULL, ap_main_loop, env);
	env = env->next_cpu;
    }
    return 0;
}

int kvm_main_loop(void)
{
    vcpu_info[0].thread = pthread_self();
    return kvm_main_loop_cpu(first_cpu);
}

static int kvm_debug(void *opaque, int vcpu)
{
    CPUState *env = cpu_single_env;

    env->exception_index = EXCP_DEBUG;
    return 1;
}

static int kvm_inb(void *opaque, uint16_t addr, uint8_t *data)
{
    *data = cpu_inb(0, addr);
    return 0;
}

static int kvm_inw(void *opaque, uint16_t addr, uint16_t *data)
{
    *data = cpu_inw(0, addr);
    return 0;
}

static int kvm_inl(void *opaque, uint16_t addr, uint32_t *data)
{
    *data = cpu_inl(0, addr);
    return 0;
}

#define PM_IO_BASE 0xb000

static int kvm_outb(void *opaque, uint16_t addr, uint8_t data)
{
    if (addr == 0xb2) {
	switch (data) {
	case 0: {
	    cpu_outb(0, 0xb3, 0);
	    break;
	}
	case 0xf0: {
	    unsigned x;

	    /* enable acpi */
	    x = cpu_inw(0, PM_IO_BASE + 4);
	    x &= ~1;
	    cpu_outw(0, PM_IO_BASE + 4, x);
	    break;
	}
	case 0xf1: {
	    unsigned x;

	    /* enable acpi */
	    x = cpu_inw(0, PM_IO_BASE + 4);
	    x |= 1;
	    cpu_outw(0, PM_IO_BASE + 4, x);
	    break;
	}
	default:
	    break;
	}
	return 0;
    }
    cpu_outb(0, addr, data);
    return 0;
}

static int kvm_outw(void *opaque, uint16_t addr, uint16_t data)
{
    cpu_outw(0, addr, data);
    return 0;
}

static int kvm_outl(void *opaque, uint16_t addr, uint32_t data)
{
    cpu_outl(0, addr, data);
    return 0;
}

static int kvm_readb(void *opaque, uint64_t addr, uint8_t *data)
{
    *data = ldub_phys(addr);
    return 0;
}
 
static int kvm_readw(void *opaque, uint64_t addr, uint16_t *data)
{
    *data = lduw_phys(addr);
    return 0;
}

static int kvm_readl(void *opaque, uint64_t addr, uint32_t *data)
{
    /* hack: Red Hat 7.1 generates some wierd accesses. */
    if (addr > 0xa0000 - 4 && addr < 0xa0000) {
	*data = 0;
	return 0;
    }

    *data = ldl_phys(addr);
    return 0;
}

static int kvm_readq(void *opaque, uint64_t addr, uint64_t *data)
{
    *data = ldq_phys(addr);
    return 0;
}

static int kvm_writeb(void *opaque, uint64_t addr, uint8_t data)
{
    stb_phys(addr, data);
    return 0;
}

static int kvm_writew(void *opaque, uint64_t addr, uint16_t data)
{
    stw_phys(addr, data);
    return 0;
}

static int kvm_writel(void *opaque, uint64_t addr, uint32_t data)
{
    stl_phys(addr, data);
    return 0;
}

static int kvm_writeq(void *opaque, uint64_t addr, uint64_t data)
{
    stq_phys(addr, data);
    return 0;
}

static int kvm_io_window(void *opaque)
{
    return 1;
}

 
static int kvm_halt(void *opaque, int vcpu)
{
    CPUState *env = cpu_single_env;

    if (!((env->interrupt_request & CPU_INTERRUPT_HARD) &&
	  (env->eflags & IF_MASK))) {
	    env->hflags |= HF_HALTED_MASK;
	    env->exception_index = EXCP_HLT;
    }

    return 1;
}

static int kvm_shutdown(void *opaque, int vcpu)
{
    qemu_system_reset_request();
    return 1;
}
 
static struct kvm_callbacks qemu_kvm_ops = {
    .debug = kvm_debug,
    .inb   = kvm_inb,
    .inw   = kvm_inw,
    .inl   = kvm_inl,
    .outb  = kvm_outb,
    .outw  = kvm_outw,
    .outl  = kvm_outl,
    .readb = kvm_readb,
    .readw = kvm_readw,
    .readl = kvm_readl,
    .readq = kvm_readq,
    .writeb = kvm_writeb,
    .writew = kvm_writew,
    .writel = kvm_writel,
    .writeq = kvm_writeq,
    .halt  = kvm_halt,
    .shutdown = kvm_shutdown,
    .io_window = kvm_io_window,
    .try_push_interrupts = try_push_interrupts,
    .post_kvm_run = post_kvm_run,
    .pre_kvm_run = pre_kvm_run,
};

int kvm_qemu_init()
{
    /* Try to initialize kvm */
    kvm_context = kvm_init(&qemu_kvm_ops, cpu_single_env);
    if (!kvm_context) {
      	return -1;
    }

    return 0;
}

int kvm_qemu_create_context(void)
{
    int r;
    if (!kvm_irqchip) {
        kvm_disable_irqchip_creation(kvm_context);
    }
    if (kvm_create(kvm_context, phys_ram_size, (void**)&phys_ram_base) < 0) {
	kvm_qemu_destroy();
	return -1;
    }
    r = kvm_arch_qemu_create_context();
    if(r <0)
	kvm_qemu_destroy();
    return 0;
}

void kvm_qemu_destroy(void)
{
    kvm_finalize(kvm_context);
}

void kvm_cpu_register_physical_memory(target_phys_addr_t start_addr,
                                      unsigned long size,
                                      unsigned long phys_offset)
{
#ifdef KVM_CAP_USER_MEMORY
    int r = 0;

    r = kvm_check_extension(kvm_context, KVM_CAP_USER_MEMORY);
    if (r) {
        if (!(phys_offset & ~TARGET_PAGE_MASK)) {
                r = kvm_is_allocated_mem(kvm_context, start_addr, size);
            if (r)
                return;
            r = kvm_is_intersecting_mem(kvm_context, start_addr);
            if (r)
                kvm_create_mem_hole(kvm_context, start_addr, size);
            r = kvm_register_userspace_phys_mem(kvm_context, start_addr,
                                                phys_ram_base + phys_offset,
                                                size, 0);
        }
        if (phys_offset & IO_MEM_ROM) {
            phys_offset &= ~IO_MEM_ROM;
            r = kvm_is_intersecting_mem(kvm_context, start_addr);
            if (r)
                kvm_create_mem_hole(kvm_context, start_addr, size);
            r = kvm_register_userspace_phys_mem(kvm_context, start_addr,
                                                phys_ram_base + phys_offset,
                                                size, 0);
        }
        if (r < 0) {
            printf("kvm_cpu_register_physical_memory: failed\n");
            exit(1);
        }
        return;
    }
#endif
    if (phys_offset & IO_MEM_ROM) {
        phys_offset &= ~IO_MEM_ROM;
        memcpy(phys_ram_base + start_addr, phys_ram_base + phys_offset, size);
    }
}

int kvm_qemu_check_extension(int ext)
{
    return kvm_check_extension(kvm_context, ext);
}

static void host_cpuid(uint32_t function, uint32_t *eax, uint32_t *ebx,
		       uint32_t *ecx, uint32_t *edx)
{
    uint32_t vec[4];

    vec[0] = function;
    asm volatile (
#ifdef __x86_64__
	 "sub $128, %%rsp \n\t"  /* skip red zone */
         "push %0;  push %%rsi \n\t"
	 "push %%rax; push %%rbx; push %%rcx; push %%rdx \n\t"
	 "mov 8*5(%%rsp), %%rsi \n\t"
	 "mov (%%rsi), %%eax \n\t"
	 "cpuid \n\t"
	 "mov %%eax, (%%rsi) \n\t"
	 "mov %%ebx, 4(%%rsi) \n\t"
	 "mov %%ecx, 8(%%rsi) \n\t"
	 "mov %%edx, 12(%%rsi) \n\t"
	 "pop %%rdx; pop %%rcx; pop %%rbx; pop %%rax \n\t"
	 "pop %%rsi; pop %0 \n\t"
	 "add $128, %%rsp"
#else
         "push %0;  push %%esi \n\t"
	 "push %%eax; push %%ebx; push %%ecx; push %%edx \n\t"
	 "mov 4*5(%%esp), %%esi \n\t"
	 "mov (%%esi), %%eax \n\t"
	 "cpuid \n\t"
	 "mov %%eax, (%%esi) \n\t"
	 "mov %%ebx, 4(%%esi) \n\t"
	 "mov %%ecx, 8(%%esi) \n\t"
	 "mov %%edx, 12(%%esi) \n\t"
	 "pop %%edx; pop %%ecx; pop %%ebx; pop %%eax \n\t"
	 "pop %%esi; pop %0 \n\t"
#endif
	 : : "rm"(vec) : "memory");
    if (eax)
	*eax = vec[0];
    if (ebx)
	*ebx = vec[1];
    if (ecx)
	*ecx = vec[2];
    if (edx)
	*edx = vec[3];
}

static void do_cpuid_ent(struct kvm_cpuid_entry *e, uint32_t function,
			 CPUState *env)
{
    env->regs[R_EAX] = function;
    qemu_kvm_cpuid_on_env(env);
    e->function = function;
    e->eax = env->regs[R_EAX];
    e->ebx = env->regs[R_EBX];
    e->ecx = env->regs[R_ECX];
    e->edx = env->regs[R_EDX];
    if (function == 0x80000001) {
	uint32_t h_eax, h_edx;
	struct utsname utsname;

	host_cpuid(function, &h_eax, NULL, NULL, &h_edx);
	uname(&utsname);
	lm_capable_kernel = strcmp(utsname.machine, "x86_64") == 0;

	// long mode
	if ((h_edx & 0x20000000) == 0 || !lm_capable_kernel)
	    e->edx &= ~0x20000000u;
	// syscall
	if ((h_edx & 0x00000800) == 0)
	    e->edx &= ~0x00000800u;
	// nx
	if ((h_edx & 0x00100000) == 0)
	    e->edx &= ~0x00100000u;
	// svm
	if (e->ecx & 4)
	    e->ecx &= ~4u;
    }
    // sysenter isn't supported on compatibility mode on AMD.  and syscall
    // isn't supported in compatibility mode on Intel.  so advertise the
    // actuall cpu, and say goodbye to migration between different vendors
    // is you use compatibility mode.
    if (function == 0) {
	uint32_t bcd[3];

	host_cpuid(0, NULL, &bcd[0], &bcd[1], &bcd[2]);
	e->ebx = bcd[0];
	e->ecx = bcd[1];
	e->edx = bcd[2];
    }
}

int kvm_qemu_init_env(CPUState *cenv)
{
    struct kvm_cpuid_entry cpuid_ent[100];
#ifdef KVM_CPUID_SIGNATURE
    struct kvm_cpuid_entry *pv_ent;
    uint32_t signature[3];
#endif
    int cpuid_nent = 0;
    CPUState copy;
    uint32_t i, limit;

    copy = *cenv;

#ifdef KVM_CPUID_SIGNATURE
    /* Paravirtualization CPUIDs */
    memcpy(signature, "KVMKVMKVM", 12);
    pv_ent = &cpuid_ent[cpuid_nent++];
    memset(pv_ent, 0, sizeof(*pv_ent));
    pv_ent->function = KVM_CPUID_SIGNATURE;
    pv_ent->eax = 0;
    pv_ent->ebx = signature[0];
    pv_ent->ecx = signature[1];
    pv_ent->edx = signature[2];

    pv_ent = &cpuid_ent[cpuid_nent++];
    memset(pv_ent, 0, sizeof(*pv_ent));
    pv_ent->function = KVM_CPUID_FEATURES;
    pv_ent->eax = 0;
#endif

    copy.regs[R_EAX] = 0;
    qemu_kvm_cpuid_on_env(&copy);
    limit = copy.regs[R_EAX];

    for (i = 0; i <= limit; ++i)
	do_cpuid_ent(&cpuid_ent[cpuid_nent++], i, &copy);

    copy.regs[R_EAX] = 0x80000000;
    qemu_kvm_cpuid_on_env(&copy);
    limit = copy.regs[R_EAX];

    for (i = 0x80000000; i <= limit; ++i)
	do_cpuid_ent(&cpuid_ent[cpuid_nent++], i, &copy);

    kvm_setup_cpuid(kvm_context, cenv->cpu_index, cpuid_nent, cpuid_ent);

    return 0;
}

int kvm_update_debugger(CPUState *env)
{
    struct kvm_debug_guest dbg;
    int i;

    dbg.enabled = 0;
    if (env->nb_breakpoints || env->singlestep_enabled) {
	dbg.enabled = 1;
	for (i = 0; i < 4 && i < env->nb_breakpoints; ++i) {
	    dbg.breakpoints[i].enabled = 1;
	    dbg.breakpoints[i].address = env->breakpoints[i];
	}
	dbg.singlestep = env->singlestep_enabled;
    }
    return kvm_guest_debug(kvm_context, env->cpu_index, &dbg);
}


/*
 * dirty pages logging
 */
/* FIXME: use unsigned long pointer instead of unsigned char */
unsigned char *kvm_dirty_bitmap = NULL;
int kvm_physical_memory_set_dirty_tracking(int enable)
{
    int r = 0;

    if (!kvm_allowed)
        return 0;

    if (enable) {
        if (!kvm_dirty_bitmap) {
            unsigned bitmap_size = BITMAP_SIZE(phys_ram_size);
            kvm_dirty_bitmap = qemu_malloc(bitmap_size);
            if (kvm_dirty_bitmap == NULL) {
                perror("Failed to allocate dirty pages bitmap");
                r=-1;
            }
            else {
                r = kvm_dirty_pages_log_enable_all(kvm_context);
            }
        }
    }
    else {
        if (kvm_dirty_bitmap) {
            r = kvm_dirty_pages_log_reset(kvm_context);
            qemu_free(kvm_dirty_bitmap);
            kvm_dirty_bitmap = NULL;
        }
    }
    return r;
}

/* get kvm's dirty pages bitmap and update qemu's */
int kvm_get_dirty_pages_log_slot(unsigned long start_addr,
                                 unsigned char *bitmap,
                                 unsigned int offset,
                                 unsigned int len)
{
    int r;
    unsigned int i, j, n=0;
    unsigned char c;
    unsigned page_number, addr, addr1;

    memset(bitmap, 0, len);
    r = kvm_get_dirty_pages(kvm_context, start_addr, bitmap);
    if (r)
        return r;

    /* 
     * bitmap-traveling is faster than memory-traveling (for addr...) 
     * especially when most of the memory is not dirty.
     */
    for (i=0; i<len; i++) {
        c = bitmap[i];
        while (c>0) {
            j = ffsl(c) - 1;
            c &= ~(1u<<j);
            page_number = i * 8 + j;
            addr1 = page_number * TARGET_PAGE_SIZE;
            addr  = offset + addr1;
            cpu_physical_memory_set_dirty(addr);
            n++;
        }
    }
    return 0;
}

/* 
 * get kvm's dirty pages bitmap and update qemu's
 * we only care about physical ram, which resides in slots 0 and 3
 */
int kvm_update_dirty_pages_log(void)
{
    int r = 0, len;

    len = BITMAP_SIZE(0xa0000);
    r =      kvm_get_dirty_pages_log_slot(0, kvm_dirty_bitmap, 0      , len);
    len = BITMAP_SIZE(phys_ram_size - 0xc0000);
    r = r || kvm_get_dirty_pages_log_slot(0xc0000, kvm_dirty_bitmap, 0xc0000, len);
    return r;
}

int kvm_get_phys_ram_page_bitmap(unsigned char *bitmap)
{
    int r=0, len, offset;
    
    len = BITMAP_SIZE(phys_ram_size);
    memset(bitmap, 0, len);

    r = kvm_get_mem_map(kvm_context, 0, bitmap);
    if (r)
        goto out;

    offset = BITMAP_SIZE(0xc0000);
    r = kvm_get_mem_map(kvm_context, 0xc0000, bitmap + offset);

 out:
    return r;
}

#ifdef KVM_CAP_IRQCHIP

int kvm_set_irq(int irq, int level)
{
    return kvm_set_irq_level(kvm_context, irq, level);
}

#endif

#endif
