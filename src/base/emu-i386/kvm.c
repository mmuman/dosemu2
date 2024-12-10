/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 *  Emulate vm86() using KVM
 *  Started 2015, Bart Oldeman
 *  References: http://lwn.net/Articles/658511/
 *  plus example at http://lwn.net/Articles/658512/
 */

#ifdef __linux__
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <asm/kvm_para.h>

#include "kvm.h"
#include "kvmmon_offsets.h"
#include "emu.h"
#include "emu-ldt.h"
#include "cpu-emu.h"
#include "vgaemu.h"
#include "dos2linux.h"
#include "mapping.h"
#include "sig.h"

#ifndef X86_EFLAGS_FIXED
#define X86_EFLAGS_FIXED 2
#endif
#include "emudpmi.h"

#define USE_INSTREMU 1
#if USE_INSTREMU
#define USE_CMMIO 0
#else
#define USE_CMMIO 1
#endif

#define SAFE_MASK (X86_EFLAGS_CF|X86_EFLAGS_PF| \
                   X86_EFLAGS_AF|X86_EFLAGS_ZF|X86_EFLAGS_SF| \
                   X86_EFLAGS_TF|X86_EFLAGS_DF|X86_EFLAGS_OF| \
                   X86_EFLAGS_RF| \
                   X86_EFLAGS_NT|X86_EFLAGS_AC|X86_EFLAGS_ID) // 0x254dd5
#define RETURN_MASK ((SAFE_MASK | 0x28 | X86_EFLAGS_FIXED) & \
                     ~X86_EFLAGS_RF) // 0x244dff

extern char _binary_kvmmon_o_bin_end[] asm("_binary_kvmmon_o_bin_end");
extern char _binary_kvmmon_o_bin_start[] asm("_binary_kvmmon_o_bin_start");

/* V86/DPMI monitor structure to run code in V86 mode with VME enabled
   or DPMI clients inside KVM
   This contains:
   1. a TSS with
     a. ss0:esp0 set to a stack at the top of the monitor structure
        This stack contains a copy of the vm86_regs struct.
     b. An interrupt redirect bitmap copied from info->int_revectored
     c. I/O bitmap, for now set to trap all ints. Todo: sync with ioperm()
   2. A GDT with 5 entries
     0. 0 entry
     1. selector 8: flat CS
     2. selector 0x10: based SS (so the high bits of ESP are always 0,
        which avoids issues with IRET).
     3. TSS
     4. LDT
   3. An IDT with 256 (0x100) entries:
     a. 0x11 entries for CPU exceptions that can occur
     b. 0xef entries for software interrupts
   4. The stack (from 1a) above
   5. Page directory and page tables
   6. The LDT, used by DPMI code; ldt_buffer in dpmi.c points here
   7. The code pointed to by the IDT entries, from kvmmon.S, on a new page
      This just pushes the exception number, error code, and all registers
      to the stack and executes the HLT instruction which is then trapped
      by KVM.
 */

#define TSS_IOPB_SIZE (65536 / 8)
#define GDT_ENTRIES 5
#define GDT_SS (GDT_ENTRIES - 3)
#define GDT_TSS (GDT_ENTRIES - 2)
#define GDT_LDT (GDT_ENTRIES - 1)
#undef IDT_ENTRIES
#define IDT_ENTRIES 0x100

#define PG_PRESENT 1
#define PG_RW 2
#define PG_USER 4
#define PG_DC 0x10

static struct monitor {
    Task tss;                                /* 0000 */
    /* tss.esp0                                 0004 */
    /* tss.ss0                                  0008 */
    /* tss.IOmapbaseT (word)                    0066 */
    struct revectored_struct int_revectored; /* 0068 */
    unsigned char io_bitmap[TSS_IOPB_SIZE+1];/* 0088 */
    /* TSS last byte (limit)                    2088 */
    unsigned char padding0[0x2100-sizeof(Task)
		-sizeof(struct revectored_struct)
		-(TSS_IOPB_SIZE+1)];
    Descriptor gdt[GDT_ENTRIES];             /* 2100 */
    unsigned char padding1[0x2200-0x2100
	-GDT_ENTRIES*sizeof(Descriptor)];    /* 2118 */
    Gatedesc idt[IDT_ENTRIES];               /* 2200 */
    unsigned char padding2[0x3000-0x2200
	-IDT_ENTRIES*sizeof(Gatedesc)
	-sizeof(unsigned int)
	-sizeof(unsigned int)*2
	-sizeof(struct emu_fpxstate)
	-sizeof(struct vm86_regs)];          /* 2308 */
    unsigned int cr2;         /* Fault stack at 2DA0 */
    unsigned int cr3;  /* cr3 in (no TLB flush if 0) */
    struct vm86_regs regs;
    unsigned padding[1];
    struct emu_fpxstate fpstate;             /* 2e00 */
    /* 3000: page directory, 4000: page table */
    unsigned int pde[PAGE_SIZE/sizeof(unsigned int)];
    unsigned int pte[(PAGE_SIZE*PAGE_SIZE)/sizeof(unsigned int)
		     /sizeof(unsigned int)];
    Descriptor ldt[LDT_ENTRIES];             /* 404000 */
    unsigned char code[256 * 32 + PAGE_SIZE];         /* 414000 */
    /* 414000 IDT exception 0 code start
       414010 IDT exception 1 code start
       .... ....
       414ff0 IDT exception 0xff code start
       415000 IDT common code start
       415024 IDT common code end
    */
    unsigned char kvm_tss[3*PAGE_SIZE];
    unsigned char kvm_identity_map[20*PAGE_SIZE];
} *monitor;

/* map the monitor high in physical/linear address space with some
   room to spare (top - 16MB, monitor takes a little over 4MB) */
#define MONITOR_DOSADDR 0xff000000

static struct kvm_cpuid2 *cpuid;
/* fpu vme de pse tsc msr mce cx8 */
#define CPUID_FEATURES_EDX 0x1bf
static struct kvm_run *run;
static int kvmfd, vmfd, vcpufd;
static struct kvm_sregs sregs;

#if USE_CMMIO
static int cmi_offs;
#define MMIO_RING(r) (struct kvm_coalesced_mmio_ring *)((char *)run + cmi_offs * PAGE_SIZE)
#endif

#define MAXSLOT 400
static struct kvm_userspace_memory_region maps[MAXSLOT];

static int init_kvm_vcpu(void);
static void kvm_set_readonly(dosaddr_t base, dosaddr_t size);

#if !defined(DISABLE_SYSTEM_WA) || !defined(KVM_CAP_IMMEDIATE_EXIT)

/* compat functions for older complex method of immediate exit
   using a signal that is blocked outside KVM_RUN but not blocked inside it */

#include <pthread.h>

#ifndef KVM_CAP_IMMEDIATE_EXIT
#define KVM_CAP_IMMEDIATE_EXIT 136
#define immediate_exit padding1[0]
#endif

#define KVM_IMMEDIATE_EXIT_SIG (SIGRTMIN + 1)
#define KERNEL_SIGSET_T_SIZE 8

