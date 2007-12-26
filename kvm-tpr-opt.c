
#include "config.h"
#include "config-host.h"

#include <string.h>

#include "hw/hw.h"
#include "sysemu.h"
#include "qemu-kvm.h"
#include "cpu.h"

#include <stdio.h>

extern kvm_context_t kvm_context;

static uint64_t map_addr(struct kvm_sregs *sregs, target_ulong virt, unsigned *perms)
{
    uint64_t mask = ((1ull << 48) - 1) & ~4095ull;
    uint64_t p, pp = 7;

    p = sregs->cr3;
    if (sregs->cr4 & 0x20) {
	p &= ~31ull;
	p = ldq_phys(p + 8 * (virt >> 30));
	if (!(p & 1))
	    return -1ull;
	p &= mask;
	p = ldq_phys(p + 8 * ((virt >> 21) & 511));
	if (!(p & 1))
	    return -1ull;
	pp &= p;
	if (p & 128) {
	    p += ((virt >> 12) & 511) << 12;
	} else {
	    p &= mask;
	    p = ldq_phys(p + 8 * ((virt >> 12) & 511));
	    if (!(p & 1))
		return -1ull;
	    pp &= p;
	}
    } else {
	p &= mask;
	p = ldl_phys(p + 4 * ((virt >> 22) & 1023));
	if (!(p & 1))
	    return -1ull;
	pp &= p;
	if (p & 128) {
	    p += ((virt >> 12) & 1023) << 12;
	} else {
	    p &= mask;
	    p = ldl_phys(p + 4 * ((virt >> 12) & 1023));
	    pp &= p;
	    if (!(p & 1))
		return -1ull;
	}
    }
    if (perms)
	*perms = pp >> 1;
    p &= mask;
    return p + (virt & 4095);
}

static uint8_t read_byte_virt(CPUState *env, target_ulong virt)
{
    struct kvm_sregs sregs;

    kvm_get_sregs(kvm_context, env->cpu_index, &sregs);
    return ldub_phys(map_addr(&sregs, virt, NULL));
}

static void write_byte_virt(CPUState *env, target_ulong virt, uint8_t b)
{
    struct kvm_sregs sregs;

    kvm_get_sregs(kvm_context, env->cpu_index, &sregs);
    stb_phys(map_addr(&sregs, virt, NULL), b);
}

static uint32_t get_bios_map(CPUState *env, unsigned *perms)
{
    uint32_t v;
    struct kvm_sregs sregs;

    kvm_get_sregs(kvm_context, env->cpu_index, &sregs);

    for (v = -4096u; v != 0; v -= 4096)
	if (map_addr(&sregs, v, perms) == 0xe0000)
	    return v;
    return -1u;
}

struct vapic_bios {
    char signature[8];
    uint32_t virt_base;
    uint32_t fixup_start;
    uint32_t fixup_end;
    uint32_t vapic;
    uint32_t vapic_size;
    uint32_t vcpu_shift;
    uint32_t real_tpr;
    uint32_t set_tpr;
    uint32_t set_tpr_eax;
    uint32_t get_tpr[8];
};

static struct vapic_bios vapic_bios;

static uint32_t real_tpr;
static uint32_t bios_addr;
static uint32_t vapic_phys;
static int bios_enabled;
static uint32_t vbios_desc_phys;

void update_vbios_real_tpr()
{
    cpu_physical_memory_rw(vbios_desc_phys, (void *)&vapic_bios, sizeof vapic_bios, 0);
    vapic_bios.real_tpr = real_tpr;
    vapic_bios.vcpu_shift = 7;
    cpu_physical_memory_rw(vbios_desc_phys, (void *)&vapic_bios, sizeof vapic_bios, 1);
}

static unsigned modrm_reg(uint8_t modrm)
{
    return (modrm >> 3) & 7;
}

static int is_abs_modrm(uint8_t modrm)
{
    return (modrm & 0xc7) == 0x05;
}

static int instruction_is_ok(CPUState *env, uint64_t rip, int is_write)
{
    uint8_t b1, b2;
    unsigned addr_offset;
    uint32_t addr;
    uint64_t p;

    if ((rip & 0xf0000000) != 0x80000000 && (rip & 0xf0000000) != 0xe0000000)
	return 0;
    b1 = read_byte_virt(env, rip);
    b2 = read_byte_virt(env, rip + 1);
    switch (b1) {
    case 0xc7: /* mov imm32, r/m32 (c7/0) */
	if (modrm_reg(b2) != 0)
	    return 0;
	/* fall through */
    case 0x89: /* mov r32 to r/m32 */
    case 0x8b: /* mov r/m32 to r32 */
	if (!is_abs_modrm(b2))
	    return 0;
	addr_offset = 2;
	break;
    case 0xa1: /* mov abs to eax */
    case 0xa3: /* mov eax to abs */
	addr_offset = 1;
	break;
    default:
	return 0;
    }
    p = rip + addr_offset;
    addr = read_byte_virt(env, p++);
    addr |= read_byte_virt(env, p++) << 8;
    addr |= read_byte_virt(env, p++) << 16;
    addr |= read_byte_virt(env, p++) << 24;
    if ((addr & 0xfff) != 0x80)
	return 0;
    real_tpr = addr;
    update_vbios_real_tpr();
    return 1;
}

