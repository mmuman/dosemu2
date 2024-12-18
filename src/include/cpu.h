/* cpu.h, for the Linux DOS emulator
 *    Copyright (C) 1993 Robert Sanders, gt8134b@prism.gatech.edu
 */

#ifndef CPU_H
#define CPU_H

#include "pic.h"
#include "types.h"
#include "bios.h"
#include "memory.h"
#include "sig.h"
#include <signal.h>
#include <inttypes.h>
#include <fenv.h>
#include "vm86_compat.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE	4096
#endif

#define HOST_PAGE_SIZE		(unsigned int)sysconf(_SC_PAGESIZE)
#define HOST_PAGE_MASK		(~(HOST_PAGE_SIZE-1))
#define HOST_PAGE_ALIGN(addr)	(((addr)+HOST_PAGE_SIZE-1)&HOST_PAGE_MASK)

#define _regs vm86s.regs

#ifndef HAVE_STD_C11
#undef static_assert
#define static_assert(c, m) ((const char *) 0)
#else
#ifndef HAVE_STATIC_ASSERT
#define static_assert _Static_assert
#endif
#endif

/* all registers as a structure */
#define REGS  vm86s.regs
/* this is used like: REG(eax) = 0xFFFFFFF */
#ifdef __i386__
/* unfortunately the regs are defined as long (not even unsigned) in vm86.h */
#define REG(reg) (*(uint32_t *)({static_assert(sizeof(REGS.reg) == 4, "bad reg"); &REGS.reg; }))
#else
#define REG(reg) (*({static_assert(sizeof(REGS.reg) == 4, "bad reg"); &REGS.reg; }))
#endif
#define SREG(reg) (*({static_assert(sizeof(REGS.reg) == 2, "bad sreg"); &REGS.reg; }))
#define READ_SEG_REG(reg) (REGS.reg)
#define WRITE_SEG_REG(reg, val) REGS.reg = (val)

#define MAY_ALIAS __attribute__((may_alias))

union dword {
  Bit32u d;
  struct { Bit16u l, h; } w;
#ifdef __i386__
  /* unsigned long member is needed only for strict aliasing,
   * we never convert to it, but sometimes convert _from_ it */
  unsigned long ul;
#endif
  struct { Bit8u l, h, b2, b3; } b;
} MAY_ALIAS;

union word {
  Bit16u w;
  struct { Bit8u l, h; } b;
} MAY_ALIAS;

struct emu_fsave {
  uint32_t		cw;
  uint32_t		sw;
  uint32_t		tag;
  uint32_t		ipoff;
  uint32_t		cssel;
  uint32_t		dataoff;
  uint32_t		datasel;
  struct { uint16_t element[5]; } st[8];
  uint32_t		status;
};

struct emu_fpxstate {
  /* 32-bit FXSAVE format in 64bit mode (same as in 32bit mode but more xmms) */
  uint16_t		cwd;
  uint16_t		swd;
  uint16_t		ftw;
  uint16_t		fop;
  uint32_t		fip;
  uint32_t		fcs;
  uint32_t		fdp;
  uint32_t		fds;
  uint32_t		mxcsr;
  uint32_t		mxcr_mask;
  struct { uint32_t element[4]; } st[8];
  struct { uint32_t element[4]; } xmm[16];
  struct { uint32_t element[4]; } reserved[3];
  struct { uint32_t element[4]; } scratch[3];
};

static_assert(sizeof(struct emu_fpxstate) == 512, "size mismatch");

void fxsave_to_fsave(const struct emu_fpxstate *fxsave,
    struct emu_fsave *fptr);
void fsave_to_fxsave(const struct emu_fsave *fptr,
    struct emu_fpxstate *fxsave);

/* Structure to describe FPU registers.  */
typedef struct emu_fpxstate emu_fpstate;
typedef emu_fpstate *emu_fpregset_t;

#ifndef HAVE_GREG_T
typedef unsigned long greg_t;
#endif