static void kvm_set_immediate_exit(int set)
{
  static int kvm_cap_immediate_exit = -1;
  if (kvm_cap_immediate_exit == -1) { // setup
    kvm_cap_immediate_exit = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT) > 0;
    if (!kvm_cap_immediate_exit) {
      struct sigaction sa;
      sigset_t sigset;
      /* KVM_SET_SIGNAL_MASK expects a kernel sigset_t which is
	 smaller than a glibc one */
      union {
	struct kvm_signal_mask mask;
	unsigned char buf[sizeof(struct kvm_signal_mask) + KERNEL_SIGSET_T_SIZE];
      } maskbuf;

      sa.sa_flags = 0;
      sigemptyset(&sa.sa_mask);
      sa.sa_handler = SIG_DFL; // never called
      sigaction(KVM_IMMEDIATE_EXIT_SIG, &sa, NULL);

      /* block KVM_IMMEDIATE_EXIT_SIG in main thread */
      sigemptyset(&sigset);
      sigaddset(&sigset, KVM_IMMEDIATE_EXIT_SIG);
      pthread_sigmask(SIG_BLOCK, &sigset, NULL);

      /* don't block any signals inside the guest */
      sigemptyset(&sigset);
      maskbuf.mask.len = KERNEL_SIGSET_T_SIZE;
      assert(sizeof(sigset) >= KERNEL_SIGSET_T_SIZE);
      memcpy(maskbuf.mask.sigset, &sigset, KERNEL_SIGSET_T_SIZE);
      if (ioctl(vcpufd, KVM_SET_SIGNAL_MASK, &maskbuf.mask) == -1) {
	perror("KVM: KVM_SET_SIGNAL_MASK");
	leavedos_main(99);
      }
    }
  }

  if (kvm_cap_immediate_exit) {
    assert(run->immediate_exit == !set);
    run->immediate_exit = set;
    return;
  }

  if (set)
    pthread_kill(pthread_self(), KVM_IMMEDIATE_EXIT_SIG);
  else {
    // need to flush the signal after KVM_RUN
    sigset_t sigset;
    assert(sigpending(&sigset) == 0 &&
	   sigismember(&sigset, KVM_IMMEDIATE_EXIT_SIG));
    sigemptyset(&sigset);
    sigaddset(&sigset, KVM_IMMEDIATE_EXIT_SIG);
    sigwaitinfo(&sigset, NULL);
  }
}

#else // function without workaround if kernels older than 4.13 not supported

static inline void kvm_set_immediate_exit(int set)
{
  assert(run->immediate_exit == !set);
  run->immediate_exit = set;
}

#endif

static void set_idt_default(dosaddr_t mon, int i)
{
    unsigned int offs = mon + offsetof(struct monitor, code) + i * 32;
    monitor->idt[i].offs_lo = offs & 0xffff;
    monitor->idt[i].offs_hi = offs >> 16;
    monitor->idt[i].seg = 0x8; // FLAT_CODE_SEL
    monitor->idt[i].type = 0xe;
    /* DPL is 0 so that software ints < 0x11 or 255 from DPMI clients will GPF.
       Exceptions are int3 (BP) and into (OF): matching the Linux kernel
       they must generate traps 3 and 4, and not GPF.
       Exceptions > 0x10 cannot be triggered with the VM's settings of CR0/CR4
       Exception 0x10 is special as it is also a common software int; in real
       DOS it can be turned off by resetting cr0.ne so it triggers IRQ13
       directly but this is impossible with KVM */
    monitor->idt[i].DPL = (i == 3 || i == 4 || i > 0x10) ? 3 : 0;
}

void kvm_set_idt_default(int i)
{
    if (i < 0x11)
        return;
    set_idt_default(sregs.tr.base, i);
}

static void set_idt(int i, uint16_t sel, uint32_t offs, int is_32, int tg)
{
    monitor->idt[i].offs_lo = offs & 0xffff;
    monitor->idt[i].offs_hi = offs >> 16;
    monitor->idt[i].seg = sel;
    monitor->idt[i].type = is_32 ? 0xe : 0x6;
    if (tg)
        monitor->idt[i].type |= 1;
    monitor->idt[i].DPL = 3;
}

void kvm_set_idt(int i, uint16_t sel, uint32_t offs, int is_32, int tg)
{
    /* don't change IDT for exceptions and special entry that interrupts
       the VM */
    if (i < 0x11)
        return;
    set_idt(i, sel, offs, is_32, tg);
}

static void kvm_set_desc(Descriptor *desc, struct kvm_segment *seg)
{
  MKBASE(desc, seg->base);
  MKLIMIT(desc, seg->limit >> (seg->g ? 12 : 0));
  desc->type = seg->type;
  desc->present = seg->present;
  desc->DPL = seg->dpl;
  desc->DB = seg->db;
  desc->S = seg->s;
  desc->gran = seg->g;
  desc->AVL = seg->avl;
  desc->unused = 0;
}

/* initialize KVM virtual machine monitor */
static void init_kvm_monitor(void)
{
  int ret, i;

  if (!cpuid)
    return;

  /* create monitor structure in memory */
  monitor = mmap_mapping_huge_page_aligned(MAPPING_SCRATCH|MAPPING_KVM,
			 sizeof(*monitor), PROT_READ | PROT_WRITE);
  /* trap all I/O instructions with GPF */
  memset(monitor->io_bitmap, 0xff, TSS_IOPB_SIZE+1);

  if (!init_kvm_vcpu()) {
    leavedos(99);
    return;
  }

  ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
  if (ret == -1) {
    perror("KVM: KVM_GET_SREGS");
    leavedos(99);
    return;
  }

  sregs.tr.base = MONITOR_DOSADDR;
  sregs.tr.limit = offsetof(struct monitor, io_bitmap) + TSS_IOPB_SIZE - 1;
  sregs.tr.selector = 0x18;
  sregs.tr.unusable = 0;
  sregs.tr.type = 0xb;
  sregs.tr.s = 0;
  sregs.tr.dpl = 0;
  sregs.tr.present = 1;
  sregs.tr.avl = 0;
  sregs.tr.l = 0;
  sregs.tr.db = 0;
  sregs.tr.g = 0;

  if (config.cpu_vm_dpmi == CPUVM_KVM)
    ldt_buffer = (unsigned char *)monitor->ldt;
  sregs.ldt.base = sregs.tr.base + offsetof(struct monitor, ldt);
  sregs.ldt.limit = LDT_ENTRIES * LDT_ENTRY_SIZE - 1;
  sregs.ldt.selector = 0x20;
  sregs.ldt.unusable = 0;
  sregs.ldt.type = 0x2;
  sregs.ldt.s = 0;
  sregs.ldt.dpl = 0;
  sregs.ldt.present = 1;
  sregs.ldt.avl = 0;
  sregs.ldt.l = 0;
  sregs.ldt.db = 0;
  sregs.ldt.g = 0;

  monitor->tss.ss0 = 0x10;
  monitor->tss.IOmapbase = offsetof(struct monitor, io_bitmap);

  // setup GDT
  sregs.gdt.base = sregs.tr.base + offsetof(struct monitor, gdt);
  sregs.gdt.limit = GDT_ENTRIES * sizeof(Descriptor) - 1;
  for (i=1; i<GDT_ENTRIES; i++) {
    monitor->gdt[i].limit_lo = 0xffff;
    monitor->gdt[i].type = 0xa;
    monitor->gdt[i].S = 1;
    monitor->gdt[i].present = 1;
    monitor->gdt[i].limit_hi = 0xf;
    monitor->gdt[i].DB = 1;
    monitor->gdt[i].gran = 1;
  }
  // based data selector (0x10), to avoid the ESP register corruption bug
  monitor->gdt[GDT_SS].type = 2;
  MKBASE(&monitor->gdt[GDT_SS], sregs.tr.base);
  /* Set TSS and LDT segments. They are not used yet, as our guest
   * does not do LTR or LLDT.
   * Note: don't forget to clear TSS-busy bit before using that. */
  kvm_set_desc(&monitor->gdt[GDT_TSS], &sregs.tr);
  kvm_set_desc(&monitor->gdt[GDT_LDT], &sregs.ldt);

  sregs.idt.base = sregs.tr.base + offsetof(struct monitor, idt);
  sregs.idt.limit = IDT_ENTRIES * sizeof(Gatedesc)-1;
  // setup IDT
  for (i=0; i<IDT_ENTRIES; i++) {
    set_idt_default(sregs.tr.base, i);
    monitor->idt[i].present = 1;
  }
  assert(kvm_mon_end - kvm_mon_start <= sizeof(monitor->code));
  memcpy(monitor->code, _binary_kvmmon_o_bin_start,
    _binary_kvmmon_o_bin_end - _binary_kvmmon_o_bin_start);

  /* setup paging */
  sregs.cr3 = sregs.tr.base + offsetof(struct monitor, pde);
  /* base PDE; others derive from this one */
  monitor->pde[0] = (sregs.tr.base + offsetof(struct monitor, pte))
    | (PG_PRESENT | PG_RW | PG_USER);
  /* exclude special regions for KVM-internal TSS and identity page */
  mmap_kvm(MAPPING_SCRATCH|MAPPING_KVM, MONITOR_DOSADDR,
	offsetof(struct monitor, kvm_tss),
	monitor, MONITOR_DOSADDR, PROT_READ | PROT_WRITE);
  mprotect_kvm(MAPPING_KVM, sregs.tr.base + offsetof(struct monitor, code),
	       sizeof(monitor->code), PROT_READ | KVM_PROT_EXEC);

  sregs.cr0 |= X86_CR0_PE | X86_CR0_PG | X86_CR0_NE | X86_CR0_ET;
  sregs.cr4 |= X86_CR4_VME;
  if (config.umip)
    sregs.cr4 |= X86_CR4_UMIP;

  /* setup registers to point to VM86 monitor */
  sregs.cs.base = 0;
  sregs.cs.limit = 0xffffffff;
  sregs.cs.selector = 0x8;
  sregs.cs.db = 1;
  sregs.cs.g = 1;

  sregs.ss.base = sregs.tr.base;
  sregs.ss.limit = 0xffffffff;
  sregs.ss.selector = 0x10;
  sregs.ss.db = 1;
  sregs.ss.g = 1;

  if (config.cpu_vm == CPUVM_KVM)
    dbug_printf("Using V86 mode inside KVM\n");
}

