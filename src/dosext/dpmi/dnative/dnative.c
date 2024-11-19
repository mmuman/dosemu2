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
 * native DPMI backend
 *
 * Author: Stas Sergeev
 */
#include <pthread.h>
#include <stdlib.h>
#include "libpcl/pcl.h"
#include "sig.h"
#include "cpu.h"
#include "dosemu_debug.h"
#include "utilities.h"
#include "emu.h"
#include "vgaemu.h"
#include "cpu-emu.h"
#include "mapping.h"
#include "emudpmi.h"
#include "dpmisel.h"
#include "dnative.h"
#include "dnpriv.h"

#define EMU_X86_FXSR_MAGIC	0x0000
static coroutine_t dpmi_tid;
static cohandle_t co_handle;
static int dpmi_ret_val;
static sigcontext_t emu_stack_frame;
static int in_dpmi_thr;
static int dpmi_thr_running;

static void copy_context(sigcontext_t *d, sigcontext_t *s)
{
#ifdef __linux__
  /* keep pointer to FPU state the same */
  fpregset_t fptr = d->fpregs;
#endif
  *d = *s;
#ifdef __linux__
  d->fpregs = fptr;
#endif
}

#ifdef __i386__
/* On i386 only, if SSE is available (i.e. since the Pentium III),
   the kernel will use FXSAVE to save FPU state, then put the following
   on the signal stack:
   * FXSAVE format converted to FSAVE format (108 bytes)
   * status and magic field where magic == X86_FXSR_MAGIC (4 bytes)
   * FXSAVE format (512 bytes), which can be used directly by our loadfpstate
   However, when restoring FPU state it will only use the mxcsr and xmm
   fields from the FXSAVE format, and take everything else from the FSAVE
   format, so we must "undo" the kernel logic and put those fields into the
   FSAVE region.
   see also arch/x86/kernel/fpu/regset.c in Linux kernel */
static void convert_from_fxsr(fpregset_t fptr,
			      const struct emu_fpxstate *fxsave)
{
  static_assert(sizeof(*fptr) == sizeof(struct emu_fsave),
		  "size mismatch");
  fxsave_to_fsave(fxsave, (struct emu_fsave *)fptr);
}
#endif

static void copy_to_dpmi(sigcontext_t *scp, cpuctx_t *s)
{
#ifdef __x86_64__
  /* needs to clear high part of RIP and RSP because AMD CPUs
   * check whole 64bit regs against 32bit CS and SS limits on iret */
  _scp_rip = 0;
  _scp_rsp = 0;
#endif
#define _C(x) _scp_##x = get_##x(s)
  _C(es);
  _C(ds);
  _C(ss);
  _C(cs);
  _C(fs);
  _C(gs);
  _C(eax);
  _C(ebx);
  _C(ecx);
  _C(edx);
  _C(esi);
  _C(edi);
  _C(ebp);
  _C(esp);
  _C(eip);
  _C(eflags);
  _C(trapno);
  _C(err);
  _scp_cr2 = (uintptr_t)MEM_BASE32(get_cr2(s));

  if (scp->fpregs) {
    void *fpregs = scp->fpregs;
#ifdef __x86_64__
    static_assert(sizeof(*scp->fpregs) == sizeof(vm86_fpu_state),
		  "size mismatch");
#else
    /* i386: convert fxsave state to fsave state */
    convert_from_fxsr(scp->fpregs, &vm86_fpu_state);
    if ((scp->fpregs->status >> 16) != EMU_X86_FXSR_MAGIC)
      return;
    fpregs = &scp->fpregs->status + 1;
#endif
    memcpy(fpregs, &vm86_fpu_state, sizeof(vm86_fpu_state));
  }
}

static void copy_to_emu(cpuctx_t *d, sigcontext_t *scp)
{
#define _D(x) get_##x(d) = _scp_##x
  _D(es);
  _D(ds);
  _D(ss);
  _D(cs);
  _D(fs);
  _D(gs);
  _D(eax);
  _D(ebx);
  _D(ecx);
  _D(edx);
  _D(esi);
  _D(edi);
  _D(ebp);
  _D(esp);
  _D(eip);
  _D(eflags);
  _D(trapno);
  _D(err);
  get_cr2(d) = DOSADDR_REL(LINP(_scp_cr2));
  if (scp->fpregs) {
    void *fpregs = scp->fpregs;
#ifdef __x86_64__
    static_assert(sizeof(*scp->fpregs) == sizeof(vm86_fpu_state),
		"size mismatch");
#else
    if ((scp->fpregs->status >> 16) == EMU_X86_FXSR_MAGIC)
      fpregs = &scp->fpregs->status + 1;
    else {
      fsave_to_fxsave(fpregs, &vm86_fpu_state);
      return;
    }
#endif
    memcpy(&vm86_fpu_state, fpregs, sizeof(vm86_fpu_state));
  }
}

static void dpmi_thr(void *arg);

