/*
 * DOSEMU debugger,  1995 Max Parke <mhp@light.lightlink.com>
 *
 * This is file mhpdbg.c
 *
 * changes: ( for details see top of file mhpdbgc.c )
 *
 *   07Jul96 Hans Lermen <lermen@elserv.ffm.fgan.de>
 *   19May96 Max Parke <mhp@lightlink.com>
 *   16Sep95 Hans Lermen <lermen@elserv.ffm.fgan.de>
 *   08Jan98 Hans Lermen <lermen@elserv.ffm.fgan.de>
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include "bitops.h"
#include "emu.h"
#include "ioselect.h"
#include "cpu.h"
#include "bios.h"
#include "coopth.h"
#include "emudpmi.h"
#include "timers.h"
#include "dosemu_config.h"
#include "utilities.h"
#include "sig.h"
#define MHP_PRIVATE
#include "mhpdbg.h"

__TLS struct mhpdbg mhpdbg;
unsigned long dosdebug_flags;

static void vmhp_printf(const char *fmt, va_list args);
static void mhp_poll(void);
static void mhp_puts(char*);
static int mhp_input(void);
static void mhp_putc(char);

static char mhp_banner[] = {
  "\nDOSEMU Debugger V0.6 connected\n"
  "- type ? to get help on commands -\n"
};
struct mhpdbgc mhpdbgc ={0};

/********/
/* CODE */
/********/

static void mhp_puts(char* s)

{
   int need_send = 0;
   for (;;){
	   if (*s == 0x00)
		   break;
	   if (*s == '\n')
		   need_send++;
	   mhp_putc (*s++);
   }
   if (need_send)
	mhp_send();
}

static void mhp_putc(char c1)

{

#if 0
   if (c1 == '\n') {
      mhpdbg.sendbuf[mhpdbg.sendptr] = '\r';
      if (mhpdbg.sendptr < SRSIZE-1)
	 mhpdbg.sendptr++;
   }
#endif
   mhpdbg.sendbuf[mhpdbg.sendptr] = c1;
   if (mhpdbg.sendptr < SRSIZE-1)
      mhpdbg.sendptr++;

}

void mhp_send(void)
{
  if (mhpdbg.sendptr) {
    if (mhpdbg.fdout != -1) {
      write(mhpdbg.fdout, mhpdbg.sendbuf, mhpdbg.sendptr);
      if (mhpdbg.sendptr < SRSIZE - 1) {
        mhpdbg.sendbuf[mhpdbg.sendptr] ='\0';
        B_printf("MHP:>\n%s", mhpdbg.sendbuf);
      }
    }

    mhpdbg.sendptr = 0;
  }
}

static char *pipename_in, *pipename_out;

void mhp_close(void)
{
   int err;
   if (mhpdbg.fdin == -1) return;
   if (mhpdbg.active) {
     mhp_putc(1); /* tell debugger terminal to also quit */
     mhp_send();
   }
   remove_from_io_select(mhpdbg.fdin);
   if (pipename_in) {
     err = unlink_under(dosemu_rundir_path, strrchr(pipename_in, '/') + 1);
     if (err)
       perror("unlink()");
     free(pipename_in);
   }
   if (pipename_out) {
     err = unlink_under(dosemu_rundir_path, strrchr(pipename_out, '/') + 1);
     if (err)
       perror("unlink()");
     free(pipename_out);
   }
   closedir_under(dosemu_rundir_path);
   mhpdbg.fdin = mhpdbg.fdout = -1;
   mhpdbg.active = 0;
}

static int wait_for_debug_terminal = 0;

int vmhp_log_intercept(const char *fmt, va_list args)
{
  if (mhpdbg.active <= 1)
    return 0;
  if (dosdebug_flags & DBGF_LOG_TO_BREAK)
    mhp_regex(fmt, args);
  return 0;
}