/* Initialize KVM and memory mappings */
static int init_kvm_vcpu(void)
{
  int ret, mmap_size;

  /* this call is only there to shut up the kernel saying
     "KVM_SET_TSS_ADDR need to be called before entering vcpu"
     this is only really needed if the vcpu is started in real mode and
     the kernel needs to emulate that using V86 mode, as is necessary
     on Nehalem and earlier Intel CPUs */
  ret = ioctl(vmfd, KVM_SET_TSS_ADDR,
	      MONITOR_DOSADDR + offsetof(struct monitor, kvm_tss));
  if (ret == -1) {
    perror("KVM: KVM_SET_TSS_ADDR\n");
    return 0;
  }

  uint64_t addr = MONITOR_DOSADDR + offsetof(struct monitor, kvm_identity_map);
  ret = ioctl(vmfd, KVM_SET_IDENTITY_MAP_ADDR, &addr);
  if (ret == -1) {
    perror("KVM: KVM_SET_IDENTITY_MAP_ADDR\n");
    return 0;
  }

  vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)0);
  if (vcpufd == -1) {
    perror("KVM: KVM_CREATE_VCPU");
    return 0;
  }

  assert(cpuid);
  /* use host CPUID, adjust signature */
  for (unsigned int i = 0; i < cpuid->nent; i++) {
    struct kvm_cpuid_entry2 *entry = &cpuid->entries[i];
    if (entry->function == KVM_CPUID_SIGNATURE) {
      entry->eax = KVM_CPUID_FEATURES;
      entry->ebx = 0x4b4d564b; // KVMK
      entry->ecx = 0x564b4d56; // VMKV
      entry->edx = 0x4d;       // M
    }
  }
  ret = ioctl(vcpufd, KVM_SET_CPUID2, cpuid);
  if (ret == -1) {
    perror("KVM: KVM_SET_CPUID2");
    return 0;
  }

  mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);
  if (mmap_size == -1) {
    perror("KVM: KVM_GET_VCPU_MMAP_SIZE");
    return 0;
  }
  if (mmap_size < sizeof(*run)) {
    error("KVM: KVM_GET_VCPU_MMAP_SIZE unexpectedly small\n");
    return 0;
  }
  run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
  if (run == MAP_FAILED) {
    perror("KVM: mmap vcpu");
    return 0;
  }
  run->exit_reason = KVM_EXIT_INTR;
  return 1;
}

int init_kvm_cpu(void)
{
  int ret;
  int nent = 512;

  kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvmfd == -1) {
    error("KVM: error opening /dev/kvm: %s\n", strerror(errno));
    return 0;
  }

#if defined(KVM_CAP_SYNC_MMU) && defined(KVM_CAP_SET_IDENTITY_MAP_ADDR) && \
  defined(KVM_CAP_SET_TSS_ADDR) && defined(KVM_CAP_XSAVE) && \
  defined(KVM_CAP_IMMEDIATE_EXIT) && defined(KVM_CAP_COALESCED_MMIO)
  /* SYNC_MMU is needed because we map shm behind KVM's back */
  ret = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_SYNC_MMU);
  if (ret <= 0) {
    error("KVM: SYNC_MMU unsupported %x\n", ret);
    goto errcap;
  }
#if USE_CMMIO
  ret = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_COALESCED_MMIO);
  if (ret <= 0) {
    error("KVM: COALESCED_MMIO unsupported %x\n", ret);
    goto errcap;
  }
  cmi_offs = ret;
#endif
  ret = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_SET_IDENTITY_MAP_ADDR);
  if (ret <= 0) {
    error("KVM: SET_IDENTITY_MAP_ADDR unsupported %x\n", ret);
    goto errcap;
  }
  ret = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_SET_TSS_ADDR);
  if (ret <= 0) {
    error("KVM: SET_TSS_ADDR unsupported %x\n", ret);
    goto errcap;
  }
  ret = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE);
  if (ret <= 0) {
    error("KVM: XSAVE unsupported %x\n", ret);
    goto errcap;
  }
  ret = ioctl(kvmfd, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT);
#ifndef KVM_IMMEDIATE_EXIT_SIG
  if (ret <= 0) {
    error("KVM: IMMEDIATE_EXIT unsupported %x\n", ret);
    goto errcap;
  }
#endif
#else
  error("kernel is too old, KVM unsupported\n");
  goto errcap;