union g_reg {
  greg_t reg;
#ifdef __x86_64__
  uint64_t q;
  uint32_t d[2];
  uint16_t w[4];
#else
  uint32_t d[1];
  uint16_t w[2];
#endif
} MAY_ALIAS;

#define DWORD__(reg, c)	(((c union g_reg *)&(reg))->d[0])
/* vxd.c redefines DWORD */
#define DWORD(wrd)	DWORD__(wrd,)
#define DWORD_(wrd)	DWORD__(wrd,)

#define LO_WORD_(wrd, c)	(((c union dword *)&(wrd))->w.l)
#define HI_WORD_(wrd, c)	(((c union dword *)&(wrd))->w.h)
#define LO_WORD(wrd)		LO_WORD_(wrd,)
#define HI_WORD(wrd)		HI_WORD_(wrd,)
#if 0
#define LO_BYTE_(wrd, c)	(((c union word *)({static_assert(sizeof(wrd)==2, "bad reg"); &(wrd);}))->b.l)
#define HI_BYTE_(wrd, c)	(((c union word *)({static_assert(sizeof(wrd)==2, "bad reg"); &(wrd);}))->b.h)
#else
#define LO_BYTE_(wrd, c)	(((c union word *)&(wrd))->b.l)
#define HI_BYTE_(wrd, c)	(((c union word *)&(wrd))->b.h)
#endif
#define LO_BYTE(wrd)		LO_BYTE_(wrd,)
#define HI_BYTE(wrd)		HI_BYTE_(wrd,)
#define LO_BYTE_c(wrd)		LO_BYTE_(wrd, const)
#define HI_BYTE_c(wrd)		HI_BYTE_(wrd, const)
#define LO_BYTE_d(wrd)	(((union dword *)&(wrd))->b.l)
#define HI_BYTE_d(wrd)	(((union dword *)&(wrd))->b.h)
#define LO_BYTE_dc(wrd)	(((const union dword *)&(wrd))->b.l)
#define HI_BYTE_dc(wrd)	(((const union dword *)&(wrd))->b.h)

#define _AL      LO(ax)
#define _BL      LO(bx)
#define _CL      LO(cx)
#define _DL      LO(dx)
#define _AH      HI(ax)
#define _BH      HI(bx)
#define _CH      HI(cx)
#define _DH      HI(dx)
#define _AX      LWORD(eax)
#define _BX      LWORD(ebx)
#define _CX      LWORD(ecx)
#define _DX      LWORD(edx)
#define _SI      LWORD(esi)
#define _DI      LWORD(edi)
#define _BP      LWORD(ebp)
#define _SP      LWORD(esp)
#define _IP      LWORD(eip)
#define _EAX     UDWORD(eax)
#define _EBX     UDWORD(ebx)
#define _ECX     UDWORD(ecx)
#define _EDX     UDWORD(edx)
#define _ESI     UDWORD(esi)
#define _EDI     UDWORD(edi)
#define _EBP     UDWORD(ebp)
#define _ESP     UDWORD(esp)
#define _EIP     UDWORD(eip)
#define _CS      (vm86s.regs.cs)
#define _DS      (vm86s.regs.ds)
#define _SS      (vm86s.regs.ss)
#define _ES      (vm86s.regs.es)
#define _FS      (vm86s.regs.fs)
#define _GS      (vm86s.regs.gs)
#define _EFLAGS  UDWORD(eflags)
#define _FLAGS   REG(eflags)

/* these are used like:  LO(ax) = 2 (sets al to 2) */
#define LO(reg)  vm86u.b[offsetof(struct vm86_struct, regs.e##reg)]
#define HI(reg)  vm86u.b[offsetof(struct vm86_struct, regs.e##reg)+1]

#define _LO(reg) LO_BYTE_d(_##e##reg)
#define _HI(reg) HI_BYTE_d(_##e##reg)
#define _LO_(reg) LO_BYTE_dc(_##e##reg##_)
#define _HI_(reg) HI_BYTE_dc(_##e##reg##_)