static void mhp_input_async(int fd, void *arg)
{
  int rc = mhp_input();
  if (rc > 0)
    ioselect_complete(fd);
}

static void mhp_init(void)
{
  int retval;

  mhpdbg_trace_init();

  mhpdbg.fdin = mhpdbg.fdout = -1;
  mhpdbg.active = 0;
  mhpdbg.sendptr = 0;

  memset(&mhpdbg.intxxtab, 0, sizeof(mhpdbg.intxxtab));
  memset(&mhpdbgc.intxxalt, 0, sizeof(mhpdbgc.intxxalt));

  retval = asprintf(&pipename_in, "%s/dosemu.dbgin.%d", dosemu_rundir_path, getpid());
  assert(retval != -1);

  retval = asprintf(&pipename_out, "%s/dosemu.dbgout.%d", dosemu_rundir_path, getpid());
  assert(retval != -1);

  retval = mkfifo(pipename_in, S_IFIFO | 0600);
  if (!retval) {
    retval = mkfifo(pipename_out, S_IFIFO | 0600);
    if (!retval) {
      /* O_NONBLOCK avoids blocking of open() itself */
      mhpdbg.fdin = open(pipename_in, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (mhpdbg.fdin != -1) {
        /* NOTE: need to open read/write else it will block */
        mhpdbg.fdout = open(pipename_out, O_RDWR | O_CLOEXEC);
        if (mhpdbg.fdout != -1) {
          /* Remove O_NONBLOCK. O_CLOEXEC unaffected. */
          fcntl(mhpdbg.fdin, F_SETFL, 0);
          add_to_io_select(mhpdbg.fdin, mhp_input_async, NULL);
        }
        else {
          close(mhpdbg.fdin);
          mhpdbg.fdin = -1;
        }
      }
    }
  }
  if (retval || mhpdbg.fdin == -1 || mhpdbg.fdout == -1)
    fprintf(stderr, "Can't create debugger pipes, dosdebug not available\n");
  if (mhpdbg.fdin == -1) {
    unlink(pipename_in);
    free(pipename_in);
    unlink(pipename_out);
    free(pipename_out);
  }
  else {
    if (dosdebug_flags & DBGF_WAIT_ON_STARTUP) {
      /* don't fiddle with select, just poll until the terminal
       * comes up to send the first input
       */
       mhpdbg.nbytes = -1;
       wait_for_debug_terminal = 1;
       mhp_input();
    }
  }

  mhpdbgc_init();
}

static void reopen_fdin(void)
{
  remove_from_io_select(mhpdbg.fdin);
  close(mhpdbg.fdin);
  mhpdbg.fdin = open(pipename_in, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (mhpdbg.fdin != -1)
    add_to_io_select(mhpdbg.fdin, mhp_input_async, NULL);
}

static int mhp_input(void)
{
  if (mhpdbg.fdin == -1)
    return -1;

  mhpdbg.nbytes = read(mhpdbg.fdin, mhpdbg.recvbuf, SRSIZE);

  if (mhpdbg.nbytes == -1) {
    error("mhp read(): %s", strerror(errno));
    return -1;
  }

  if (mhpdbg.nbytes == 0 && !wait_for_debug_terminal) {
    if (mhpdbgc.stopped) {
      mhp_cmd("g");
      mhp_send();
    }
    mhpdbg.active = 0;
    reopen_fdin();
    return 0;
  }

  if (mhpdbg.nbytes < SRSIZE - 1) {
    mhpdbg.recvbuf[mhpdbg.nbytes] = '\0';
    B_printf("MHP:< %s\n", mhpdbg.recvbuf);
  }

  if (!mhpdbg.active) {
    mhpdbg.active = 1; /* 1 = new session */
  }
  return mhpdbg.nbytes;
}

static void mhp_poll_loop(void)
{
   static int in_poll_loop;
   char *ptr, *ptr1;
   if (in_poll_loop) {
      error("mhp_poll_loop() reentered\n");
      return;
   }
   in_poll_loop++;
   for (;;) {
      handle_signals();
      coopth_run();
      /* NOTE: if there is input on mhpdbg.fdin, as result of handle_signals
       *       io_select() is called and this then calls mhp_input.
       *       ( all clear ? )
       */
      if (mhpdbg.nbytes <= 0) {
         if (traceloop && mhpdbgc.stopped) {
           mhpdbg.nbytes=strlen(loopbuf);
           memcpy(mhpdbg.recvbuf,loopbuf,mhpdbg.nbytes+1);
         }
         else {
          if (mhpdbgc.stopped) {
            usleep(JIFFIE_TIME/10);
            continue;
          }
          else break;
        }
      }
      else {
        if (traceloop) { traceloop=loopbuf[0]=0; }
      }
      if ((mhpdbg.recvbuf[0] == 'q') && (mhpdbg.recvbuf[1] <= ' ')) {
	 if (mhpdbgc.stopped) {
	   mhp_cmd("g");
	   mhp_send();
	 }
	 mhpdbg.active = 0;
	 mhpdbg.sendptr = 0;
         mhpdbg.nbytes = 0;
         break;
      }
      mhpdbg.recvbuf[mhpdbg.nbytes] = 0x00;
      ptr = (char *)mhpdbg.recvbuf;
      while (ptr && *ptr) {
	ptr1 = strsep(&ptr, "\r\n");
	if (!ptr1)
	  ptr1 = ptr;
	if (!ptr1)
	  break;
        mhp_cmd(ptr1);
        mhp_send();
      }
      mhpdbg.nbytes = 0;
   }
   in_poll_loop--;
}

static void mhp_pre_vm86(void)
{
    if (!mhpdbg.active)
	return;
    if ((!in_dpmi_pm() && isset_TF()) ||
	    (in_dpmi_pm() && dpmi_mhp_issetTF())) {
	if (mhpdbgc.trapip != mhp_getcsip_value()) {
	    mhpdbgc.trapcmd = 0;
	    mhpdbgc.stopped = 1;
	    mhp_poll();
	}
    }
    if (mhpdbgc.stopped)
	mhp_poll();
}

static void mhp_poll(void)
{

  if (!mhpdbg.active) {
     mhpdbg.nbytes = 0;
     return;
  }

  if (mhpdbg.active == 1) {
    /* new session has started */
    mhpdbg.active++;

    mhp_printf ("%s", mhp_banner);
    mhp_poll_loop();
  }
  if (mhpdbgc.want_to_stop) {
    mhpdbgc.stopped = 1;
    mhpdbgc.want_to_stop = 0;
  }
  if (mhpdbgc.stopped) {
      if (dosdebug_flags & DBGF_LOG_TEMPORARY) {
         dosdebug_flags &= ~DBGF_LOG_TEMPORARY;
	 mhp_cmd("log off");
      }
      mhp_cmd("r0");
      mhp_send();
  }
  mhp_poll_loop();
}

static void mhp_boot(void)
{

  if (!wait_for_debug_terminal) {
     mhpdbg.nbytes = 0;
     return;
  }

  wait_for_debug_terminal = 0;
  mhp_poll_loop();
  mhpdbgc.want_to_stop = 1;
}

void mhp_intercept_log(const char *flags, int temporary)
{
   char buf[255];
   sprintf(buf, "log %s", flags);
   mhp_cmd(buf);
   mhp_cmd("log on");
   if (temporary)
      dosdebug_flags |= DBGF_LOG_TEMPORARY;
}

void mhp_intercept(const char *msg, const char *logflags)
{
   if (!mhpdbg.active || (mhpdbg.fdin == -1)) return;
   mhpdbgc.stopped = 1;
   mhpdbgc.want_to_stop = 0;
   traceloop = 0;
   mhp_printf("%s", msg);
   mhp_cmd("r0");
   mhp_send();
   if (!(dosdebug_flags & DBGF_IN_LEAVEDOS)) {
     if (logflags)
       mhp_intercept_log(logflags, 1);
     return;
   }
   mhp_poll_loop();
}

void mhp_exit_intercept(int errcode)
{
   char buf[255];
   if (!errcode || !mhpdbg.active || (mhpdbg.fdin == -1) ) return;

   sprintf(buf, "\n****\nleavedos(%d) called, at termination point of DOSEMU\n****\n\n", errcode);
   dosdebug_flags |= DBGF_IN_LEAVEDOS;
   mhp_intercept(buf, NULL);
}

int mhp_revectored(int inum)
{
    return test_bit(inum, mhpdbg.intxxtab);
}

void mhp_adjust_revectored(int inum)
{
    set_bit(inum, mhpdbgc.intxxalt);
}

unsigned int mhp_debug(unsigned code, unsigned int parm1, unsigned int parm2)
{
  int rtncd = 0;
#if 0
  return rtncd;
#endif
  mhpdbgc.currcode = code;
  switch (DBG_TYPE(mhpdbgc.currcode)) {
  case DBG_INIT:
	  mhp_init();
	  break;
  case DBG_BOOT:
	  mhp_boot();
	  break;
  case DBG_INTx:
	  if (!mhpdbg.active)
	     break;
	  if (test_bit(DBG_ARG(mhpdbgc.currcode), mhpdbg.intxxtab)) {
	    if ((mhpdbgc.bpload==1) && (DBG_ARG(mhpdbgc.currcode) == 0x21) && ((LWORD(eax) & 0xff0f) == 0x4b00) ) {
	      mhpdbgc.bpload_bp = SEGOFF2LINEAR(SREG(cs), LWORD(eip));
	      if (mhp_setbp(mhpdbgc.bpload_bp)) {
		Bit16u int_op = READ_WORD(SEGOFF2LINEAR(SREG(cs), LWORD(eip) - 2));
		mhp_printf("bpload: intercepting EXEC\n");
		if (int_op == 0x21cd) {
			mhp_modify_eip(-2);
			mhp_cmd("r");
			mhp_modify_eip(2);
		}
		mhp_bpset();
		mhpdbgc.bpload++;
		mhpdbgc.bpload_par = MK_FP32(BIOSSEG, DBGload_parblock);
		MEMCPY_2UNIX(mhpdbgc.bpload_par, SEGOFF2LINEAR(SREG(es), LWORD(ebx)), 14);
		MEMCPY_2UNIX(mhpdbgc.bpload_cmdline, PAR4b_addr(commandline_ptr), 128);
		MEMCPY_2UNIX(mhpdbgc.bpload_cmd, SEGOFF2LINEAR(SREG(ds), LWORD(edx)), 128);

		SREG(es) = BIOSSEG;
		LWORD(ebx) = DBGload_parblock;
		LWORD(eax) = 0x4b01; /* load, but don't execute */
	      } else {
		mhp_printf("bpload: ??? #1\n");
		mhp_cmd("r");

	        mhpdbgc.bpload_bp=0;
	        mhpdbgc.bpload=0;
	      }
	      if (!--mhpdbgc.int21_count) {
	        volatile register int i=0x21; /* beware, set_bit-macro has wrong constraints */
	        clear_bit(i, mhpdbg.intxxtab);
	        if (test_bit(i, mhpdbgc.intxxalt)) {
	          clear_bit(i, mhpdbgc.intxxalt);
	          clear_bit(i, &vm86s.int_revectored);
	        }
	      }
	    }
	    else {
	      if ((DBG_ARG(mhpdbgc.currcode) != 0x21) || !mhpdbgc.bpload ) {
	        mhpdbgc.stopped = 1;
	        if (parm1)
	          LWORD(eip) -= 2;
	        mhpdbgc.int_handled = 0;
	        if (!parm2) {
	          mhp_poll();
		} else {
		  mhp_cmd("r0");
		}
	        if (mhpdbgc.int_handled)
	          rtncd = 1;
	        else if (parm1)
	          LWORD(eip) += 2;
	      }
	    }
	  }
	  break;
  case DBG_INTxDPMI:
	  if (!mhpdbg.active) break;
          mhpdbgc.stopped = 1;
#if WITH_DPMI
          dpmi_mhp_intxxtab[DBG_ARG(mhpdbgc.currcode) & 0xff] &= ~2;
#endif
	  break;
  case DBG_TRAP:
	  if (!mhpdbg.active)
	     break;
	  if (DBG_ARG(mhpdbgc.currcode) == 1 && mhpdbgc.trapcmd) { /* single step */
                  switch (mhpdbgc.trapcmd) {
		  case 2: /* t command -- step until IP changes */
			  if (mhpdbgc.trapip == mhp_getcsip_value())
				  break;
			  /* no break */
		  case 1: /* ti command */
			  mhpdbgc.trapcmd = 0;
			  mhpdbgc.stopped = 1;
			  break;
		  }
		  rtncd = 1;	// suppress int 1

		  if (traceloop && mhp_bpchk(mhp_getcsip_value())) {
			  traceloop = 0;
			  loopbuf[0] = '\0';
		  }
	  }

	  if (DBG_ARG(mhpdbgc.currcode) == 3) { /* int3 (0xCC) */
		  int ok=0;
		  unsigned int csip=mhp_getcsip_value() - 1;
		  if (mhpdbgc.bpload_bp == csip ) {
		    /* mhp_cmd("r"); */
		    switch (mhpdbgc.bpload) {
		    case 2:
		      mhp_modify_eip(-1);
		      mhp_printf("bpload: INT3 caught at %x:%x\n", _CS, _IP);
		      SREG(cs)=BIOSSEG;
		      LWORD(eip) = DBGload_OFF;
		      mhpdbgc.trapcmd = 1;
		      mhpdbgc.bpload++;
		      break;
		    case 3:
		      mhp_clearbp(mhpdbgc.bpload_bp);
		      mhp_modify_eip(-1);
		      mhp_printf("bpload: program exited\n");
		      mhpdbgc.bpload = 0;
		      ok = 1;
		      break;
		    default:
		      error("wrong bpload state %i\n", mhpdbgc.bpload);
		    }
		  }
		  else {
		    if ((ok = mhp_bpchk(csip))) {
			  mhp_modify_eip(-1);
		    }
		    else {
		      if ((ok=test_bit(3, mhpdbg.intxxtab))) {
		        /* software programmed INT3 */
		        mhp_modify_eip(-1);
		        mhp_cmd("r");
		        mhp_modify_eip(+1);
		      }
		    }
		  }
		  if (ok) {
		    mhpdbgc.trapcmd = 0;
		    rtncd = 1;
		    mhpdbgc.stopped = 1;
		  }
	  }
	  break;
  case DBG_PRE_VM86:
	  mhp_pre_vm86();
	  break;
  case DBG_POLL:
	  mhp_poll();
	  break;
  case DBG_GPF:
	  if (!mhpdbg.active)
	     break;
	  mhpdbgc.stopped = 1;
	  mhp_poll();
	  break;
  default:
	  break;
  }
  return rtncd;
}

static void vmhp_printf(const char *fmt, va_list args)
{
  char frmtbuf[SRSIZE];

  vsprintf(frmtbuf, fmt, args);

  mhp_puts(frmtbuf);
}

void mhp_printf(const char *fmt,...)
{
  va_list args;

  va_start(args, fmt);
  vmhp_printf(fmt, args);
  va_end(args);
}

int mhpdbg_is_stopped(void)
{
  return mhpdbgc.stopped;
}