#endif

  vmfd = ioctl(kvmfd, KVM_CREATE_VM, (unsigned long)0);
  if (vmfd == -1) {
    warn("KVM: KVM_CREATE_VM: %s\n", strerror(errno));
    return 0;
  }

  cpuid = malloc(sizeof(*cpuid) + nent * sizeof(cpuid->entries[0]));
  memset(cpuid, 0, sizeof(*cpuid) + nent * sizeof(cpuid->entries[0]));	// valgrind
  cpuid->nent = nent;
  ret = ioctl(kvmfd, KVM_GET_SUPPORTED_CPUID, cpuid);
  if (ret == -1) {
    perror("KVM: KVM_GET_SUPPORTED_CPUID");
    goto err;
  }
  if ((cpuid->entries[1].edx & CPUID_FEATURES_EDX) != CPUID_FEATURES_EDX) {
    error("KVM: unsupported features, need %x got %x\n",
        CPUID_FEATURES_EDX, cpuid->entries[1].edx);
    goto err;
  }

  init_kvm_monitor();
  return 1;

err:
  close(vmfd);
  free(cpuid);
  cpuid = NULL;
errcap:
  close(kvmfd);
  return 0;
}

static struct kvm_userspace_memory_region *
kvm_get_memory_region(dosaddr_t dosaddr, dosaddr_t size)
{
  int slot;
  struct kvm_userspace_memory_region *p = &maps[0];
  struct kvm_userspace_memory_region *ret = NULL;

  for (slot = 0; slot < MAXSLOT; slot++, p++) {
    if (p->guest_phys_addr <= dosaddr &&
	dosaddr + size <= p->guest_phys_addr + p->memory_size) {
      ret = p;
      break;
    }
  }
  /* if we are on full kvm then there should be no missing slots */
  if (config.cpu_vm == CPUVM_KVM && config.cpu_vm_dpmi == CPUVM_KVM)
    assert(slot < MAXSLOT);
  return ret;
}

static void set_kvm_memory_region(struct kvm_userspace_memory_region *region)
{
  int ret;
  Q_printf("KVM: map slot=%d flags=%d dosaddr=0x%08llx size=0x%08llx unixaddr=0x%llx\n",
	   region->slot, region->flags, region->guest_phys_addr,
	   region->memory_size, region->userspace_addr);
  ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, region);
  if (ret == -1) {
    perror("KVM: KVM_SET_USER_MEMORY_REGION");
    leavedos_main(99);
  }
}

void set_kvm_memory_regions(void)
{
  int slot;

  if (!cpuid)
    return;

  for (slot = 0; slot < MAXSLOT; slot++) {
    struct kvm_userspace_memory_region *p = &maps[slot];
    if (p->memory_size != 0)
      set_kvm_memory_region(p);
  }
}

static void mmap_kvm_no_overlap(unsigned targ, void *addr, size_t mapsize, int flags)
{
  struct kvm_userspace_memory_region *region;
  int slot;

  if (config.cpu_vm_dpmi != CPUVM_KVM && addr != monitor) {
    if (targ >= LOWMEM_SIZE + HMASIZE)
      return;
    if (targ + mapsize > LOWMEM_SIZE + HMASIZE)
      mapsize = LOWMEM_SIZE + HMASIZE - targ;
    if (mapsize == 0)
      return;
  }

  for (slot = 0; slot < MAXSLOT; slot++)
    if (maps[slot].memory_size == 0) break;

  if (slot == MAXSLOT) {
    error("KVM: insufficient number of memory slots %i\n", MAXSLOT);
    leavedos_main(99);
  }

  region = &maps[slot];
  region->slot = slot;
  /* NOTE: KVM guest does not have the mem_base offset in LDT
   * because we can do the same with EPT, keeping guest mem zero-based. */
  region->guest_phys_addr = targ;
  region->userspace_addr = (uintptr_t)addr;
  region->memory_size = mapsize;
  region->flags = flags;
  Q_printf("KVM: mapped guest %#x to host addr %p, size=%zx, LOG_DIRTY=%d\n",
	   targ, addr, mapsize, flags == KVM_MEM_LOG_DIRTY_PAGES ? 1 : 0);
  /* NOTE: the actual EPT update is delayed to set_kvm_memory_regions */
}

static void do_munmap_kvm(dosaddr_t targ, size_t mapsize)
{
  /* unmaps KVM regions from targ to targ+mapsize, taking care of overlaps
     NOTE: the actual EPT update is delayed to set_kvm_memory_regions */
  int slot;
  for (slot = 0; slot < MAXSLOT; slot++) {
    struct kvm_userspace_memory_region *region = &maps[slot];
    size_t sz = region->memory_size;
    unsigned gpa = region->guest_phys_addr;
    if (sz > 0 && targ + mapsize > gpa && targ < gpa + sz) {
      /* overlap: first  unmap this mapping */
      region->memory_size = 0;
      /* may need to remap head or tail */
      if (gpa < targ) {
	region->memory_size = targ - gpa;
      }
      if (gpa + sz > targ + mapsize) {
	mmap_kvm_no_overlap(targ + mapsize,
			    (void *)((uintptr_t)region->userspace_addr +
				     targ + mapsize - gpa),
			    gpa + sz - (targ + mapsize), region->flags);
      }
    }
  }
}

void mmap_kvm(int cap, unsigned phys_addr, size_t mapsize, void *addr, dosaddr_t targ, int protect)
{
  size_t pagesize = sysconf(_SC_PAGESIZE);
  unsigned int start = targ / pagesize;
  unsigned int end = start + mapsize / pagesize;
  unsigned int page;

  assert(cap & (MAPPING_INIT_LOWRAM|MAPPING_LOWMEM|MAPPING_KVM|MAPPING_VGAEMU));
  /* with KVM we need to manually remove/shrink existing mappings */
  do_munmap_kvm(phys_addr, mapsize);
  mmap_kvm_no_overlap(phys_addr, addr, mapsize, 0);
  /* monitor dirty pages on regular low ram for JIT */
  if ((cap & MAPPING_LOWMEM) && IS_EMU_JIT())
    kvm_set_dirty_log(phys_addr, mapsize);
  for (page = start; page < end; page++, phys_addr += pagesize) {
    int pde_entry = page >> 10;
    if (monitor->pde[pde_entry] == 0)
      monitor->pde[pde_entry] = monitor->pde[0] + pde_entry*pagesize;
    monitor->pte[page] = phys_addr;
  }
  mprotect_kvm(cap, targ, mapsize, protect);
}

void mprotect_kvm(int cap, dosaddr_t targ, size_t mapsize, int protect)
{
  size_t pagesize = sysconf(_SC_PAGESIZE);
  unsigned int start = targ / pagesize;
  unsigned int end = start + mapsize / pagesize;
  unsigned int page;
  struct kvm_userspace_memory_region *p;

  if (!(cap & (MAPPING_LOWMEM|MAPPING_EMS|MAPPING_HMA|
	       MAPPING_DPMI|MAPPING_VGAEMU|MAPPING_KVM|MAPPING_CPUEMU|
	       MAPPING_EXTMEM))) return;
  if (memcheck_is_rom(targ)) {
    kvm_set_readonly(targ, mapsize);
    return;
  }
  p = kvm_get_memory_region(monitor->pte[start] & _PAGE_MASK, PAGE_SIZE);
  if (!p) return;

  /* never apply read and write protections to regions with dirty logging or
     phys MMIO and r/o */
  if (!(protect & PROT_WRITE) &&
      (p->flags & (KVM_MEM_LOG_DIRTY_PAGES|KVM_MEM_READONLY)))
    return;

  if (monitor == NULL) return;

  Q_printf("KVM: protecting %x:%zx with prot %x\n", targ, mapsize, protect);

  for (page = start; page < end; page++) {
    monitor->pte[page] &= _PAGE_MASK;
    if (protect & PROT_WRITE)
      monitor->pte[page] |= PG_PRESENT | PG_RW | PG_USER;
    else if (protect & PROT_READ)
      monitor->pte[page] |= PG_PRESENT | PG_USER;
    if (cap & MAPPING_KVM)
      monitor->pte[page] &= ~PG_USER;
  }
  monitor->cr3 = sregs.cr3; /* Force TLB flush */
}