/* these are used like: LWORD(eax) = 65535 (sets ax to 65535) */
#define LWORD(reg)	(*({ static_assert(sizeof(REGS.reg) == 4, "bad reg"); \
	&vm86u.w[offsetof(struct vm86_struct, regs.reg)/2]; }))
#define HWORD(reg)	(*({ static_assert(sizeof(REGS.reg) == 4, "bad reg"); \
	&vm86u.w[offsetof(struct vm86_struct, regs.reg)/2+1]; }))
#define UDWORD(reg)	(*({ static_assert(sizeof(REGS.reg) == 4, "bad reg"); \
	&vm86u.d[offsetof(struct vm86_struct, regs.reg)/4]; }))

#define _LWORD(reg)	LO_WORD(_##reg)
#define _HWORD(reg)	HI_WORD(_##reg)
#define _LWORD_(reg)	((unsigned)LO_WORD_(_##reg, const))
#define _HWORD_(reg)	((unsigned)HI_WORD_(_##reg, const))

/* this is used like: SEG_ADR((char *), es, bx) */
#define SEG_ADR(type, seg, reg)  type(LINEAR2UNIX(SEGOFF2LINEAR(SREG(seg), LWORD(e##reg))))

/* alternative SEG:OFF to linear conversion macro */
#define SEGOFF2LINEAR(seg, off)  ((((unsigned)(seg)) << 4) + (off))

#define SEG2UNIX(seg)		LINEAR2UNIX(SEGOFF2LINEAR(seg, 0))

typedef unsigned int FAR_PTR;	/* non-normalized seg:off 32 bit DOS pointer */
typedef struct {
  u_short offset;
  u_short segment;
} far_t;
#define FAR_NULL (far_t){0,0}
#define MK_FP16(s,o)		((((unsigned int)(s)) << 16) | ((o) & 0xffff))
#define MK_FP(f)		MK_FP16(f.segment, f.offset)
#define FP_OFF16(far_ptr)	((far_ptr) & 0xffff)
#define FP_SEG16(far_ptr)	(((far_ptr) >> 16) & 0xffff)
#define MK_FP32(s,o)		LINEAR2UNIX(SEGOFF2LINEAR(s,o))
#define FP_OFF32(linear)	((linear) & 15)
#define FP_SEG32(linear)	(((linear) >> 4) & 0xffff)
#define rFAR_PTR(type,far_ptr) ((type)((FP_SEG16(far_ptr) << 4)+(FP_OFF16(far_ptr))))
#define FARt_PTR(f_t_ptr) (MK_FP32((f_t_ptr).segment, (f_t_ptr).offset))
#define MK_FARt(seg, off) ((far_t){(off), (seg)})
static inline far_t rFAR_FARt(FAR_PTR far_ptr) {
  return MK_FARt(FP_SEG16(far_ptr), FP_OFF16(far_ptr));
}
static inline void *FAR2PTR(FAR_PTR far_ptr) {
  return MK_FP32(FP_SEG16(far_ptr), FP_OFF16(far_ptr));
}
static inline dosaddr_t FAR2ADDR(far_t ptr) {
  return SEGOFF2LINEAR(ptr.segment, ptr.offset);
}

#define peek(seg, off)	(READ_WORD(SEGOFF2LINEAR(seg, off)))

extern emu_fpstate vm86_fpu_state;
extern fenv_t dosemu_fenv;

/*
 * Boy are these ugly, but we need to do the correct 16-bit arithmetic.
 * Using non-obvious calling conventions..
 */
#define pushw(base, ptr, val) \
	do { \
		ptr = (Bit16u)(ptr - 1); \
		WRITE_BYTE((base) + ptr, (val) >> 8); \
		ptr = (Bit16u)(ptr - 1); \
		WRITE_BYTE((base) + ptr, val); \
	} while(0)

