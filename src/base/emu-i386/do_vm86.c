
/* Define if we want graphics in X (of course we want :-) (root@zaphod) */
/* WARNING: This may not work in BSD, because it was written for Linux! */
#include <stdio.h>
#include <termios.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <fenv.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/types.h>
#include <limits.h>

#include "emu.h"
#include "mhpdbg.h"
#ifdef __linux__
#include "sys_vm86.h"
#endif
#include "bios.h"
#include "mouse.h"
#include "serial.h"
#include "xms.h"
#include "timers.h"
#include "cmos.h"
#include "memory.h"
#include "port.h"
#include "int.h"
#include "disks.h"
#include "ipx.h"                /* TRB - add support for ipx */
#include "bitops.h"
#include "coopth.h"
#include "utilities.h"
#ifdef X86_EMULATOR
#include "cpu-emu.h"
#endif
#include "kvm.h"

#include "video.h"

#include "pic.h"
#include "emudpmi.h"
#include "hlt.h"
#include "ipx.h"
#include "vgaemu.h"
#include "sig.h"

static void pic_run(void);

int vm86_fault(unsigned trapno, unsigned err, dosaddr_t cr2)
{
#ifdef USE_MHPDBG
  mhp_debug(DBG_INTx + (trapno << 8), 0, 1);
#endif

  if (dpmi_active() && dpmi_realmode_exception(trapno, err, cr2))
    return 0;

  switch (trapno) {
  case 0x00: /* divide_error */
  case 0x01: /* debug */
  case 0x03: /* int3 */
  case 0x04: /* overflow */
  case 0x05: /* bounds */
  case 0x07: /* device_not_available */
    error_once("exception %#x occured\n", trapno);
    if (!IS_REDIRECTED(trapno))
      goto sgleave;
    do_int(trapno);
    return 0;

  case 0x10: /* coprocessor error */
    raise_fpu_irq(); /* this is the 386 way of signalling this */
    return 0;

  case 0x11: /* alignment check */
    /* we are now safe; nevertheless, fall into the default
     * case and exit dosemu, as an AC fault in vm86 is(?) a
     * catastrophic failure.
     */
    goto sgleave;

  case 0x06: /* invalid_op */
    {
      unsigned char *csp;
      error_once("SIGILL while in vm86(): %04x:%04x\n", SREG(cs), LWORD(eip));
      if (config.vga && SREG(cs) == config.vbios_seg) {
	if (!config.vbios_post)
	  error("Fault in VBIOS code, try setting $_vbios_post=(1)\n");
	else
	  error("Fault in VBIOS code, try running xdosemu under X\n");
	goto sgleave;
      }
#if 0
      show_regs();
#endif /* 0 */
      csp = SEG_ADR((unsigned char *), cs, ip);
      /* this one is for CPU detection programs
       * actually we should check if int0x06 has been
       * hooked by the pgm and redirected to it */
      if (!IS_IRET(0x06))
      {
	do_int(trapno);
	return 0;
      }
      /* Some db commands start with 2e (use cs segment)
	 and thus is accounted for here */
      if (csp[0] == 0x2e) {
	csp++;
	LWORD(eip)++;
	goto sgleave;
      }
      if (csp[0] == 0xf0) {
	dbug_printf("ERROR: LOCK prefix not permitted!\n");
	LWORD(eip)++;
	return 0;
      }
      goto sgleave;
    }

  default:
sgleave:
    error("unexpected CPU exception 0x%02x err=0x%08x cr2=%08x while in vm86 (DOS)\n",
	  trapno, err, cr2);
    show_regs();
    flush_log();
    leavedos_from_sig(4);
  }
  return 0; /* keeps GCC happy */
}