static void kvm_set_readonly(dosaddr_t base, dosaddr_t size)
{
  struct kvm_userspace_memory_region *p = kvm_get_memory_region(base, size);
  void *addr = (void *)((uintptr_t)(p->userspace_addr +
				    (base - p->guest_phys_addr)));
  do_munmap_kvm(base, size);
  mmap_kvm_no_overlap(base, addr, size, KVM_MEM_READONLY);
}

void kvm_set_mmio(dosaddr_t base, dosaddr_t size, int on)
{
  struct kvm_userspace_memory_region *p = kvm_get_memory_region(base, size);
#if USE_CMMIO
  struct kvm_coalesced_mmio_zone mmz = {};
  int ret;
#endif

  assert(p->flags & KVM_MEM_LOG_DIRTY_PAGES);
  if (on == (p->flags == KVM_MEM_LOG_DIRTY_PAGES)) {
    uint64_t region_size = p->memory_size;
    p->flags = KVM_MEM_LOG_DIRTY_PAGES;
#if USE_CMMIO
    mmz.addr = p->guest_phys_addr;
    mmz.size = region_size;
#endif
    if (on) {
      p->memory_size = 0;  // remove region thus disabling dirty page logging
      p->flags |= KVM_MEM_READONLY;
#if USE_CMMIO
      ret = ioctl(vmfd, KVM_REGISTER_COALESCED_MMIO, &mmz);
      if (ret == -1) {
        perror("KVM: KVM_REGISTER_COALESCED_MMIO");
        leavedos_main(99);
      }
#endif
    } else {
#if USE_CMMIO
      ret = ioctl(vmfd, KVM_UNREGISTER_COALESCED_MMIO, &mmz);
      if (ret == -1) {
        perror("KVM: KVM_UNREGISTER_COALESCED_MMIO");
        leavedos_main(99);
      }
#endif
    }
    set_kvm_memory_region(p);
    p->memory_size = region_size;
  }
}

/* Enable dirty logging from base to base+size.
 * This will not change the KVM-phys->host user space mapping itself but due
 * to the way KVM works the memory slot typically needs to be split in 3 parts:
 * 1. part without dirty log 2. part with dirty log 3. part without dirty log.
 */
void kvm_set_dirty_log(dosaddr_t base, dosaddr_t size)
{
  struct kvm_userspace_memory_region *p = kvm_get_memory_region(base, size);
  void *addr = (void *)((uintptr_t)(p->userspace_addr +
				    (base - p->guest_phys_addr)));
  do_munmap_kvm(base, size);
  mmap_kvm_no_overlap(base, addr, size, KVM_MEM_LOG_DIRTY_PAGES);
}

/* get dirty bitmap for memory region containing base.
 * If base is not at the start of that region, the bitmap is shifted.
 */
void kvm_get_dirty_map(dosaddr_t base, unsigned char *bitmap)
{
  size_t bitmap_size;
  struct kvm_dirty_log dirty_log = {0};
  struct kvm_userspace_memory_region *p =
    kvm_get_memory_region(base, PAGE_SIZE);

  assert(p->flags & KVM_MEM_LOG_DIRTY_PAGES);
  dirty_log.slot = p->slot;
  dirty_log.dirty_bitmap = bitmap;
  ioctl(vmfd, KVM_GET_DIRTY_LOG, &dirty_log);
  bitmap_size = ((p->memory_size >> PAGE_SHIFT)+CHAR_BIT-1) / CHAR_BIT;
  if (p->guest_phys_addr < base) {
    int offset = ((base - p->guest_phys_addr) >> PAGE_SHIFT) / CHAR_BIT;
    memmove(bitmap, bitmap + offset, bitmap_size - offset);
  }
}

/* This function works like handle_vm86_fault in the Linux kernel,
   except:
   * since we use VME we only need to handle
     PUSHFD, POPFD, IRETD always
     POPF, IRET only if it sets TF or IF with VIP set
     STI only if VIP is set and VIF was not set
     INT only if it is revectored
   * The Linux kernel splits the CPU flags into on-CPU flags and
     flags (VFLAGS) IOPL, NT, AC, and ID that are kept on the stack.
     Here all those flags are merged into on-CPU flags, with the
     exception of IOPL. IOPL is always set to 0 on the CPU,
     and to 3 on the stack with PUSHF
*/
static int kvm_handle_vm86_fault(struct vm86_regs *regs, unsigned int cpu_type)
{
  unsigned char opcode;
  int data32 = 0, pref_done = 0;
  unsigned int csp = regs->cs << 4;
  unsigned int ssp = regs->ss << 4;
  unsigned short ip = regs->eip & 0xffff;
  unsigned short sp = regs->esp & 0xffff;
  unsigned int orig_flags = regs->eflags;
  int ret = -1;

  do {
    switch (opcode = popb(csp, ip)) {
    case 0x66:      /* 32-bit data */     data32 = 1; break;
    case 0x67:      /* 32-bit address */  break;
    case 0x2e:      /* CS */              break;
    case 0x3e:      /* DS */              break;
    case 0x26:      /* ES */              break;
    case 0x36:      /* SS */              break;
    case 0x65:      /* GS */              break;
    case 0x64:      /* FS */              break;
    case 0xf2:      /* repnz */           break;
    case 0xf3:      /* rep */             break;
    default: pref_done = 1;
    }
  } while (!pref_done);

  switch (opcode) {

  case 0x9c: { /* only pushfd faults with VME */
    unsigned int flags = regs->eflags & RETURN_MASK;
    if (regs->eflags & X86_EFLAGS_VIF)
      flags |= X86_EFLAGS_IF;
    flags |= X86_EFLAGS_IOPL;
    if (data32)
      pushl(ssp, sp, flags);
    else
      pushw(ssp, sp, flags);
    break;
  }

  case 0xcd: { /* int xx */
    int intno = popb(csp, ip);
    ret = VM86_INTx + (intno << 8);
    break;
  }

  case 0xcf: /* iret */
    if (data32) {
      ip = popl(ssp, sp);
      regs->cs = popl(ssp, sp);
    } else {
      ip = popw(ssp, sp);
      regs->cs = popw(ssp, sp);
    }
    /* fall through into popf */
  case 0x9d: { /* popf */
    unsigned int newflags;
    if (data32) {
      newflags = popl(ssp, sp);
      if (cpu_type >= CPU_286 && cpu_type <= CPU_486) {
	newflags &= ~X86_EFLAGS_ID;
	if (cpu_type < CPU_486)
	  newflags &= ~X86_EFLAGS_AC;
      }
      regs->eflags &= ~SAFE_MASK;
    } else {
      /* must have VIP or TF set in VME, otherwise does not trap */
      newflags = popw(ssp, sp);
      regs->eflags &= ~(SAFE_MASK & 0xffff);
    }
    regs->eflags |= newflags & SAFE_MASK;
    if (newflags & X86_EFLAGS_IF) {
      regs->eflags |= X86_EFLAGS_VIF;
      if (!(orig_flags & X86_EFLAGS_TF) && (orig_flags & X86_EFLAGS_VIP))
        ret = VM86_STI;
    } else {
      regs->eflags &= ~X86_EFLAGS_VIF;
    }
    break;
  }

  case 0xfa: /* CLI (non-VME) */
    regs->eflags &= ~X86_EFLAGS_VIF;
    break;

  case 0xfb: /* STI */
    /* must have VIP set in VME, otherwise does not trap */
    regs->eflags |= X86_EFLAGS_VIF;
    ret = VM86_STI;
    break;

  default:
    return VM86_UNKNOWN;
  }

  regs->esp = (regs->esp & 0xffff0000) | sp;
  regs->eip = (regs->eip & 0xffff0000) | ip;
  if (ret != -1)
    return ret;
  if (orig_flags & X86_EFLAGS_TF)
    return VM86_TRAP + (1 << 8);
  return ret;
}