#define pushl(base, ptr, val) \
	do { \
		pushw(base, ptr, (Bit16u)((val) >> 16)); \
		pushw(base, ptr, (Bit16u)(val)); \
	} while(0)

#define popb(base, ptr) \
	({ \
		Bit8u __res = READ_BYTE((base) + ptr); \
		ptr = (Bit16u)(ptr + 1); \
		__res; \
	})

#define popw(base, ptr) \
	({ \
		Bit8u __res0, __res1; \
		__res0 = READ_BYTE((base) + ptr); \
		ptr = (Bit16u)(ptr + 1); \
		__res1 = READ_BYTE((base) + ptr); \
		ptr = (Bit16u)(ptr + 1); \
		(__res1 << 8) | __res0; \
	})

#define popl(base, ptr) \
	({ \
		Bit16u __res0, __res1; \
		__res0 = popw(base, ptr); \
		__res1 = popw(base, ptr); \
		(__res1 << 16) | __res0; \
	})

#if defined(__x86_64__) || defined (__i386__)
#define getflags() \
	({ \
		unsigned long __value; \
		asm volatile("pushf ; pop %0":"=g" (__value)); \
		__value; \
	})
#else
#define getflags() 0
#endif

static inline void loadfpstate_legacy(emu_fpstate *buf)
{
	struct emu_fsave fsave;
	fxsave_to_fsave(buf, &fsave);
	asm volatile("frstor %0\n" :: "m"(fsave));
}

static inline void savefpstate_legacy(emu_fpstate *buf)
{
	struct emu_fsave fsave;
	asm volatile("fnsave %0; fwait\n" : "=m"(fsave));
	fsave_to_fxsave(&fsave, buf);
}

#if defined(__x86_64__)
/* use 32bit versions */
#define loadfpstate(value) asm volatile("fxrstor %0\n" :: "m"(value))
#define savefpstate(value) asm volatile("fxsave %0\n" : "=m"(value))
#elif defined (__i386__)
#define loadfpstate(value) do { \
	if (config.cpufxsr) \
		asm volatile("fxrstor %0\n" :: "m"(value)); \
	else \
		loadfpstate_legacy(&value); \
} while (0)
#define savefpstate(value) do { \
	if (config.cpufxsr) \
		asm volatile("fxsave %0\n" : "=m"(value)); \
	else \
		savefpstate_legacy(&value); \
} while (0)
#else
#define loadfpstate(value)
#define savefpstate(value)
#endif

/* flags */
#define CF  (1 <<  0)
#define PF  (1 <<  2)
#define AF  (1 <<  4)
#define ZF  (1 <<  6)
#define SF  (1 <<  7)
#define TF  TF_MASK	/* (1 <<  8) */
#define IF  IF_MASK	/* (1 <<  9) */
#define DF  (1 << 10)
#define OF  (1 << 11)
#define NT  NT_MASK	/* (1 << 14) */
#define RF  (1 << 16)
#define VM  VM_MASK	/* (1 << 17) */
#define AC  AC_MASK	/* (1 << 18) */
#define VIF VIF_MASK
#define VIP VIP_MASK
#define ID  ID_MASK

#define IOPL_SHIFT 12
#ifndef IOPL_MASK
#define IOPL_MASK  (3 << IOPL_SHIFT)
#endif

  /* Flag setting and clearing, and testing */
        /* interrupt flag */
#define set_IF() (_EFLAGS |= VIF)
#define clear_IF() (_EFLAGS &= ~VIF)
#define clear_IF_timed() (clear_IF(), ({if (!is_cli) is_cli++;}))
#define isset_IF() ((_EFLAGS & VIF) != 0)
       /* carry flag */
#define set_CF() (_EFLAGS |= CF)
#define clear_CF() (_EFLAGS &= ~CF)
#define isset_CF() ((_EFLAGS & CF) != 0)
       /* sign flag */