static int vm86_hlt_handle(void)
{
  dosaddr_t lina = SEGOFF2LINEAR(_CS, _IP);
  int ret = HLT_RET_NORMAL;

  if ((lina >= BIOS_HLT_BLK) && (lina < BIOS_HLT_BLK+BIOS_HLT_BLK_SIZE)) {
    Bit16u offs = lina - BIOS_HLT_BLK;
    hlt_handle(vm86_hlt_state, offs, NULL);
  }
  else if (lina == XMSControl_ADD) {
    xms_control();
  }
  else if (lina == INT42HOOK_ADD) {
    int42_hook();
  }
  else if (lina == POSTHOOK_ADD) {
    post_hook();
  }
  else if (lina == DPMI_ADD + HLT_OFF(DPMI_dpmi_init)) {
    /* The hlt instruction is 6 bytes in from DPMI_ADD */
    _IP += 1;	/* skip halt to point to FAR RET */
    CARRY;
    dpmi_init();
  }
  else if ((lina >= DPMI_ADD) && (lina < DPMI_ADD + (DPMI_end - DPMI_OFF))) {
    dpmi_realmode_hlt(lina);
  }
  else {
    h_printf("HLT: unknown halt request CS:IP=%04x:%04x!\n", _CS, _IP);
    _IP += 1;
    ret = HLT_RET_NORMAL;
    cpu_idle();
  }
  return ret;
}

/*  */
/* vm86_GP_fault @@@  32768 MOVED_CODE_BEGIN @@@ 01/23/96, ./src/arch/linux/async/sigsegv.c --> src/emu-i386/do_vm86.c  */
/*
 * DANG_BEGIN_FUNCTION vm86_GP_fault
 *
 * description:
 * All from the kernel unhandled general protection faults from V86 mode
 * are handled here. This are mainly port IO and the HLT instruction.
 *
 * DANG_END_FUNCTION
 */