static void set_vm86_seg(struct kvm_segment *seg, unsigned selector)
{
  seg->selector = selector;
  seg->base = selector << 4;
  seg->limit = 0xffff;
  seg->type = 3;
  seg->present = 1;
  seg->dpl = 3;
  seg->db = 0;
  seg->s = 1;
  seg->g = 0;
  seg->avl = 0;
  seg->unusable = 0;
}

static void set_ldt_seg(struct kvm_segment *seg, unsigned selector)
{
  Descriptor *desc = &monitor->ldt[selector >> 3];
  desc->type |= 1;  /* force the "accessed" bit in LDT before access */
  seg->selector = selector;
  seg->base = DT_BASE(desc);
  seg->limit = DT_LIMIT(desc);
  if (desc->gran) seg->limit = (seg->limit << 12) | 0xfff;
  seg->type = desc->type;
  seg->present = desc->present;
  seg->dpl = desc->DPL;
  seg->db = desc->DB;
  seg->s = desc->S;
  seg->g = desc->gran;
  seg->avl = desc->AVL;
  seg->unusable = !desc->present;
}

void kvm_update_fpu(void)
{
  struct kvm_fpu fpu = {};
  int ret;

  memcpy(fpu.fpr, vm86_fpu_state.st, sizeof(vm86_fpu_state.st));
  fpu.fcw = vm86_fpu_state.cwd;
  fpu.fsw = vm86_fpu_state.swd;
  fpu.ftwx = vm86_fpu_state.ftw;
  fpu.last_opcode = vm86_fpu_state.fop;
  fpu.last_ip = vm86_fpu_state.fip;
  fpu.last_dp = vm86_fpu_state.fdp;
  memcpy(fpu.xmm, vm86_fpu_state.xmm, sizeof(vm86_fpu_state.xmm));
  fpu.mxcsr = vm86_fpu_state.mxcsr;
  ret = ioctl(vcpufd, KVM_SET_FPU, &fpu);
  if (ret == -1) {
    perror("KVM: KVM_SET_FPU");
    leavedos_main(99);
  }
}

void kvm_get_fpu(void)
{
  struct kvm_fpu fpu;
  int ret = ioctl(vcpufd, KVM_GET_FPU, &fpu);
  if (ret == -1) {
    perror("KVM: KVM_GET_FPU");
    leavedos_main(99);
  }
  memcpy(vm86_fpu_state.st, fpu.fpr, sizeof(vm86_fpu_state.st));
  vm86_fpu_state.cwd = fpu.fcw;
  vm86_fpu_state.swd = fpu.fsw;
  vm86_fpu_state.ftw = fpu.ftwx;
  vm86_fpu_state.fop = fpu.last_opcode;
  vm86_fpu_state.fip = fpu.last_ip;
  vm86_fpu_state.fcs = 0;
  vm86_fpu_state.fdp = fpu.last_dp;
  vm86_fpu_state.fds = 0;
  memcpy(vm86_fpu_state.xmm, fpu.xmm, sizeof(vm86_fpu_state.xmm));
  vm86_fpu_state.mxcsr = fpu.mxcsr;
}

void kvm_enter(int pm)
{
  kvm_update_fpu();
}

void kvm_leave(int pm)
{
  kvm_get_fpu();

  /* collect and invalidate all touched low dirty pages with JIT code */
  if (IS_EMU_JIT()) {
    int slot;
    struct kvm_userspace_memory_region *p = &maps[0];
    for (slot = 0; slot < MAXSLOT; slot++, p++)
      if (p->memory_size &&
	  p->guest_phys_addr + p->memory_size <= LOWMEM_SIZE+HMASIZE &&
	  (p->flags & KVM_MEM_LOG_DIRTY_PAGES) &&
	  memcheck_is_system_ram(p->guest_phys_addr)) {
	unsigned char bitmap[(LOWMEM_SIZE+HMASIZE)/CHAR_BIT];
	int i;
	kvm_get_dirty_map(p->guest_phys_addr, bitmap);
	for (i = 0; i < p->memory_size >> PAGE_SHIFT; i++)
	  if (test_bit(i, bitmap))
	    e_invalidate_page_full(p->guest_phys_addr + (i << PAGE_SHIFT));
      }
  }
}

static int kvm_post_run(struct vm86_regs *regs, struct kvm_regs *kregs)
{
  int ret = ioctl(vcpufd, KVM_GET_REGS, kregs);
  if (ret == -1) {
    perror("KVM: KVM_GET_REGS");
    leavedos_main(99);
  }
  ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
  if (ret == -1) {
    perror("KVM: KVM_GET_SREGS");
    leavedos_main(99);
  }
  /* don't interrupt GDT code */
  if (!(kregs->rflags & X86_EFLAGS_VM) && !(sregs.cs.selector & 4)) {
    g_printf("KVM: interrupt in GDT code, resuming\n");
    return 0;
  }
  if (!run->ready_for_interrupt_injection &&
      (kregs->rflags & (X86_EFLAGS_IF | X86_EFLAGS_VIF))) {
    g_printf("KVM: not ready for injection on ring3\n");
    return 0;
  }

  regs->eax = kregs->rax;
  regs->ebx = kregs->rbx;
  regs->ecx = kregs->rcx;
  regs->edx = kregs->rdx;
  regs->esi = kregs->rsi;
  regs->edi = kregs->rdi;
  regs->ebp = kregs->rbp;
  regs->esp = kregs->rsp;
  regs->eip = kregs->rip;
  regs->eflags = kregs->rflags;

  regs->cs = sregs.cs.selector;
  regs->ss = sregs.ss.selector;
  if (kregs->rflags & X86_EFLAGS_VM) {
    regs->ds = sregs.ds.selector;
    regs->es = sregs.es.selector;
    regs->fs = sregs.fs.selector;
    regs->gs = sregs.gs.selector;
  } else {
    regs->__null_ds = sregs.ds.selector;
    regs->__null_es = sregs.es.selector;
    regs->__null_fs = sregs.fs.selector;
    regs->__null_gs = sregs.gs.selector;
  }
  return 1;
}

#if USE_CMMIO
static void process_pending_mmio(void)
{
  struct kvm_coalesced_mmio_ring *mr = MMIO_RING(run);
  while (mr->first != mr->last) {
    struct kvm_coalesced_mmio *cmi = &mr->coalesced_mmio[mr->first++];
//    mr->first %= KVM_COALESCED_MMIO_MAX;
    switch(cmi->len) {
    case 1: write_byte(cmi->phys_addr, cmi->data[0]); break;
    case 2: write_word(cmi->phys_addr, *(uint16_t*)cmi->data); break;
    case 4: write_dword(cmi->phys_addr, *(uint32_t*)cmi->data); break;
    case 8: write_qword(cmi->phys_addr, *(uint64_t*)cmi->data); break;
    }
  }
  mr->first = 0;
  mr->last = 0;
}
#endif