#define set_SF() (_EFLAGS |= SF)
#define clear_SF() (_EFLAGS &= ~SF)
#define isset_SF() ((_EFLAGS & SF) != 0)
       /* zero flag */
#define set_ZF() (_EFLAGS |= ZF)
#define clear_ZF() (_EFLAGS &= ~ZF)
       /* direction flag */
#define set_DF() (_EFLAGS |= DF)
#define clear_DF() (_EFLAGS &= ~DF)
#define isset_DF() ((_EFLAGS & DF) != 0)
       /* nested task flag */
#define set_NT() (_EFLAGS |= NT)
#define clear_NT() (_EFLAGS &= ~NT)
#define isset_NT() ((_EFLAGS & NT) != 0)
       /* trap flag */
#define set_TF() (_EFLAGS |= TF)
#define clear_TF() (_EFLAGS &= ~TF)
#define isset_TF() ((_EFLAGS & TF) != 0)
       /* alignment flag */
#define set_AC() (_EFLAGS |= AC)
#define clear_AC() (_EFLAGS &= ~AC)
#define isset_AC() ((_EFLAGS & AC) != 0)
       /* Virtual Interrupt Pending flag */
#define set_VIP()   (_EFLAGS |= VIP)
#define clear_VIP() (_EFLAGS &= ~VIP)
#define isset_VIP()   ((_EFLAGS & VIP) != 0)

#ifdef USE_MHPDBG
#define set_EFLAGS(flgs, new_flgs) ({ \
  uint32_t __oflgs = (flgs); \
  uint32_t __nflgs = (new_flgs); \
  (flgs)=(__nflgs) | IF | IOPL_MASK | ((__nflgs & IF) ? VIF : 0) \
    (mhpdbg.active ? (__oflgs & TF) : 0); \
  ((__nflgs & IF) ? set_IF() : clear_IF()); \
})
#else
#define set_EFLAGS(flgs, new_flgs) ({ \
  uint32_t __nflgs = (new_flgs); \
  (flgs)=(__nflgs) | IF | IOPL_MASK | ((__nflgs & IF) ? VIF : 0); \
  ((__nflgs & IF) ? set_IF() : clear_IF()); \
})
#endif
#define set_FLAGS(flags) set_EFLAGS(_FLAGS, flags)
#define get_EFLAGS(flags) ({ \
  int __flgs = (flags); \
  (((__flgs & IF) ? __flgs | VIF : __flgs & ~VIF) | IF | IOPL_MASK | 2); \
})
#define get_FLAGS(flags) ({ \
  int __flgs = (flags); \
  (((__flgs & VIF) ? __flgs | IF : __flgs & ~IF)); \
})
#define read_EFLAGS() (isset_IF()? (_EFLAGS | IF):(_EFLAGS & ~IF))
#define read_FLAGS()  (isset_IF()? (_FLAGS | IF):(_FLAGS & ~IF))

#define CARRY   set_CF()
#define NOCARRY clear_CF()

#define vflags read_FLAGS()

#define IS_CR0_AM_SET() (config.cpu_vm == CPUVM_VM86)

/* this is the array of interrupt vectors */
struct vec_t {
  unsigned short offset;
  unsigned short segment;
};

#if 0
EXTERN struct vec_t *ivecs;

#endif
#ifdef TRUST_VEC_STRUCT
#define IOFF(i) ivecs[i].offset
#define ISEG(i) ivecs[i].segment
#else
#define IOFF(i) READ_WORD(i * 4)
#define ISEG(i) READ_WORD(i * 4 + 2)
#endif

#define IVEC(i) ((ISEG(i)<<4) + IOFF(i))
#define SETIVEC(i, seg, ofs)	do { WRITE_WORD(i * 4 + 2, seg); \
				  WRITE_WORD(i * 4, ofs); } while (0)

#define OP_IRET			0xcf

#include "memory.h" /* for INT_OFF */
#define IS_REDIRECTED(i)	(ISEG(i) != BIOSSEG)
#define IS_IRET(i)		(READ_BYTE(IVEC(i)) == OP_IRET)