static int bios_is_mapped(CPUState *env, uint64_t rip)
{
    uint32_t probe;
    uint64_t phys;
    struct kvm_sregs sregs;
    unsigned perms;
    uint32_t i;
    uint32_t offset, fixup;

    if (bios_enabled)
	return 1;

    kvm_get_sregs(kvm_context, env->cpu_index, &sregs);

    probe = (rip & 0xf0000000) + 0xe0000;
    phys = map_addr(&sregs, probe, &perms);
    if (phys != 0xe0000)
	return 0;
    bios_addr = probe;
    for (i = 0; i < 64; ++i) {
	cpu_physical_memory_read(phys, (void *)&vapic_bios, sizeof(vapic_bios));
	if (memcmp(vapic_bios.signature, "kvm aPiC", 8) == 0)
	    break;
	phys += 1024;
	bios_addr += 1024;
    }
    if (i == 64)
	return 0;
    if (bios_addr == vapic_bios.virt_base)
	return 1;
    vbios_desc_phys = phys;
    for (i = vapic_bios.fixup_start; i < vapic_bios.fixup_end; i += 4) {
	offset = ldl_phys(phys + i - vapic_bios.virt_base);
	fixup = phys + offset;
	stl_phys(fixup, ldl_phys(fixup) + bios_addr - vapic_bios.virt_base);
    }
    vapic_phys = vapic_bios.vapic - vapic_bios.virt_base + phys;
    return 1;
}

static int enable_vapic(CPUState *env)
{
    struct kvm_sregs sregs;

    kvm_get_sregs(kvm_context, env->cpu_index, &sregs);
    sregs.tr.selector = 0xdb + (env->cpu_index << 8);
    kvm_set_sregs(kvm_context, env->cpu_index, &sregs);

    kvm_enable_vapic(kvm_context, env->cpu_index,
		     vapic_phys + (env->cpu_index << 7));
    return 1;
}

static void patch_call(CPUState *env, uint64_t rip, uint32_t target)
{
    uint32_t offset;

    offset = target - vapic_bios.virt_base + bios_addr - rip - 5;
    write_byte_virt(env, rip, 0xe8); /* call near */
    write_byte_virt(env, rip + 1, offset);
    write_byte_virt(env, rip + 2, offset >> 8);
    write_byte_virt(env, rip + 3, offset >> 16);
    write_byte_virt(env, rip + 4, offset >> 24);
}

static void patch_instruction(CPUState *env, uint64_t rip)
{
    uint8_t b1, b2;

    b1 = read_byte_virt(env, rip);
    b2 = read_byte_virt(env, rip + 1);
    switch (b1) {
    case 0x89: /* mov r32 to r/m32 */
	write_byte_virt(env, rip, 0x50 + modrm_reg(b2));  /* push reg */
	patch_call(env, rip + 1, vapic_bios.set_tpr);
	break;
    case 0x8b: /* mov r/m32 to r32 */
	write_byte_virt(env, rip, 0x90);
	patch_call(env, rip + 1, vapic_bios.get_tpr[modrm_reg(b2)]);
	break;
    case 0xa1: /* mov abs to eax */
	patch_call(env, rip, vapic_bios.get_tpr[0]);
	break;
    case 0xa3: /* mov eax to abs */
	patch_call(env, rip, vapic_bios.set_tpr_eax);
	break;
    case 0xc7: /* mov imm32, r/m32 (c7/0) */
	write_byte_virt(env, rip, 0x68);  /* push imm32 */
	write_byte_virt(env, rip + 1, read_byte_virt(env, rip+6));
	write_byte_virt(env, rip + 2, read_byte_virt(env, rip+7));
	write_byte_virt(env, rip + 3, read_byte_virt(env, rip+8));
	write_byte_virt(env, rip + 4, read_byte_virt(env, rip+9));
	patch_call(env, rip + 5, vapic_bios.set_tpr);
	break;
    default:
	printf("funny insn %02x %02x\n", b1, b2);
    }
}

void kvm_tpr_access_report(CPUState *env, uint64_t rip, int is_write)
{
    if (!instruction_is_ok(env, rip, is_write))
	return;
    if (!bios_is_mapped(env, rip))
	return;
    if (!enable_vapic(env))
	return;
    patch_instruction(env, rip);
}

void kvm_tpr_opt_setup(CPUState *env)
{
    if (smp_cpus > 1)
	return;
    kvm_enable_tpr_access_reporting(kvm_context, env->cpu_index);
}