static void do_mmio(void)
{
  dosaddr_t addr = (dosaddr_t)run->mmio.phys_addr;
  unsigned char *data = run->mmio.data;
  if (run->mmio.is_write) {
    switch(run->mmio.len) {
    case 1: write_byte(addr, data[0]); break;
    case 2: write_word(addr, *(uint16_t*)data); break;
    case 4: write_dword(addr, *(uint32_t*)data); break;
    case 8: write_qword(addr, *(uint64_t*)data); break;
    }
  } else {
    switch(run->mmio.len) {
    case 1: data[0] = read_byte(addr); break;
    case 2: *(uint16_t*)data = read_word(addr); break;
    case 4: *(uint32_t*)data = read_dword(addr); break;
    case 8: *(uint64_t*)data = read_qword(addr); break;
    }
  }
}

#if USE_INSTREMU
static void do_exit_mmio(void)
{
  int ret;

  /* from the KVM api.txt: "the corresponding operations are complete
     (and guest state is consistent) only after userspace has re-entered
     the kernel with KVM_RUN. The kernel side will first finish
     incomplete operations and then check for pending signals." */
  kvm_set_immediate_exit(1);
  do {
    do_mmio();
    ret = ioctl(vcpufd, KVM_RUN, NULL);
    /* read-modify-write instructions give two KVM_EXIT_MMIO
       exits in a row before the signal exit */
  } while (ret == 0 && run->exit_reason == KVM_EXIT_MMIO);
  assert(ret == -1 && errno == EINTR);
  kvm_set_immediate_exit(0);
}
#endif