/*
#define WORD(i) (unsigned short)(i)
*/

struct pm_regs {
	unsigned ebx;
	unsigned ecx;
	unsigned edx;
	unsigned esi;
	unsigned edi;
	unsigned ebp;
	unsigned eax;
	unsigned eip;
	unsigned short cs;
	unsigned eflags;
	unsigned esp;
	unsigned short ss;
	unsigned short es;
	unsigned short ds;
	unsigned short fs;
	unsigned short gs;

	unsigned trapno;
	unsigned err;
	dosaddr_t cr2;
};
typedef struct pm_regs cpuctx_t;
#define REGS_SIZE offsetof(struct pm_regs, trapno)

#define _es     (scp->es)
#define _ds     (scp->ds)
#define _es_    (scp->es)
#define _ds_    (scp->ds)
#define get_edi(s)    ((s)->edi)
#define get_esi(s)    ((s)->esi)
#define get_ebp(s)    ((s)->ebp)
#define get_esp(s)    ((s)->esp)
#define get_ebx(s)    ((s)->ebx)
#define get_edx(s)    ((s)->edx)
#define get_ecx(s)    ((s)->ecx)
#define get_eax(s)    ((s)->eax)
#define get_eip(s)    ((s)->eip)
#define get_eflags(s) ((s)->eflags)
#define get_es(s)     ((s)->es)
#define get_ds(s)     ((s)->ds)
#define get_ss(s)     ((s)->ss)
#define get_fs(s)     ((s)->fs)
#define get_gs(s)     ((s)->gs)
#define get_cs(s)     ((s)->cs)
#define get_trapno(s) ((s)->trapno)
#define get_err(s)    ((s)->err)
#define get_cr2(s)    ((s)->cr2)
#define _edi    get_edi(scp)
#define _esi    get_esi(scp)
#define _ebp    get_ebp(scp)
#define _esp    get_esp(scp)
#define _ebx    get_ebx(scp)
#define _edx    get_edx(scp)
#define _ecx    get_ecx(scp)
#define _eax    get_eax(scp)
#define _eip    get_eip(scp)
#define _edi_   (scp->edi)
#define _esi_   (scp->esi)
#define _ebp_   (scp->ebp)
#define _esp_   (scp->esp)
#define _ebx_   (scp->ebx)
#define _edx_   (scp->edx)
#define _ecx_   (scp->ecx)
#define _eax_   (scp->eax)
#define _eip_   (scp->eip)
#define _cs     (scp->cs)
#define _cs_    (scp->cs)
#define _gs     (scp->gs)
#define _fs     (scp->fs)
#define _ss     (scp->ss)
#define _err    (scp->err)
#define _eflags (scp->eflags)
#define _eflags_ (scp->eflags)
#define _cr2    (scp->cr2)
#define _trapno (scp->trapno)
/* compatibility */
#define _rdi    _edi
#define _rsi    _esi
#define _rbp    _ebp
#define _rsp    _esp
#define _rbx    _ebx
#define _rdx    _edx
#define _rcx    _ecx
#define _rax    _eax
#define _rip    _eip
#define _rax    _eax
#define _rip    _eip

void show_regs(void);
void show_ints(int, int);
char *emu_disasm(unsigned int ip);
void dump_state(void);

int cpu_trap_0f (unsigned char *, cpuctx_t *);

#define _PAGE_MASK	(~(PAGE_SIZE-1))
/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&_PAGE_MASK)

enum { es_INDEX, cs_INDEX, ss_INDEX, ds_INDEX, fs_INDEX, gs_INDEX,
  eax_INDEX, ebx_INDEX, ecx_INDEX, edx_INDEX, esi_INDEX, edi_INDEX,
  ebp_INDEX, esp_INDEX, eip_INDEX, eflags_INDEX };

extern int is_cli;

#endif /* CPU_H */