static int handle_GP_fault(void)
{
  unsigned char *csp, *lina;
  int pref_seg;
  int done,is_rep,prefix66,prefix67;

#if 0
  u_short *ssp;
  static int haltcount = 0;
#endif

#define ASIZE_IS_32 (prefix67)
#define OSIZE_IS_32 (prefix66)
#define LWECX	    (ASIZE_IS_32 ? REG(ecx) : LWORD(ecx))
#define setLWECX(x) {if (ASIZE_IS_32) REG(ecx)=(x); else LWORD(ecx) = (x);}
#define MAX_HALT_COUNT 3

#if 0
    csp = SEG_ADR((unsigned char *), cs, ip);
    ssp = SEG_ADR((us *), ss, sp);
    if ((*csp&0xfd)==0xec) {	/* inb, outb */
    	i_printf("VM86_GP_FAULT at %08lx, cod=%02x %02x*%02x %02x %02x %02x\n"
		 "                 stk=%04x %04x %04x %04x\n",
		 (long)csp,
		 csp[-2], csp[-1], csp[0], csp[1], csp[2], csp[3],
		 ssp[0], ssp[1], ssp[2], ssp[3]);
    }
#endif

  csp = lina = SEG_ADR((unsigned char *), cs, ip);

  /* fprintf(stderr, "CSP in cpu is 0x%04x\n", *csp); */


  /* DANG_BEGIN_REMARK
   * Here we handle all prefixes prior switching to the appropriate routines
   * The exception CS:EIP will point to the first prefix that effects
   * the faulting instruction, hence, 0x65 0x66 is same as 0x66 0x65.
   * So we collect all prefixes and remember them.
   * - Hans Lermen
   * DANG_END_REMARK
   */

  #define __SEG_ADR_32(type, seg, reg)  type(MK_FP32(seg, reg))
  #define SEG_ADR_32(type, seg, reg)  type(LINEAR2UNIX(SEGOFF2LINEAR(seg, reg)))
  done=0;
  is_rep=0;
  prefix66=prefix67=0;
  pref_seg=-1;

  do {
    switch (*(csp++)) {
       case 0x66:      /* operand prefix */  prefix66=1; break;
       case 0x67:      /* address prefix */  prefix67=1; break;
       case 0x2e:      /* CS */              pref_seg=SREG(cs); break;
       case 0x3e:      /* DS */              pref_seg=SREG(ds); break;
       case 0x26:      /* ES */              pref_seg=SREG(es); break;
       case 0x36:      /* SS */              pref_seg=SREG(ss); break;
       case 0x65:      /* GS */              pref_seg=SREG(gs); break;
       case 0x64:      /* FS */              pref_seg=SREG(fs); break;
       case 0xf2:      /* repnz */
       case 0xf3:      /* rep */             is_rep=1; break;
       default: done=1;
    }
  } while (!done);
  csp--;

  switch (*csp) {

  case 0xf1:                   /* int 1 */
    LWORD(eip)++; /* emulated "undocumented" instruction */
    do_int(1);
    break;

  case 0x6c:                    /* insb */
    LWORD(eip)++;
    /* NOTE: ES can't be overwritten */
    /* WARNING: no test for (E)DI wrapping! */
    if (ASIZE_IS_32)		/* a32 insb */
      REG(edi) += port_rep_inb(LWORD(edx), SEG_ADR_32((Bit8u *),SREG(es),REG(edi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    else			/* a16 insb */
      LWORD(edi) += port_rep_inb(LWORD(edx), SEG_ADR_32((Bit8u *),SREG(es),LWORD(edi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    if (is_rep) setLWECX(0);
    break;

  case 0x6d:			/* (rep) insw / insd */
    LWORD(eip)++;
    /* NOTE: ES can't be overwritten */
    /* WARNING: no test for (E)DI wrapping! */
    if (prefix66) {		/* insd */
      if (ASIZE_IS_32)		/* a32 insd */
	REG(edi) += port_rep_ind(LWORD(edx), SEG_ADR_32((Bit32u *),SREG(es),REG(edi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
      else			/* a16 insd */
	LWORD(edi) += port_rep_ind(LWORD(edx), SEG_ADR_32((Bit32u *),SREG(es),LWORD(edi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    }
    else {			/* insw */
      if (ASIZE_IS_32)		/* a32 insw */
	REG(edi) += port_rep_inw(LWORD(edx), SEG_ADR_32((Bit16u *),SREG(es),REG(edi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
      else			/* a16 insw */
	LWORD(edi) += port_rep_inw(LWORD(edx), SEG_ADR_32((Bit16u *),SREG(es),LWORD(edi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    }
    if (is_rep) setLWECX(0);
    break;

  case 0x6e:			/* (rep) outsb */
    LWORD(eip)++;
    if (pref_seg < 0) pref_seg = SREG(ds);
    /* WARNING: no test for (E)SI wrapping! */
    if (ASIZE_IS_32)		/* a32 outsb */
      REG(esi) += port_rep_outb(LWORD(edx), __SEG_ADR_32((Bit8u *),pref_seg,REG(esi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    else			/* a16 outsb */
      LWORD(esi) += port_rep_outb(LWORD(edx), __SEG_ADR_32((Bit8u *),pref_seg,LWORD(esi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    if (is_rep) setLWECX(0);
    break;

  case 0x6f:			/* (rep) outsw / outsd */
    LWORD(eip)++;
    if (pref_seg < 0) pref_seg = SREG(ds);
    /* WARNING: no test for (E)SI wrapping! */
    if (prefix66) {		/* outsd */
      if (ASIZE_IS_32)		/* a32 outsd */
	REG(esi) += port_rep_outd(LWORD(edx), __SEG_ADR_32((Bit32u *),pref_seg,REG(esi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
      else			/* a16 outsd */
	LWORD(esi) += port_rep_outd(LWORD(edx), __SEG_ADR_32((Bit32u *),pref_seg,LWORD(esi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    }
    else {			/* outsw */
      if (ASIZE_IS_32)		/* a32 outsw */
	REG(esi) += port_rep_outw(LWORD(edx), __SEG_ADR_32((Bit16u *),pref_seg,REG(esi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
      else			/* a16 outsw */
	LWORD(esi) += port_rep_outw(LWORD(edx), __SEG_ADR_32((Bit16u *),pref_seg,LWORD(esi)),
		LWORD(eflags)&DF, (is_rep?LWECX:1));
    }
    if (is_rep) setLWECX(0);
    break;

  case 0xe5:			/* inw xx, ind xx */
    LWORD(eip) += 2;
    if (prefix66) REG(eax) = port_ind((int) csp[1]);
    else LWORD(eax) = port_inw((int) csp[1]);
    break;

  case 0xe4:			/* inb xx */
    LWORD(eip) += 2;
    LWORD(eax) &= ~0xff;
    LWORD(eax) |= port_inb((int) csp[1]);
    break;

  case 0xed:			/* inw dx, ind dx */
    LWORD(eip)++;
    if (prefix66) REG(eax) = port_ind(LWORD(edx));
    else LWORD(eax) = port_inw(LWORD(edx));
    break;

  case 0xec:			/* inb dx */
    LWORD(eip)++;
    LWORD(eax) &= ~0xff;
    LWORD(eax) |= port_inb(LWORD(edx));
    break;

  case 0xe7:			/* outw xx */
    LWORD(eip) += 2;
    if (prefix66) port_outd((int)csp[1], REG(eax));
    else port_outw((int)csp[1], LWORD(eax));
    break;

  case 0xe6:			/* outb xx */
    LWORD(eip) += 2;
    port_outb((int) csp[1], LO(ax));
    break;

  case 0xef:			/* outw dx, outd dx */
    LWORD(eip)++;
    if (prefix66) port_outd(LWORD(edx), REG(eax));
    else port_outw(LWORD(edx), LWORD(eax));
    break;

  case 0xee:			/* outb dx */
    LWORD(eip)++;
    port_outb(LWORD(edx), LO(ax));
    break;

  case 0xf4:			/* hlt...I use it for various things,
		  like trapping direct jumps into the XMS function */
    vm86_hlt_handle();
    break;

  case 0x0f: /* was: RDE hack, now handled in cpu.c */
    if (!cpu_trap_0f(csp, NULL))
      return 0;
    break;

  case 0xf0:			/* lock */
  default:
    return 0;
  }

  LWORD(eip) += (csp-lina);
  return 1;
}

static void vm86_GP_fault(void)
{
    unsigned char *lina;
    if (handle_GP_fault()) {
#ifdef USE_MHPDBG
	if (isset_TF() && !in_dpmi_pm())
		mhp_debug(DBG_TRAP + (1 << 8), 0, 0);
#endif
	return;
    }
    lina = SEG_ADR((unsigned char *), cs, ip);
#ifdef USE_MHPDBG
    mhp_debug(DBG_GPF, 0, 0);
#endif
    set_debug_level('g', 1);
    error_once("general protection at %p: %x\n", lina,*lina);
    show_regs();
    show_ints(0, 0x33);
    if (IS_REDIRECTED(0x0d)) {
	do_int(0x0d);
	return;
    }
    fatalerr = 4;
    leavedos(fatalerr);		/* shouldn't return */
}
/* @@@ MOVE_END @@@ 32768 */

static int handle_GP_hlt(void)
{
  unsigned char *csp;
  csp = SEG_ADR((unsigned char *), cs, ip);
  if (*csp == 0xf4)
    return vm86_hlt_handle();
  return HLT_RET_NONE;
}

#ifdef __i386__
static int true_vm86(union vm86_union *x)
{
    int ret;
    uint32_t old_flags = REG(eflags);

    loadfpstate(vm86_fpu_state);
again:
#if 0
    ret = vm86(&x->vm86ps);
#else
    /* need to use vm86_plus for now as otherwise dosdebug doesn't work */
    ret = vm86_plus(VM86_ENTER, &x->vm86compat);
#endif
    /* optimize VM86_STI case that can return with ints disabled
     * if VIP is set since kernel commit 5ed92a8ab (4.3-rc1) */
    if (VM86_TYPE(ret) == VM86_STI) {
	if (!isset_IF())
	    goto again;
    }
    /* kernel has a nasty habit of clearing VIP.
     * TODO: check kernel version */
    REG(eflags) |= (old_flags & VIP);

    savefpstate(vm86_fpu_state);
    /* there is no real need to save and restore the FPU state of the
       emulator itself: savefpstate (fnsave) also resets the current FPU
       state using fninit; fesetenv then restores trapping of division by
       zero and overflow which is good enough for calling FPU-using
       routines.
    */
    fesetenv(&dosemu_fenv);
    return ret;
}

void true_vm86_fault(sigcontext_t *scp)
{
    if (_scp_trapno == 0x0e) {
	/* we can get to instremu from here, so unblock SIGALRM & friends.
	 * It is needed to interrupt instremu when it runs for too long. */
	signal_unblock_async_sigs();
	if (vga_emu_fault(_scp_cr2, _scp_err, NULL) == True)
	    return;
    }
    vm86_fault(_scp_trapno, _scp_err, _scp_cr2);
}
#endif

static int do_vm86(union vm86_union *x)
{
#ifdef X86_EMULATOR
    if (config.cpu_vm == CPUVM_EMU || interp_inst_emu_count)
	return e_vm86();
#endif
    if (config.cpu_vm == CPUVM_KVM)
	return kvm_vm86(&x->vm86ps);
#ifdef __i386__
    return true_vm86(x);
#else
    leavedos_main(2);
    return 0;
#endif
}

static void _do_vm86(void)
{
    int retval;
#ifdef USE_MHPDBG
    int dret = 0;
#endif

    if (isset_IF() && isset_VIP()) {
	error("both IF and VIP set\n");
	clear_VIP();
    }
    in_vm86 = 1;
    retval = do_vm86(&vm86u);
    in_vm86 = 0;

    if (
#ifdef X86_EMULATOR
	(debug_level('e')>1)||
#endif
	(debug_level('g')>3)) {
	dbug_printf ("RET_VM86, cs=%04x:%04x ss=%04x:%04x f=%08x ret=0x%x\n",
		_CS, _EIP, _SS, _SP, _EFLAGS, retval);
	if (debug_level('g')>8)
	    dbug_printf ("ax=%08x bx=%08x ss=%04x sp=%08x bp=%08x\n"
			 "           cx=%08x dx=%08x ds=%04x cs=%04x ip=%08x\n"
			 "           si=%08x di=%08x es=%04x flg=%08x\n",
			_EAX, _EBX, _SS, _ESP, _EBP, _ECX, _EDX, _DS, _CS, _EIP,
			_ESI, _EDI, _ES, _EFLAGS);
    }

    switch (VM86_TYPE(retval)) {
    case VM86_UNKNOWN:
	vm86_GP_fault();
	if (in_dpmi_pm())
	    return;
#ifdef USE_MHPDBG
	/* instructions that cause GPF, could also cause single-step
	 * trap but didn't. Catch them here. */
	if (mhpdbg.active)
	    mhp_debug(DBG_PRE_VM86, 0, 0);
#endif
	break;
    case VM86_STI:
	I_printf("Return from vm86() for STI\n");
#ifdef USE_MHPDBG
	/* VIP breaks us out for STI which could otherwise get a
	 * single-step trap. Catch it here. */
	if (mhpdbg.active)
	    mhp_debug(DBG_PRE_VM86, 0, 0);
#endif
	break;
    case VM86_INTx:
#ifdef USE_MHPDBG
	if (mhpdbg.active)
	    dret = mhp_debug(DBG_INTx + (VM86_ARG(retval) << 8), 1, 0);
	if (!dret)
#endif
	    do_int(VM86_ARG(retval));
	break;
    case VM86_TRAP:
#ifdef USE_MHPDBG
	if (mhpdbg.active)
	    dret = mhp_debug(DBG_TRAP + (VM86_ARG(retval) << 8), 0, 0);
	if (!dret)
#endif
	   do_int(VM86_ARG(retval));
	break;
    case VM86_PICRETURN:
        I_printf("Return for FORCE_PIC\n");
        break;
    case VM86_SIGNAL:
	I_printf("Return for SIGNAL\n");
	break;
    default:
	error("unknown return value from vm86()=%x,%d-%x\n", VM86_TYPE(retval), VM86_TYPE(retval), VM86_ARG(retval));
	fatalerr = 4;
    }
}

/*
 * DANG_BEGIN_FUNCTION run_vm86
 *
 * description:
 * Here is where DOSEMU runs VM86 mode with the vm86() call
 * which also has the registers that it will be called with. It will stop
 * vm86 mode for many reasons, like trying to execute an interrupt, doing
 * port I/O to ports not opened for I/O, etc ...
 *
 * DANG_END_FUNCTION
 */
void run_vm86(void)
{
    int retval, cnt;

    if (
#ifdef X86_EMULATOR
	(debug_level('e')>1)||
#endif
	(debug_level('g')>3)) {
	dbug_printf ("DO_VM86,  cs=%04x:%04x ss=%04x:%04x f=%08x\n",
		_CS, _EIP, _SS, _SP, _EFLAGS);
	if (debug_level('g')>8)
	    dbug_printf ("ax=%08x bx=%08x ss=%04x sp=%08x bp=%08x\n"
			 "           cx=%08x dx=%08x ds=%04x cs=%04x ip=%08x\n"
			 "           si=%08x di=%08x es=%04x flg=%08x\n",
			_EAX, _EBX, _SS, _ESP, _EBP, _ECX, _EDX, _DS, _CS, _EIP,
			_ESI, _EDI, _ES, _EFLAGS);
    }

    cnt = 0;
    while ((retval = handle_GP_hlt())) {
	cnt++;
	if (debug_level('g')>3) {
	    g_printf("DO_VM86: premature fault handled, %i\n", cnt);
	    g_printf("RET_VM86, cs=%04x:%04x ss=%04x:%04x f=%08x\n",
		_CS, _EIP, _SS, _SP, _EFLAGS);
	}
	if (in_dpmi_pm())
	    return;
#ifdef USE_MHPDBG
	if (mhpdbg.active)
	    mhp_debug(DBG_PRE_VM86, 0, 0);
#endif
	/* if thread wants some sleep, we can't fuck it in a busy loop */
	if (coopth_wants_sleep_vm86()) {
	    pic_run();                     // try to awake
	    if (in_dpmi_pm())
		return;
	    if (coopth_wants_sleep_vm86()) // not awaken?
		return;
	}
	/* some subsystems doesn't want this optimization loop as well */
	if (retval == HLT_RET_SPECIAL)
	    return;
	if (retval != HLT_RET_NORMAL)
	    break;

	if (debug_level('g') > 3) {
	  dbug_printf ("DO_VM86,  cs=%04x:%04x ss=%04x:%04x f=%08x\n",
		_CS, _EIP, _SS, _SP, _EFLAGS);
	  if (debug_level('g')>8)
	    dbug_printf ("ax=%08x bx=%08x ss=%04x sp=%08x bp=%08x\n"
			 "           cx=%08x dx=%08x ds=%04x cs=%04x ip=%08x\n"
			 "           si=%08x di=%08x es=%04x flg=%08x\n",
			_EAX, _EBX, _SS, _ESP, _EBP, _ECX, _EDX, _DS, _CS, _EIP,
			_ESI, _EDI, _ES, _EFLAGS);
	}
    }
    pic_run();		/* trigger any hardware interrupts requested */
    if (in_dpmi_pm())
	return;

#ifdef USE_MHPDBG
    if (mhpdbg.active)
	mhp_debug(DBG_PRE_VM86, 0, 0);
#endif

    _do_vm86();
}

/* same as run_vm86(), but avoids any looping in handling GPFs */
void vm86_helper(void)
{
  assert(!in_dpmi_pm());
  clear_VIP();
  _do_vm86();
  handle_signals();
  coopth_run();
}

static void pic_run(void)
{
    int inum;

    if (!pic_pending()) {
        clear_VIP();
        return;
    }

    if (!isset_IF()) {
#if 0
        /* try to detect timer flood, and not set VIP if it is there.
         * See https://github.com/stsp/dosemu2/issues/918
         */
        if (pic_sys_time < pic_dos_time +
                               TIMER0_FLOOD_THRESHOLD ||
                               pic_pending_masked(1 << PIC_IRQ0))
            set_VIP();
        else
            r_printf("PIC: timer flood work-around\n");
#else
        /* the above work-around regresses goblins2, see
         * https://github.com/dosemu2/dosemu2/issues/1300
         */
        set_VIP();
#endif
        return;                      /* exit if ints are disabled */
    }
    clear_VIP();

    inum = pic_get_inum();
    if (dpmi_active())
        run_pm_int(inum);
    else
        real_run_int(inum);
}

/*
 * DANG_BEGIN_FUNCTION loopstep_run_vm86
 *
 * description:
 * Here we collect all stuff, that has to be executed within
 * _one_ pass (step) of a loop containing run_vm86().
 * DANG_END_FUNCTION
 */
void loopstep_run_vm86(void)
{
    uncache_time();
    if (!dosemu_frozen && !signal_pending()) {
	if (in_dpmi_pm())
	    run_dpmi();
	else
	    run_vm86();
    }
    if (dosemu_frozen)
	dosemu_sleep();
    do_periodic_stuff();
    hardware_run();
#ifdef USE_MHPDBG
    if (mhpdbg_is_stopped())
	return;
#endif
}

int do_call_back(Bit16u cs, Bit16u ip)
{
    fake_call_to(cs, ip); /* far jump to the vm86(DOS) routine */
    return coopth_sched();
}

int do_int_call_back(int intno)
{
    do_int(intno);
    return coopth_sched();
}

int vm86_init(void)
{
    return 0;
}

void *vm86_hlt_state;

Bit16u hlt_register_handler_vm86(emu_hlt_t handler)
{
    return hlt_register_handler(vm86_hlt_state, handler);
}

int hlt_unregister_handler_vm86(Bit16u start_addr)
{
    return hlt_unregister_handler(vm86_hlt_state, start_addr);
}