/* Inner loop for KVM, runs until HLT or signal */
static unsigned int kvm_run(void)
{
  unsigned int exit_reason = 0;
  struct kvm_regs kregs = {};
  static struct vm86_regs saved_regs;
  struct vm86_regs *regs = &monitor->regs;

  if (run->exit_reason != KVM_EXIT_HLT &&
      memcmp(regs, &saved_regs, sizeof(*regs))) {
    /* Only set registers if changes happened, usually
       this means a hardware interrupt or sometimes
       a callback, and also for the very first call to boot */
    int ret;

    kregs.rax = regs->eax;
    kregs.rbx = regs->ebx;
    kregs.rcx = regs->ecx;
    kregs.rdx = regs->edx;
    kregs.rsi = regs->esi;
    kregs.rdi = regs->edi;
    kregs.rbp = regs->ebp;
    kregs.rsp = regs->esp;
    kregs.rip = regs->eip;
    kregs.rflags = regs->eflags;
    ret = ioctl(vcpufd, KVM_SET_REGS, &kregs);
    if (ret == -1) {
      perror("KVM: KVM_SET_REGS");
      leavedos_main(99);
    }

    if (regs->eflags & X86_EFLAGS_VM) {
      set_vm86_seg(&sregs.cs, regs->cs);
      set_vm86_seg(&sregs.ds, regs->ds);
      set_vm86_seg(&sregs.es, regs->es);
      set_vm86_seg(&sregs.fs, regs->fs);
      set_vm86_seg(&sregs.gs, regs->gs);
      set_vm86_seg(&sregs.ss, regs->ss);
    } else {
      set_ldt_seg(&sregs.cs, regs->cs);
      set_ldt_seg(&sregs.ds, regs->__null_ds);
      set_ldt_seg(&sregs.es, regs->__null_es);
      set_ldt_seg(&sregs.fs, regs->__null_fs);
      set_ldt_seg(&sregs.gs, regs->__null_gs);
      set_ldt_seg(&sregs.ss, regs->ss);
    }
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
      perror("KVM: KVM_SET_SREGS");
      leavedos_main(99);
    }
  }

  while (!exit_reason) {
    int ret = ioctl(vcpufd, KVM_RUN, NULL);
    int errn = errno;

    /* KVM should only exit for four reasons:
       1. KVM_EXIT_HLT: at the hlt in kvmmon.S following an exception.
          In this case the registers are pushed on and popped from the stack.
       2. KVM_EXIT_INTR: (with ret==-1) after a signal. In this case we
          must restore and save registers using ioctls.
       3. KVM_EXIT_IRQ_WINDOW_OPEN: if it is not possible to inject interrupts
          (or in our case properly interrupt using reason 2)
          KVM is re-entered asking it to exit when interrupt injection is
          possible, then it exits with this code. This only happens if a signal
          occurs during execution of the monitor code in kvmmon.S.
       4. KVM_EXIT_MMIO: when attempting to write to ROM or r/w from/to MMIO
    */
    if (ret != 0 && ret != -1)
      error("KVM: strange return %i, errno=%i\n", ret, errn);
    if (ret == -1 && errn == EINTR) {
      if (!kvm_post_run(regs, &kregs))
        continue;
      saved_regs = *regs;
      exit_reason = KVM_EXIT_INTR;
      break;
    } else if (ret != 0) {
      error("KVM: KVM_RUN failed: %s\n", strerror(errn));
      leavedos_main(99);
    }

#if USE_CMMIO
    process_pending_mmio();
#endif

    switch (run->exit_reason) {
    case KVM_EXIT_HLT:
      exit_reason = KVM_EXIT_HLT;
      break;
    case KVM_EXIT_MMIO:
      /* for ROM: simply ignore the write and continue */
      if (memcheck_is_rom(run->mmio.phys_addr))
	break;

      /* with instremu always exit on MMIO */
#if !USE_INSTREMU
      /* Note: do not exit even for VIP, because in this case the
       * interrupt window is not open, or we wouldn't be here except
       * perhaps for the STI hold-off case. */
      do_mmio();
      /* go to next iteration */
#else
      do_exit_mmio();
      /* going to emulate some instructions */
      if (!kvm_post_run(regs, &kregs))
	break;
      saved_regs = *regs;
      exit_reason = KVM_EXIT_MMIO;
#endif
      break;
    case KVM_EXIT_IRQ_WINDOW_OPEN:
      run->request_interrupt_window = !run->ready_for_interrupt_injection;
      if (run->request_interrupt_window || !run->if_flag) break;
      if (!kvm_post_run(regs, &kregs))
        break;

      saved_regs = *regs;
      exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
      break;
    case KVM_EXIT_FAIL_ENTRY:
      error("KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx\n",
	      (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
      leavedos_main(99);
      break;
    case KVM_EXIT_INTERNAL_ERROR:
      error("KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x\n", run->internal.suberror);
      leavedos_main(99);
      break;
    default:
      error("KVM: exit_reason = 0x%x\n", run->exit_reason);
      leavedos_main(99);
      break;
    }
  }
  return exit_reason;
}

static void kvm_vme_tf_popf_fixup(struct vm86_regs *regs)
{
  if (regs->eip == 0 || !(regs->eflags & X86_EFLAGS_TF))
    return;
  /* The problem was noticed on
   * AMD FX(tm)-8350 Eight-Core Processor
   * stepping: 0 microcode: 0x6000852
   *
   * popfw fails to clear TF flag:
   * https://github.com/dosemu2/dosemu2/issues/1350
   * We need to clear it by hands.
   * We check for popf and assume it tried to clear TF.
   * */
  if (READ_BYTE(SEGOFF2LINEAR(regs->cs, regs->eip - 1)) == 0x9d) {
    if (!config.test_mode) {
      error("KVM: applying TF fixup\n");
      regs->eflags &= ~X86_EFLAGS_TF;
    } else {
      error("KVM: not applying TF fixup (test mode)\n");
    }
  }
}

/* Emulate vm86() using KVM */
int kvm_vm86(struct vm86_struct *info)
{
  struct vm86_regs *regs;
  int vm86_ret;
  unsigned int trapno, exit_reason;

  regs = &monitor->regs;
  *regs = info->regs;
#if 0
  memcpy(&monitor->fpstate, &vm86_fpu_state, sizeof(vm86_fpu_state));
#endif
  monitor->int_revectored = info->int_revectored;
  monitor->tss.esp0 = offsetof(struct monitor, regs) + sizeof(monitor->regs);

  regs->eflags &= (SAFE_MASK | X86_EFLAGS_VIF | X86_EFLAGS_VIP);
  regs->eflags |= X86_EFLAGS_FIXED | X86_EFLAGS_VM | X86_EFLAGS_IF;

  do {
    exit_reason = kvm_run();

    vm86_ret = VM86_SIGNAL;
    if (exit_reason != KVM_EXIT_HLT) break;

    /* high word(orig_eax) = exception number */
    /* low word(orig_eax) = error code */
    trapno = (regs->orig_eax >> 16) & 0xff;
#if 1
    if (trapno == 1 && (sregs.cr4 & X86_CR4_VME))
      kvm_vme_tf_popf_fixup(regs);
#endif
    if (trapno == 1 || trapno == 3)
      vm86_ret = VM86_TRAP | (trapno << 8);
    else if (trapno == 0xd)
      vm86_ret = kvm_handle_vm86_fault(regs, info->cpu_type);
  } while (vm86_ret == -1);

  info->regs = *regs;
  info->regs.eflags |= X86_EFLAGS_IOPL;
#if 0
  /* we do not update fpstate for performance reasons */
  memcpy(&vm86_fpu_state, &monitor->fpstate, sizeof(vm86_fpu_state));
#endif
  if (vm86_ret == VM86_SIGNAL && exit_reason == KVM_EXIT_HLT) {
    unsigned trapno = (regs->orig_eax >> 16) & 0xff;
    unsigned err = regs->orig_eax & 0xffff;
    vm86_fault(trapno, err, monitor->cr2);
  } else if (exit_reason == KVM_EXIT_MMIO) {
    /* disable instr_emu for now, as coalesced_mmio is slightly faster
     * than sim. However JIT is much faster than those. */
#if USE_INSTREMU
    dosaddr_t addr = (dosaddr_t)run->mmio.phys_addr;
    if (vga.inst_emu && vga_access(addr, addr))
      instr_emu_sim(NULL, 0);
#endif
  }
  return vm86_ret;
}

/* Emulate do_dpmi_control() using KVM */
int kvm_dpmi(cpuctx_t *scp)
{
  struct vm86_regs *regs;
  int ret;
  unsigned int exit_reason;

  monitor->tss.esp0 = offsetof(struct monitor, regs) +
    offsetof(struct vm86_regs, es);
#if 0
  memcpy(&monitor->fpstate, &vm86_fpu_state, sizeof(vm86_fpu_state));
#endif
  regs = &monitor->regs;
  do {
    regs->eax = _eax;
    regs->ebx = _ebx;
    regs->ecx = _ecx;
    regs->edx = _edx;
    regs->esi = _esi;
    regs->edi = _edi;
    regs->ebp = _ebp;
    regs->esp = _esp;
    regs->eip = _eip;

    regs->cs = _cs;
    regs->__null_ds = _ds;
    regs->__null_es = _es;
    regs->ss = _ss;
    regs->__null_fs = _fs;
    regs->__null_gs = _gs;

    regs->eflags = _eflags;
    regs->eflags &= (SAFE_MASK | X86_EFLAGS_VIF | X86_EFLAGS_VIP |
            X86_EFLAGS_IF);
    regs->eflags |= X86_EFLAGS_FIXED;

    exit_reason = kvm_run();

    _eax = regs->eax;
    _ebx = regs->ebx;
    _ecx = regs->ecx;
    _edx = regs->edx;
    _esi = regs->esi;
    _edi = regs->edi;
    _ebp = regs->ebp;
    _esp = regs->esp;
    _eip = regs->eip;

    _cs = regs->cs;
    _ds = regs->__null_ds;
    _es = regs->__null_es;
    _ss = regs->ss;
    _fs = regs->__null_fs;
    _gs = regs->__null_gs;

    _eflags = regs->eflags;

#if 0
  /* we do not update fpstate for performance reasons */
  memcpy(&vm86_fpu_state, &monitor->fpstate, sizeof(vm86_fpu_state));
#endif

    ret = DPMI_RET_DOSEMU; /* mirroring sigio/sigalrm */
    if (exit_reason == KVM_EXIT_HLT) {
      /* orig_eax >> 16 = exception number */
      /* orig_eax & 0xffff = error code */
      _cr2 = monitor->cr2;
      _trapno = (regs->orig_eax >> 16) & 0xff;
      _err = regs->orig_eax & 0xffff;
      if (_trapno > 0x10) {
	// convert software ints into the GPFs that the DPMI code expects
	_err = (_trapno << 3) + 2;
	_trapno = 0xd;
	_eip -= 2;
      }

      if (_trapno == 0x10) {
#if 0
        struct kvm_fpu fpu;
        ioctl(vcpufd, KVM_GET_FPU, &fpu);
#ifdef __x86_64__
        memcpy(__fpstate->_st, fpu.fpr, sizeof __fpstate->_st);
        __fpstate->cwd = fpu.fcw;
        __fpstate->swd = fpu.fsw;
        __fpstate->ftw = fpu.ftwx;
        __fpstate->fop = fpu.last_opcode;
        __fpstate->rip = fpu.last_ip;
        __fpstate->rdp = fpu.last_dp;
        memcpy(__fpstate->_xmm, fpu.xmm, sizeof __fpstate->_xmm);
#else
        memcpy(__fpstate->_st, fpu.fpr, sizeof __fpstate->_st);
        __fpstate->cw = fpu.fcw;
        __fpstate->sw = fpu.fsw;
        __fpstate->tag = fpu.ftwx;
        __fpstate->ipoff = fpu.last_ip;
        __fpstate->cssel = _cs;
        __fpstate->dataoff = fpu.last_dp;
        __fpstate->datasel = _ds;
#endif
        print_exception_info(scp);
#endif
        dbug_printf("coprocessor exception, calling IRQ13\n");
        raise_fpu_irq();
        ret = DPMI_RET_DOSEMU;
      } else
	ret = DPMI_RET_FAULT;
    } else if (exit_reason == KVM_EXIT_MMIO) {
      dosaddr_t addr = (dosaddr_t)run->mmio.phys_addr;
      if (vga.inst_emu && vga_access(addr, addr))
        instr_emu_sim(scp, 1);
      ret = DPMI_RET_CLIENT;
    }
  } while (!signal_pending() && ret == DPMI_RET_CLIENT);
  return ret;
}

void kvm_done(void)
{
  close(vcpufd);
  close(vmfd);
  close(kvmfd);
  free(cpuid);
}

#endif