static int handle_pf(cpuctx_t *scp)
{
    int rc;
    dosaddr_t cr2 = _cr2;
#ifdef X86_EMULATOR
#ifdef HOST_ARCH_X86
    /* DPMI code touches cpuemu prot */
    if (IS_EMU_JIT() && e_invalidate_page_full(cr2))
        return DPMI_RET_CLIENT;
#endif
#endif
    signal_unblock_async_sigs();
    rc = vga_emu_fault(cr2, _err, scp);
    /* going for dpmi_fault() or deinit_handler(),
     * careful with async signals and sas_wa */
    signal_restore_async_sigs();
    if (rc == True)
        return DPMI_RET_CLIENT;
    return DPMI_RET_FAULT;
}

/* ======================================================================== */
/*
 * DANG_BEGIN_FUNCTION native_dpmi_control
 *
 * This function is similar to the vm86() syscall in the kernel and
 * switches to dpmi code.
 *
 * DANG_END_FUNCTION
 */

int native_dpmi_control(cpuctx_t *scp)
{
    unsigned saved_IF = (_eflags & IF);

    _eflags = get_EFLAGS(_eflags);
    if (in_dpmi_thr)
        signal_switch_to_dpmi();
    else
        dpmi_tid =
            co_create(co_handle, dpmi_thr, NULL, NULL, SIGSTACK_SIZE);
    dpmi_thr_running++;
    co_call(dpmi_tid);
    dpmi_thr_running--;
    if (in_dpmi_thr)
        signal_switch_to_dosemu();
    assert(_eflags & IF);
    if (!saved_IF)
        _eflags &= ~IF;
    _eflags &= ~VIF;
    if (dpmi_ret_val == DPMI_RET_FAULT && _trapno == 0x0e)
        dpmi_ret_val = handle_pf(scp);
    /* we may return here with sighandler's signal mask.
     * This is done for speed-up. dpmi_control() restores the mask. */
    return dpmi_ret_val;
}

int native_dpmi_exit(cpuctx_t *scp)
{
    int ret;
    if (!in_dpmi_thr)
        return DPMI_RET_DOSEMU;
    D_printf("DPMI: leaving\n");
    dpmi_ret_val = DPMI_RET_EXIT;
    ret = native_dpmi_control(scp);
    if (in_dpmi_thr)
        error("DPMI thread have not terminated properly\n");
    return ret;
}

void dpmi_return(sigcontext_t *scp, int retcode)
{
    /* only used for CPUVM_NATIVE (from sigsegv.c: dosemu_fault1()) */
    if (!DPMIValidSelector(_scp_cs)) {
        dosemu_error("Return to dosemu requested within dosemu context\n");
        return;
    }
    dpmi_ret_val = retcode;
    if (retcode == DPMI_RET_EXIT) {
        copy_context(scp, &emu_stack_frame);
        return;
    }
    copy_to_emu(dpmi_get_scp(), scp);
    /* signal handlers start with clean FPU state, but we unmask
       overflow/division by zero in main code */
    fesetenv(&dosemu_fenv);
    signal_return_to_dosemu();
    co_resume(co_handle);
    signal_return_to_dpmi();
    if (dpmi_ret_val == DPMI_RET_EXIT)
        copy_context(scp, &emu_stack_frame);
    else
        copy_to_dpmi(scp, dpmi_get_scp());
}

void dpmi_switch_sa(int sig, siginfo_t * inf, void *uc)
{
    ucontext_t *uct = uc;
    sigcontext_t *scp = &uct->uc_mcontext;
    copy_context(&emu_stack_frame, scp);
    copy_to_dpmi(scp, dpmi_get_scp());
    unsetsig(DPMI_TMP_SIG);
    deinit_handler(scp, &uct->uc_flags);
}

static void indirect_dpmi_transfer(void)
{
    signative_start();
    signal_set_altstack(1);
    /* for some absolutely unclear reason neither pthread_self() nor
     * pthread_kill() are the memory barriers. */
    asm volatile ("":::"memory");
    pthread_kill(pthread_self(), DPMI_TMP_SIG);
    /* and we are back */
    signal_set_altstack(0);
    signative_stop();
    /* we inherited FPU state from DPMI, so put back to DOSEMU state */
    fesetenv(&dosemu_fenv);
}

static void dpmi_thr(void *arg)
{
    in_dpmi_thr++;
    indirect_dpmi_transfer();
    in_dpmi_thr--;
}

int native_dpmi_setup(void)
{
    signative_init();
    co_handle = co_thread_init(PCL_C_MC);
    return 0;
}

void native_dpmi_done(void)
{
    if (in_dpmi_thr && !dpmi_thr_running)
        co_delete(dpmi_tid);
    co_thread_cleanup(co_handle);
}

void native_dpmi_enter(void)
{
    /* if we are going directly to a sighandler, mask async signals. */
    if (in_dpmi_thr)
        signal_restore_async_sigs();
}

void native_dpmi_leave(void)
{
    /* for speed-up, DPMI switching corrupts signal mask. Fix it here. */
    signal_unblock_async_sigs();
}
