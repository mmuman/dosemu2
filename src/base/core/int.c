#include <stdio.h>
#include <string.h>
#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif
#include <sys/statvfs.h>
#include "Linux/magic.h"
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <glob.h>

#include "version.h"

#include "emu.h"
#include "serial.h"
#include "memory.h"
#include "timers.h"
#include "mouse.h"
#include "disks.h"
#include "bios.h"
#include "iodev.h"
#include "pic.h"
#include "lpt.h"
#include "bitops.h"
#include "hma.h"
#include "xms.h"
#include "int.h"
#include "vint.h"
#include "dos2linux.h"
#include "video.h"
#include "clipboard.h"
#include "priv.h"
#include "doshelpers.h"
#include "lowmem.h"
#include "plugin_config.h"
#include "utilities.h"
#include "redirect.h"
#include "pci.h"
#include "joystick.h"
#include "aspi.h"
#include "vgaemu.h"
#include "hlt.h"
#include "coopth.h"
#include "mhpdbg.h"
#include "ipx.h"
#ifdef X86_EMULATOR
#include "cpu-emu.h"
#endif

#include "emudpmi.h"

#include "keyboard/keyb_server.h"

#undef  DEBUG_INT1A

#if WINDOWS_HACKS
enum win3x_mode_enum win3x_mode;
#endif

static char win3x_title[256];

static void dos_post_boot(void);
static int post_boot;

static int int33(void);
static int _int66_(int, int);
static void do_rvc_chain(int i, int stk_offs);
static void int21_rvc_setup(void);
static void int2f_rvc_setup(void);
static void int33_rvc_setup(void);
static void revect_setup(void);
#define run_caller_func(i, revect, arg) \
    int_handlers[i].interrupt_function[revect](arg, revect)
#define run_secrevect_func(i, arg1, arg2) \
    int_handlers[i].secrevect_function(arg1, arg2)
static void redirect_devices(void);
static int enable_redirect(void);
static int do_redirect(int old_only);
static int redir_it(void);
static void debug_int(const char *s, int i);

static int msdos_remap_extended_open(void);
static int rehash_redir_groups(void);

typedef int interrupt_function_t(int, int);
enum { NO_REVECT, REVECT, INTF_MAX };
typedef void revect_function_t(void);
typedef far_t unrevect_function_t(uint16_t, uint16_t);
typedef void secrevect_function_t(uint16_t, uint16_t);
static struct {
    interrupt_function_t *interrupt_function_arr[INTF_MAX][INTF_MAX];
    #define interrupt_function interrupt_function_arr[cur_rvc_setup()]
    secrevect_function_t *secrevect_function;
    revect_function_t *revect_function;
    unrevect_function_t *unrevect_function;
} int_handlers[0x100];

/* set if some directories are mounted during startup */
static int redir_state;
static int redir_state2;

static char title_hint[9] = "";
static char title_current[TITLE_APPNAME_MAXLEN];
static int can_change_title = 0;
static u_short hlt_off;
static int int_tid, int_rvc_tid;

static int cur_rvc_setup(void)
{
    if (config.int_hooks == -1 || config.int_hooks == 1)
	return REVECT;
    return NO_REVECT;
}

u_short INT_OFF(u_char i)
{
    return ((BIOS_HLT_BLK_SEG << 4) + i + hlt_off);
}

void jmp_to(int cs, int ip)
{
    SREG(cs) = cs;
    REG(eip) = ip;
}

static void change_window_title(char *title)
{
    if (Video->change_config)
	Video->change_config(CHG_TITLE_APPNAME, title);
}

static void kill_time(long usecs)
{
    hitimer_t t_start;

    t_start = GETusTIME(0);
    while (GETusTIME(0) - t_start < usecs) {
	set_IF();
	coopth_wait();
	clear_IF();
    }
}

static void mbr_jmp(void *arg)
{
    unsigned offs = (long) arg;

    LWORD(esp) = 0x7c00;
    SREG(cs) = SREG(ds) = SREG(es) = SREG(ss) = 0;
    LWORD(edi) = 0x7dfe;
    LWORD(eip) = 0x7c00;
    LWORD(ebp) = LWORD(esi) = offs;
}

static void process_master_boot_record(void)
{
    /* Ok, _we_ do the MBR code in 32-bit C code,
     * so this obviously is _not_ stolen from any DOS code ;-)
     *
     * Now, what _does_ the original MSDOS MBR?
     * 1. It moves itself down to 0:0x600
     * 2. It sets DS,ES,SS to the new segment (0 in this case)
     * 3. It sets the stack pinter to just below the loaded MBR (SP=0x7c00)
     * 4. It searches for a partition having the bootflag set (=0x80)
     * 5. It loads the bootsector of this partition to 0:0x7c00
     * 6. It does a long jump to 0:0x7c00, with following registers set:
     *    SS:BP,DS:SI pointing to the boot partition entry within 0:600 MBR
     *    DI = 0x7dfe
     */
    struct on_disk_mbr *mbr = LOWMEM(0x600);
    struct on_disk_mbr *bootrec = LOWMEM(0x7c00);
    int i;
    unsigned offs;

    memcpy(mbr, bootrec, 0x200);	/* move the mbr down */

    for (i = 0; i < 4; i++) {
	if (mbr->partition[i].bootflag == 0x80)
	    break;
    }
    if (i >= 4) {
	/* aiee... no bootflags sets */
	error("no bootflag set, Leaving DOS...\n");
	leavedos(99);
    }
    LO(dx) = 0x80;		/* drive C:, DOS boots only from C: */
    if (config.hdiskboot >= 2)
	LO(dx) += config.hdiskboot - 2;
    HI(dx) = mbr->partition[i].start_head;
    LO(cx) = mbr->partition[i].start_sector;
    HI(cx) = PTBL_HL_GET(&mbr->partition[i], start_track);
    LWORD(eax) = 0x0201;	/* read one sector */
    LWORD(ebx) = 0x7c00;	/* target offset, ES is 0 */
    do_int_call_back(0x13);
    if ((REG(eflags) & CF) || (bootrec->signature != MBR_SIG)) {
	/* error while booting */
	error("error on reading bootsector, Leaving DOS...\n");
	leavedos(99);
    }

    offs = 0x600 + offsetof(struct on_disk_mbr, partition) + sizeof(mbr->partition[0]) * i;
    coopth_add_post_handler(mbr_jmp, (void *) (long) offs);
}

static void revect_helper(int stk_offs)
{
    int ah = HI(ax);
    int subh = LO(bx);
    int stk = HI(bx) + stk_offs;
    int inum = ah;
    uint16_t old_ax;
    uint16_t old_flags;

    LWORD(eax) = HWORD(eax);
    LWORD(ebx) = HWORD(ebx);
    HWORD(eax) = HWORD(ebx) = 0;
    NOCARRY;
    set_ZF();
    switch (subh) {
    case DOS_SUBHELPER_RVC_VERSION_CHECK:
	if (ah < DOSEMU_EMUFS_DRIVER_MIN_VERSION) {
	    CARRY;
	    error("emufs is too old, ver %i need %i\n", ah,
		  DOSEMU_EMUFS_DRIVER_VERSION);
	    break;
	}
	if (ah < DOSEMU_EMUFS_DRIVER_VERSION) {
	    error("emufs is too old, ver %i need %i, but trying to continue\n",
		  ah, DOSEMU_EMUFS_DRIVER_VERSION);
	    error("@To upgrade you can remove ~/.dosemu/drives and it will be re-created.\n");
	}
	break;
    case DOS_SUBHELPER_RVC_CALL:
	do_rvc_chain(inum, stk);
	break;
    case DOS_SUBHELPER_RVC2_CALL:
	old_ax = LWORD(ecx);
	LWORD(ecx) = HWORD(ecx);
	HWORD(ecx) = 0;
	old_flags = LWORD(edx);
	LWORD(edx) = HWORD(edx);
	HWORD(edx) = 0;
	di_printf("int_rvc 0x%02x, doing second revect call\n", inum);
	run_secrevect_func(inum, old_ax, old_flags);
	break;
    case DOS_SUBHELPER_RVC_NEXT_VEC: {
	int start = (ah == 0xff ? 0 : ah + 1);
	int i;
	for (i = start; i < 256; i++) {
	    if (int_handlers[i].interrupt_function[REVECT])
		break;
	}
	if (i == 256) {
	    set_ZF();
	    break;
	}
	assert(int_handlers[i].unrevect_function);
	clear_ZF();
	HI(ax) = i;
	break;
    }
    case DOS_SUBHELPER_RVC_UNREVECT: {
	far_t entry;
	if (!int_handlers[inum].unrevect_function) {
	    CARRY;
	    break;
	}
	entry = int_handlers[inum].unrevect_function(SREG(es), LWORD(edi));
	if (!entry.segment) {
	    CARRY;
	    break;
	}
	SREG(ds) = entry.segment;
	LWORD(esi) = entry.offset;
	break;
    }
    default:
	CARRY;
	break;
    }
}

static void (*clnup_handler)(void);

int register_cleanup_handler(void (*call)(void))
{
//    assert(!clnup_handler);
    clnup_handler = call;
    return 0;
}

static void emufs_helper(void)
{
    char *p, *p1, *cmdl;

    switch (LO(bx)) {
    case DOS_SUBHELPER_EMUFS_REDIRECT:
	NOCARRY;
	if (!redir_it()) {
	    CARRY;
	    break;
	}
	p = FAR2PTR(READ_DWORD(SEGOFF2LINEAR(_ES, _DI) + 18));
	p1 = strpbrk(p, "\r\n");
	if (p1)
	    cmdl = strndup(p, p1 - p);
	else
	    cmdl = strdup(p);
	p = cmdl + strlen(cmdl) - 1;
	while (*p == ' ') {
	    *p = 0;
	    p--;
	}
	p = strrchr(cmdl, ' ');
	if (p) {
	    p++;
	    if (strcasecmp(p, "/ALL") == 0)
		redirect_devices();
	}
	free(cmdl);
	break;
    case DOS_SUBHELPER_EMUFS_IOCTL:
	switch (HI(ax)) {
	case EMUFS_HELPER_REDIRECT:
	    NOCARRY;
	    if (!enable_redirect()) {
		CARRY;
		break;
	    }
	    redirect_devices();
	    break;
	case EMUFS_HELPER_REHASH_DYN:
	    NOCARRY;
	    rehash_redir_groups();
	    break;
	default:
	    CARRY;
	    break;
	}
	break;
    default:
	error("Unsupported emufs helper %i\n", LO(bx));
	CARRY;
	break;
    }
}

/* returns 1 if dos_helper() handles it, 0 otherwise */
/* dos helper and mfs startup (was 0xfe) */
static int dos_helper(int stk_offs, int revect)
{
    switch (LO(ax)) {
    case DOS_HELPER_DOSEMU_CHECK:	/* Linux dosemu installation test */
	LWORD(eax) = DOS_HELPER_MAGIC;
	LWORD(ebx) = VERSION_NUM * 0x100 + SUBLEVEL;	/* major version 0.49 -> 0049 */
	/* The patch level in the form n.n is a float...
	 * ...we let GCC at compile time translate it to 0xHHLL, HH=major, LL=minor.
	 * This way we avoid usage of float instructions.
	 */
	LWORD(ecx) = REVISION;
	LWORD(edx) = (config.X) ? 0x1 : 0;	/* Return if running under X */
	g_printf("WARNING: dosemu installation check\n");
	if (debug_level('g'))
	    show_regs();
	break;

    case DOS_HELPER_SHOW_REGS:	/* SHOW_REGS */
	show_regs();
	break;

    case DOS_HELPER_SHOW_INTS:	/* SHOW INTS, BH-BL */
	show_ints(HI(bx), LO(bx));
	break;

    case DOS_HELPER_PRINT_STRING:	/* PRINT STRING ES:DX */
	dbug_printf("DOS to EMU: \"%s\"\n", SEG_ADR((char *), es, dx));
	break;

    case DOS_HELPER_ADJUST_IOPERMS:	/* SET IOPERMS: bx=start, cx=range,
					   carry set for get, clear for release */
	{
	    int cflag = LWORD(eflags) & CF ? 1 : 0;

	    i_printf("I/O perms: 0x%04x 0x%04x %d\n", LWORD(ebx),
		     LWORD(ecx), cflag);
	    if (set_ioperm(LWORD(ebx), LWORD(ecx), cflag)) {
		error("SET_IOPERMS request failed!!\n");
		CARRY;		/* failure */
	    } else {
		if (cflag)
		    warn("WARNING! DOS can now access I/O ports 0x%04x to 0x%04x\n", LWORD(ebx), LWORD(ebx) + LWORD(ecx) - 1);
		else
		    warn("Access to ports 0x%04x to 0x%04x clear\n",
			 LWORD(ebx), LWORD(ebx) + LWORD(ecx) - 1);
		NOCARRY;	/* success */
	    }
	}
	break;

    case DOS_HELPER_REVECT_HELPER:
	revect_helper(stk_offs);
	break;

    case DOS_HELPER_CONTROL_VIDEO:	/* initialize video card */
	if (LO(bx) == 0) {
	    if (set_ioperm(0x3b0, 0x3db - 0x3b0, 0))
		warn("couldn't shut off ioperms\n");
	    SETIVEC(0x10, BIOSSEG, INT_OFF(0x10));	/* restore our old vector */
	    config.vga = 0;
	} else {
	    unsigned int ssp, sp;

	    if (!config.mapped_bios) {
		error("CAN'T DO VIDEO INIT, BIOS NOT MAPPED!\n");
		return 1;
	    }
	    if (set_ioperm(0x3b0, 0x3db - 0x3b0, 1))
		warn("couldn't get range!\n");
	    config.vga = 1;
	    warn("WARNING: jumping to 0[c/e]000:0003\n");

	    ssp = SEGOFF2LINEAR(SREG(ss), 0);
	    sp = LWORD(esp);
	    pushw(ssp, sp, SREG(cs));
	    pushw(ssp, sp, LWORD(eip));
	    LWORD(esp) -= 4;
	    SREG(cs) = config.vbios_seg;
	    LWORD(eip) = 3;
	    show_regs();
	}
	break;

    case DOS_HELPER_SHOW_BANNER:	/* show banner */
	if (config.fdisks + config.hdisks == 0) {
	    error("No drives defined, exiting\n");
	    leavedos(2);
	}
	if (config.quiet)
	    break;
	p_dos_str(PACKAGE_NAME " " VERSTR "\n");
//	p_dos_str
//	    ("Please test against a recent version before reporting bugs and problems.\n");
	p_dos_str
	    ("Get the latest code at http://dosemu2.github.io/dosemu2\n");
	p_dos_str
	    ("Submit Bugs via https://github.com/dosemu2/dosemu2/issues\n");
	p_dos_str
	    ("Ask for help in mail list: linux-msdos@vger.kernel.org\n");
	p_dos_str
	    ("This program comes with ABSOLUTELY NO WARRANTY.\n"
	     "This is free software, GPL v2 (or any later version) "
	     "distribution conditions.\n");
	p_dos_str("\n");
	break;

    case DOS_HELPER_PRESTROKES_START:
	start_pre_strokes();
	break;

    case DOS_HELPER_INSERT_INTO_KEYBUFFER:
	k_printf
	    ("KBD: WARNING: outdated keyboard helper fn 6 was called!\n");
	break;

    case DOS_HELPER_GET_BIOS_KEY:	/* INT 09 "get bios key" helper */
	_AX = get_bios_key(_AH);
	k_printf("HELPER: get_bios_key() returned %04x\n", _AX);
	break;

    case DOS_HELPER_VIDEO_INIT:
	v_printf("Starting Video initialization\n");
	_AL = config.vbios_post;
	break;

    case DOS_HELPER_GET_DEBUG_STRING:
	/* TRB - handle dynamic debug flags in dos_helper() */
	LWORD(eax) =
	    GetDebugFlagsHelper(MK_FP32(_regs.es, _regs.edi & 0xffff), 1);
	g_printf("DBG: Get flags\n");
	break;

    case DOS_HELPER_SET_DEBUG_STRING:
	g_printf("DBG: Set flags\n");
	LWORD(eax) =
	    SetDebugFlagsHelper(MK_FP32(_regs.es, _regs.edi & 0xffff));
	g_printf("DBG: Flags set\n");
	break;

    case DOS_HELPER_SET_HOGTHRESHOLD:
	g_printf("IDLE: Setting hogthreshold value to %u\n", LWORD(ebx));
	config.hogthreshold = LWORD(ebx);
	break;

    case DOS_HELPER_TEST_MODE:
	config.test_mode = LO(bx);
	return 1;

    case DOS_HELPER_MFS_HELPER:
	mfs_inte6();
	return 1;

    case DOS_HELPER_EMUFS_HELPER:
	emufs_helper();
	return 1;

#if 0
    case DOS_HELPER_DOSC:
	if (HI(ax) == 0xdc) {
	    /* install check and notify */
	    if (!dosc_interface())
		return 0;
	    running_DosC = LWORD(ebx);
	    return 1;
	}
	if (running_DosC) {
	    return dosc_interface();
	}
	return 0;
#endif

    case DOS_HELPER_EMS_HELPER:
	ems_helper();
	return 1;

    case DOS_HELPER_EMS_BIOS:
	{
	    LWORD(eax) = HWORD(eax);
	    E_printf
		("EMS: in 0xe6,0x22 handler! ax=0x%04x, bx=0x%04x, dx=0x%04x, "
		 "cx=0x%04x\n", LWORD(eax), LWORD(ebx), LWORD(edx),
		 LWORD(ecx));
	    if (config.ems_size)
		ems_fn(&REGS);
	    else {
		error("EMS: not running ems_fn!\n");
		return 0;
	    }
	    break;
	}

    case DOS_HELPER_XMS_HELPER:
	xms_helper();
	return 1;

    case DOS_HELPER_GARROT_HELPER:	/* Mouse garrot helper */
	if (!LWORD(ebx))	/* Wait sub-function requested */
	    idle_enable(50, 0, "mouse_garrot");
	else {			/* Get Hogthreshold value sub-function */
	    LWORD(ebx) = config.hogthreshold;
	    LWORD(eax) = config.hogthreshold;
	}
	break;

    case DOS_HELPER_SERIAL_HELPER:	/* Serial helper */
	serial_helper();
	break;

    case DOS_HELPER_MOUSE_HELPER:{
	    uint8_t *p = MK_FP32(BIOSSEG, bios_in_int10_callback);

	    switch (LWORD(ebx)) {
	    case DOS_SUBHELPER_MOUSE_START_VIDEO_MODE_SET:
		/* Note: we hook int10 very late, after display.sys already hooked it.
		 * So when we call previous handler in bios.S, we actually call
		 * display.sys's one, which will call us again.
		 * So have to protect ourselves from re-entrancy. */
		*p = 1;
		break;
	    case DOS_SUBHELPER_MOUSE_END_VIDEO_MODE_SET:
		*p = 0;
		break;
	    }
#if WINDOWS_HACKS
	    if (win3x_mode != INACTIVE) {
		/* work around win.com's small stack that gets overflown when
		 * display.sys's int10 handler calls too many things with hw interrupts
		 * enabled. */
		uint16_t new_ss, new_sp;
		int switched, to_copy;
		uint64_t cookie;
		uint8_t *stk, *new_stk;
		switch (LWORD(ebx)) {
		case DOS_SUBHELPER_MOUSE_START_VIDEO_MODE_SET:
		    to_copy = _min(64, (0x10000 - _SP) & 0xffff);
		    switched = get_rm_stack(&new_ss, &new_sp,
			    ((uint64_t)to_copy << 32) | ((unsigned)_SS << 16) |
			    _SP);
		    if (switched) {
			stk = SEG_ADR((uint8_t *), ss, sp);
			SREG(ss) = new_ss;
			LWORD(esp) = new_sp - to_copy;
			new_stk = SEG_ADR((uint8_t *), ss, sp);
			memcpy(new_stk, stk, to_copy);
		    }
		    break;
		case DOS_SUBHELPER_MOUSE_END_VIDEO_MODE_SET:
		    new_sp = put_rm_stack(&cookie);
		    if (new_sp) {
			uint16_t old_ss = (cookie >> 16) & 0xffff;
			uint16_t old_sp = cookie & 0xffff;
			int sp_delta;
			to_copy = cookie >> 32;
			sp_delta = LWORD(esp) + to_copy - new_sp;
			stk = SEG_ADR((uint8_t *), ss, sp);
			new_stk =
			    LINEAR2UNIX(SEGOFF2LINEAR(old_ss, old_sp) +
					sp_delta);
			memcpy(new_stk, stk, to_copy - sp_delta);
			SREG(ss) = old_ss;
			LWORD(esp) = old_sp + sp_delta;
		    } else {
			error("SS changed by video mode set\n");
		    }
		    break;
		}
	    }
#endif
	    mouse_helper(&vm86s.regs);
	    break;
	}

    case DOS_HELPER_CDROM_HELPER:{
	    E_printf
		("CDROM: in 0x40 handler! ax=0x%04x, bx=0x%04x, dx=0x%04x, "
		 "cx=0x%04x\n", LWORD(eax), LWORD(ebx), LWORD(edx),
		 LWORD(ecx));
	    cdrom_helper(NULL, NULL, 0);
	    break;
	}

    case DOS_HELPER_ASPI_HELPER:{
	    A_printf
		("ASPI: in 0x41 handler! ax=0x%04x, bx=0x%04x, dx=0x%04x, "
		 "cx=0x%04x\n", LWORD(eax), LWORD(ebx), LWORD(edx),
		 LWORD(ecx));
	    aspi_helper(HI(ax));
	    break;
	}

    case DOS_HELPER_TCP_HELPER:
	tcp_helper(&vm86s.regs);
	break;

    case DOS_HELPER_IPX_HELPER:
	ipx_helper(&vm86s.regs);
	break;

    case DOS_HELPER_GET_CPU_SPEED:
	{
	    REG(eax) = 0;
	    break;
	}

    case DOS_HELPER_GET_TERM_TYPE:
	{
	    u_short i;
	    int co, li;

	    i = config.console_keyb;
	    if (config.X || config.sdl)
		i |= 8;
	    if (config.console_video)
		i |= 0x10;
	    if (config.vga)
		i |= 0x20;
	    if (config.dualmon)
		i |= 0x40;
	    if (config.term)
		i |= 0x80;
	    if (config.dumb_video)
		i |= 0x100;
	    LWORD(eax) = i;
	    if (config.term) {
		gettermcap(0, &co, &li);
	    } else {
		co = vga.text_width;
		li = vga.text_height;
	    }
	    LWORD(ebx) = co;
	    LWORD(ecx) = li;
	    break;
	}

    case DOS_HELPER_GETCWD:
	{
	    char *buf =
		getcwd(SEG_ADR((char *), es, dx), (size_t) LWORD(ecx));
	    LWORD(eax) =
		buf == NULL ? 0 : (SEGOFF2LINEAR(_ES, _DX) & 0xffff);
	    break;
	}
    case DOS_HELPER_GETPID: {
	pid_t pid = getpid();
	LWORD(eax) = pid;
	LWORD(ebx) = pid >> 16;
	break;
    }

    case DOS_HELPER_CHDIR:
	LWORD(eax) = chdir(SEG_ADR((char *), es, dx));
	break;
#ifdef X86_EMULATOR
    case DOS_HELPER_CPUEMUON:
	config.cpu_vm = CPUVM_EMU;
	config.cpu_vm_dpmi = CPUVM_EMU;
	break;
    case DOS_HELPER_CPUEMUOFF:
	/* FIXME: dunno to what cpu_vm to switch */
	if (IS_EMU()) {
	    error("unsupported emuoff helper\n");
	}
	break;
#endif
    case DOS_HELPER_XCONFIG:
	if (Video->change_config) {
	    LWORD(eax) =
		Video->change_config((unsigned) LWORD(edx),
				     SEG_ADR((void *), es, bx));
	} else {
	    _AX = -1;
	}
	break;
    case DOS_HELPER_BOOTSECT:
	coopth_leave();
	mimic_boot_blk();
	break;
    case DOS_HELPER_READ_MBR:
	boot();
	break;
    case DOS_HELPER_MBR:
	process_master_boot_record();
	break;
    case DOS_HELPER_EXIT:
	if (LWORD(eax) == DOS_HELPER_REALLY_EXIT) {
	    /* terminate code is in bx */
	    dbug_printf("DOS termination requested\n");
	    if (!config.dumb_video)
		p_dos_str("\n\rLeaving DOS...\n\r");
	    leavedos(LO(bx));
	}
	break;

#ifdef USE_COMMANDS_PLUGIN
    case DOS_HELPER_COMMANDS:
	if (!commands_plugin_inte6())
	    return 0;
	break;
    case DOS_HELPER_COMMANDS_DONE:
	if (!commands_plugin_inte6_done())
	    return 0;
	break;
    case DOS_HELPER_SET_RETCODE:
	if (!commands_plugin_inte6_set_retcode())
	    return 0;
	break;
#endif

    default:
	error("bad dos helper function: AX=0x%04x\n", LWORD(eax));
	return 0;
    }

    return 1;
}

static int int15(void)
{
    int num;

    if (HI(ax) != 0x4f)
	NOCARRY;

    switch (HI(ax)) {
    case 0x10:			/* TopView/DESQview */
	switch (LO(ax)) {
	case 0x00:{		/* giveup timeslice */
		idle_enable(100, 0, "topview");
		break;
	    }
	}
	CARRY;
	break;

    case 0x24:			/* PS/2 A20 gate support */
	switch (LO(ax)) {
	case 0:		/* disable A20 gate */
	    set_a20(0);
	    HI(ax) = 0;
	    NOCARRY;
	    break;

	case 1:		/* enable A20 gate */
	    set_a20(1);
	    HI(ax) = 0;
	    NOCARRY;
	    break;

	case 2:		/* get A20 gate status */
	    HI(ax) = 0;
	    LO(ax) = a20;
	    LWORD(ecx) = 0;
	    NOCARRY;
	    break;

	case 3:		/* query A20 gate support */
	    HI(ax) = 0;
	    LWORD(ebx) = 3;
	    NOCARRY;
	    break;

	default:
	    HI(ax) = 0x86;
	    CARRY;
	}
	break;

    case 0x41:			/* wait on external event */
	break;
    case 0x4f:			/* Keyboard intercept */
	HI(ax) = 0x86;
	/*k_printf("INT15 0x4f CARRY=%x AX=%x\n", (LWORD(eflags) & CF),LWORD(eax)); */
	k_printf("INT15 0x4f CARRY=%x AX=%x\n", (_FLAGS & CF), LWORD(eax));
	CARRY;
/*
    if (LO(ax) & 0x80 )
      if (1 || !(LO(ax)&0x80) ){
	fprintf(stderr, "Carrying it out\n");
        CARRY;
      }
      else
	NOCARRY;
*/
	break;

    case 0x80:			/* default BIOS hook: device open */
    case 0x81:			/* default BIOS hook: device close */
    case 0x82:			/* default BIOS hook: program termination */
	HI(ax) = 0;
	break;

    case 0x83:
	h_printf("int 15h event wait:\n");
	show_regs();
	CARRY;
	break;			/* no event wait */
    case 0x84:
	joy_bios_read();
	break;
    case 0x85:
	num = LWORD(eax) & 0xFF;	/* default bios handler for sysreq key */
	if (num == 0 || num == 1) {
	    LWORD(eax) &= 0x00FF;
	    break;
	}
	LWORD(eax) &= 0xFF00;
	LWORD(eax) |= 1;
	CARRY;
	break;
    case 0x86:
	/* wait...cx:dx=time in usecs */
	g_printf("doing int15 wait...ah=0x86\n");
	show_regs();
	kill_time((long) ((LWORD(ecx) << 16) | LWORD(edx)));
	NOCARRY;
	break;

    case 0x87:{
	    unsigned int *lp;
	    unsigned src_addr, dst_addr;
	    unsigned src_limit, dst_limit;
	    unsigned int length;
	    void *s, *d;
	    unsigned int old_a20 = a20;

	    lp = SEG_ADR((unsigned int *), es, si);
	    lp += 4;
	    src_addr = (*lp >> 16) & 0x0000FFFF;
	    src_limit = *lp & 0x0000FFFF;
	    lp++;
	    src_addr |= (*lp & 0xFF000000) | ((*lp << 16) & 0x00FF0000);
	    src_limit |= (*lp & 0x000F0000);
	    lp++;
	    dst_addr = (*lp >> 16) & 0x0000FFFF;
	    dst_limit = *lp & 0x0000FFFF;
	    lp++;
	    dst_addr |= (*lp & 0xFF000000) | ((*lp << 16) & 0x00FF0000);
	    dst_limit |= (*lp & 0x000F0000);

	    length = LWORD(ecx) << 1;

	    x_printf("int 15: block move: src=%#x dst=%#x len=%#x\n",
		     src_addr, dst_addr, length);

	    if (src_limit < length - 1 || dst_limit < length - 1 ||
		src_addr + length > LOWMEM_SIZE + EXTMEM_SIZE ||
		dst_addr + length > LOWMEM_SIZE + EXTMEM_SIZE) {
		x_printf("block move failed\n");
		LWORD(eax) = 0x0200;
		CARRY;
		break;
	    }
	    /* Have to enable a20 before translating addresses */
	    if (!a20)
		set_a20(1);
	    while (length) {
		/* avoid crossing page boundaries */
		int s_al = PAGE_ALIGN(src_addr) - src_addr;
		int d_al = PAGE_ALIGN(dst_addr) - dst_addr;
		int todo;

		if (!s_al)
		    s_al += PAGE_SIZE;
		if (!d_al)
		    d_al += PAGE_SIZE;
		todo = _min(s_al, d_al);
		todo = _min(todo, length);
		x_printf("int 15: copy subblock: src=%#x dst=%#x len=%#x\n",
		     src_addr, dst_addr, todo);
		s = physaddr_to_unixaddr(src_addr);
		if (s == MAP_FAILED) {
		    error("error mapping %x to addr\n", src_addr);
		    break;
		}
		d = physaddr_to_unixaddr(dst_addr);
		if (d == MAP_FAILED) {
		    error("error mapping %x to addr\n", dst_addr);
		    break;
		}
		e_invalidate_pa(dst_addr, todo);
		memcpy(d, s, todo);
		src_addr += todo;
		dst_addr += todo;
		length -= todo;
	    }
	    if (old_a20 != a20)
		set_a20(old_a20);
	    if (length) {
		LWORD(eax) = 0x0200;
		CARRY;
	    } else {
		LWORD(eax) = 0;
		NOCARRY;
	    }
	    break;
	}

    case 0x88:
	LWORD(eax) = EXTMEM_SIZE >> 10;
	NOCARRY;
	break;

    case 0x89:			/* enter protected mode : kind of tricky! */
	LWORD(eax) |= 0xFF00;	/* failed */
	CARRY;
	break;
    case 0x90:			/* no device post/wait stuff */
	CARRY;
	break;
    case 0x91:
	CARRY;
	break;
    case 0xbf:			/* DOS/16M,DOS/4GW */
	switch (REG(eax) &= 0x00FF) {
	case 0:
	case 1:
	case 2:		/* installation check */
	default:
	    REG(edx) = 0;
	    CARRY;
	    break;
	}
	break;
    case 0xc0:
	SREG(es) = ROM_CONFIG_SEG;
	LWORD(ebx) = ROM_CONFIG_OFF;
	HI(ax) = 0;
	break;
    case 0xc1:
	CARRY;
	break;			/* no ebios area */
    case 0xc2:
	mouse_ps2bios();
	break;
    case 0xc3:
	/* no watchdog */
	CARRY;
	break;
    case 0xc4:
	/* no post */
	CARRY;
	break;
    case 0xc9:
	if (LO(ax) == 0x10) {
	    HI(ax) = 0;
	    HI(cx) = vm86s.cpu_type;
	    LO(cx) = 0x20;
	    break;
	}
	/* else fall through */
    case 0xd8:			/* EISA - should be set in config? */
    case 0xda:
    case 0xdb:
	HI(ax) = 0x86;
	CARRY;
	break;

    case 0xe8:
#if 0
      -- -- -- --b - 15E801-- -- -- -- -- -- -- -- -- -- -- -- -- -- -INT 15 - Phoenix BIOS v4 .0 - GET MEMORY SIZE FOR > 64 M CONFIGURATIONS AX = E801h Return:CF clear if
	    successful
	    AX = extended memory between 1 M and 16 M, in K(max 3 C00h =
							    15 MB)
	  BX = extended memory above 16 M, in 64 K blocks CX = configured memory 1 M to 16 M, in K DX = configured memory above 16 M, in 64 K blocks CF set on error Notes:supported by the A03 level(6 / 14 /
					   94) and later XPS P90 BIOSes,
		as well as the Compaq Contura, 3 / 8 / 93 DESKPRO / i,
		and 7 / 26 /
		93 LTE Lite 386 ROM BIOS supported by AMI BIOSes dated 8 /
		23 / 94 or later on some systems, the BIOS returns AX =
		BX = 0000 h;
	in this case,
	    use CX and DX instead of AX and BX this interface is used by
	    Windows NT 3.1, OS / 2 v2 .11 / 2.20,
	    and is used as a fall - back by newer versions if AX = E820h
    is not supported SeeAlso:
	    AH = 8 Ah "Phoenix", AX = E802h, AX = E820h, AX =
		E881h "Phoenix"-- --
		-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#endif
	    if (LO(ax) == 1) {
	    Bit32u mem = EXTMEM_SIZE >> 10;
	    if (mem < 0x3c00) {
		LWORD(eax) = mem;
		LWORD(ebx) = 0;
	    } else {
		LWORD(eax) = 0x3c00;
		LWORD(ebx) = ((mem - 0x3c00) >> 6);
	    }
	    LWORD(ecx) = LWORD(eax);
	    LWORD(edx) = LWORD(ebx);
	    NOCARRY;
	    break;
	} else if (REG(eax) == 0xe820 && REG(edx) == 0x534d4150) {
	    REG(eax) = REG(edx);
	    /* a maximum of 20 bytes will be transferred at one time, even if ECX is
	     * higher; some BIOSes (e.g. Award Modular BIOS v4.50PG) ignore the
	     * value of ECX on entry, and always copy 20 bytes (RBIL) */
	    if (REG(ebx) < system_memory_map_size && REG(ecx) >= 20) {
		REG(ecx) = 20;
		MEMCPY_2DOS(SEGOFF2LINEAR(_ES, _DI),
			    (char *) system_memory_map + REG(ebx),
			    REG(ecx));
		REG(ebx) += REG(ecx);
		if (REG(ebx) >= system_memory_map_size)
			REG(ebx) = 0;
		NOCARRY;
	    } else {
		REG(eax) = 0x8600;
		CARRY;
	    }
	    break;
	}
	/* Fall through !! */

    default:
	g_printf("int 15h error: ax=0x%04x\n", LWORD(eax));
	CARRY;
	break;
    }
    return 1;
}

/* Set the DOS ticks value in BIOS area, then clear midnight flag */
static void set_ticks(unsigned long new_ticks)
{
    WRITE_DWORD(BIOS_TICK_ADDR, new_ticks);
    /* A timer read/write should reset the overflow flag */
    WRITE_BYTE(TICK_OVERFLOW_ADDR, 0);
    h_printf("TICKS: update ticks to %ld\n", new_ticks);
}

/*
 * DANG_BEGIN_FUNCTION int1a
 *
 * int 0x1A call
 *
 * This has (among other things) the calls that DOS makes to get/set its sense
 * of time. On booting, DOS gets the RTC time and date with AH=2 and AH=4,
 * after that it should use AH=0 calls to read the 'tick' counter from BIOS
 * memory. Each time this crosses midnight, a flag is set that DOS uses to
 * increment its date.
 *
 * Here we can now change the 'view' of time so the calls either return BIOS
 * tick (most DOS like), read the PIT counter (avoids INT-8 changes) or gets
 * LINUX time (most accurate for long term NTP-adjusted time keeping).
 *
 * DANG_END_FUNCTION
 */

static int int1a(void)
{
    int_yield();

    switch (HI(ax)) {

/*
--------B-1A00-------------------------------
INT 1A - TIME - GET SYSTEM TIME
	AH = 00h
Return: CX:DX = number of clock ticks since midnight
	AL = midnight flag, nonzero if midnight passed since time last read
Notes:	there are approximately 18.2 clock ticks per second, 1800B0h per 24 hrs
	  (except on Tandy 2000, where the clock runs at 20 ticks per second)
	IBM and many clone BIOSes set the flag for AL rather than incrementing
	  it, leading to loss of a day if two consecutive midnights pass
	  without a request for the time (e.g. if the system is on but idle)
->	since the midnight flag is cleared, if an application calls this
->	  function after midnight before DOS does, DOS will not receive the
->	  midnight flag and will fail to advance the date

Note that DOSEMU's int8 (just like Bochs and some others, see bios.S)
increments AL so we *don't* lose a day if two consecutive midnights pass.
*/
    case 0:			/* read time counter */
	{
	    int day_rollover;
	    idle_enable2(50, 50, 0, "int1a:0");
	    if (config.timemode == TM_LINUX) {
		/* Set BIOS area flags to LINUX time computed values always */
		last_ticks = get_linux_ticks(0, &day_rollover);
	    } else if (config.timemode == TM_BIOS) {
		/* BIOSTIMER_ONLY_VIEW
		 *
		 * We rely on the INT8 routine doing the right thing,
		 * DOS apps too rely on the relationship between INT1A and 0x46c timer.
		 * We already do all appropriate things to trigger the simulated INT8
		 * correctly (well, sometimes faking it), so the 0x46c timer incremented
		 * by the (realmode) INT8 handler should be always in sync.
		 * Therefore, we keep INT1A,AH0 simple instead of trying to be too clever;-)
		 */
		static int first = 1;
		if (first) {
		    /* take over the correct value _once_ only */
		    last_ticks = (unsigned long) (pic_sys_time >> 16)
			+ (sys_base_ticks + usr_delta_ticks);
		    set_ticks(last_ticks);
		    first = 0;
		}

		last_ticks = READ_DWORD(BIOS_TICK_ADDR);
		day_rollover = READ_BYTE(TICK_OVERFLOW_ADDR);
	    } else {		/* (config.timemode == TM_PIT) assumed */

		/* not BIOSTIMER_ONLY_VIEW
		 * pic_sys_time is a zero-based tick (1.19MHz) counter. As such, if we
		 * shift it right by 16 we get the number of PIT0 overflows, that is,
		 * the number of 18.2ms timer ticks elapsed since starting dosemu. This
		 * is independent of any int8 speedup a program can set, since the PIT0
		 * counting frequency is fixed. The count overflows after 7 1/2 years.
		 * usr_delta_ticks is 0 as long as nobody sets a new time (B-1A01)
		 */
#ifdef DEBUG_INT1A
		g_printf
		    ("TIMER: sys_base_ticks=%ld usr_delta_ticks=%ld pic_sys_time=%#Lx\n",
		     sys_base_ticks, usr_delta_ticks, pic_sys_time);
#endif
		last_ticks = (unsigned long) (pic_sys_time >> 16);
		last_ticks += (sys_base_ticks + usr_delta_ticks);

		/* has the midnight passed? */
		day_rollover = last_ticks / TICKS_IN_A_DAY;
		last_ticks %= TICKS_IN_A_DAY;
		/* since pic_sys_time continues to increase, avoid further midnight overflows */
		sys_base_ticks -= day_rollover * TICKS_IN_A_DAY;
	    }

	    LWORD(eax) = day_rollover;
	    LWORD(ecx) = (last_ticks >> 16) & 0xffff;
	    LWORD(edx) = last_ticks & 0xffff;

#ifdef DEBUG_INT1A
	    if (debug_level('g')) {
		time_t time_val;
		long k = last_ticks / 18.2065;	/* sorry */
		time(&time_val);
		g_printf("INT1A: read timer = %ld (%ld:%ld:%ld) %s\n",
			 last_ticks, k / 3600, (k % 3600) / 60, (k % 60),
			 ctime(&time_val));
	    }
#else
	    g_printf("INT1A: read timer=%ld, midnight=%d\n", last_ticks,
		     LO(ax));
#endif
	    set_ticks(last_ticks);	/* Write to BIOS_TICK_ADDR & clear TICK_OVERFLOW_ADDR */
	}
	break;

/*
--------B-1A01-------------------------------
INT 1A - TIME - SET SYSTEM TIME
	AH = 01h
	CX:DX = number of clock ticks since midnight
Return: nothing
Notes:	there are approximately 18.2 clock ticks per second, 1800B0h per 24 hrs
	  (except on Tandy 2000, where the clock runs at 20 ticks per second)
	this call resets the midnight-passed flag
SeeAlso: AH=00h,AH=03h,INT 21/AH=2Dh
*/
    case 1:			/* write time counter */
	if (config.timemode == TM_LINUX) {
	    g_printf("INT1A: can't set DOS timer\n");	/* Allow time set except in 'LINUX view' case. */
	} else {
	    /* get current system time and check it (previous usr_delta could be != 0) */
	    long t;
	    do {
		t = (pic_sys_time >> 16) + sys_base_ticks;
		if (t < 0)
		    sys_base_ticks += TICKS_IN_A_DAY;
	    } while (t < 0);

	    /* get user-requested time */
	    last_ticks = ((uint32_t)LWORD(ecx) << 16) | (LWORD(edx) & 0xffff);

	    usr_delta_ticks = last_ticks - t;

#ifdef DEBUG_INT1A
	    g_printf
		("TIMER: sys_base_ticks=%ld usr_delta_ticks=%ld pic_sys_time=%#Lx\n",
		 sys_base_ticks, usr_delta_ticks, pic_sys_time);
#endif
	    g_printf("INT1A: set timer to %ld\n", last_ticks);

	    set_ticks(last_ticks);	/* Write to BIOS_TICK_ADDR & clear TICK_OVERFLOW_ADDR */
	}
	break;

/*
--------B-1A02-------------------------------
INT 1A - TIME - GET REAL-TIME CLOCK TIME (AT,XT286,PS)
	AH = 02h
Return: CF clear if successful
	    CH = hour (BCD)
	    CL = minutes (BCD)
	    DH = seconds (BCD)
	    DL = daylight savings flag (00h standard time, 01h daylight time)
	CF set on error (i.e. clock not running or in middle of update)
Note:	this function is also supported by the Sperry PC, which predates the
	  IBM AT; the data is returned in binary rather than BCD on the Sperry,
	  and DL is always 00h
SeeAlso: AH=00h,AH=03h,AH=04h,INT 21/AH=2Ch
*/
    case 2:			/* get time */
	idle(50, 50, 0, "int1a:2");
	if (config.timemode != TM_BIOS) {
	    get_linux_ticks(1, NULL);	/* Except BIOS view time, force RTC to LINUX time. */
	}
	HI(cx) = rtc_read(CMOS_HOUR);
	LO(cx) = rtc_read(CMOS_MIN);
	HI(dx) = rtc_read(CMOS_SEC);
	LO(dx) = 0;		/* No daylight saving - yuch */
	g_printf("INT1A: RTC time %02x:%02x:%02x\n", HI(cx), LO(cx),
		 HI(dx));
	NOCARRY;
	break;

/*
--------B-1A03-------------------------------
INT 1A - TIME - SET REAL-TIME CLOCK TIME (AT,XT286,PS)
	AH = 03h
	CH = hour (BCD)
	CL = minutes (BCD)
	DH = seconds (BCD)
	DL = daylight savings flag (00h standard time, 01h daylight time)
Return: nothing
Note:	this function is also supported by the Sperry PC, which predates the
	  IBM AT; the data is specified in binary rather than BCD on the
	  Sperry, and the value of DL is ignored
*/
    case 3:			/* set time */
	if (config.timemode != TM_BIOS) {
	    g_printf("INT1A: RTC: can't set time\n");
	} else {
	    rtc_write(CMOS_HOUR, HI(cx));
	    rtc_write(CMOS_MIN, LO(cx));
	    rtc_write(CMOS_SEC, HI(dx));
	    g_printf("INT1A: RTC set time %02x:%02x:%02x\n", HI(cx),
		     LO(cx), HI(dx));
	}
	NOCARRY;
	break;

/*
--------B-1A04-------------------------------
INT 1A - TIME - GET REAL-TIME CLOCK DATE (AT,XT286,PS)
	AH = 04h
Return: CF clear if successful
	    CH = century (BCD)
	    CL = year (BCD)
	    DH = month (BCD)
	    DL = day (BCD)
	CF set on error
SeeAlso: AH=02h,AH=04h"Sperry",AH=05h,INT 21/AH=2Ah,INT 4B/AH=02h"TI"
*/
    case 4:			/* get date */
	idle(50, 50, 0, "int1a:4");
	if (config.timemode != TM_BIOS) {
	    get_linux_ticks(1, NULL);
	}
	HI(cx) = rtc_read(CMOS_CENTURY);
	LO(cx) = rtc_read(CMOS_YEAR);
	HI(dx) = rtc_read(CMOS_MONTH);
	LO(dx) = rtc_read(CMOS_DOM);
	/* REG(eflags) &= ~CF; */
	g_printf("INT1A: RTC date %04x%02x%02x (DOS format)\n", LWORD(ecx),
		 HI(dx), LO(dx));
	NOCARRY;
	break;

/*
--------B-1A05-------------------------------
INT 1A - TIME - SET REAL-TIME CLOCK DATE (AT,XT286,PS)
	AH = 05h
	CH = century (BCD)
	CL = year (BCD)
	DH = month (BCD)
	DL = day (BCD)
Return: nothing
*/
    case 5:			/* set date */
	if (config.timemode != TM_BIOS) {
	    g_printf("INT1A: RTC: can't set date\n");
	} else {
	    rtc_write(CMOS_CENTURY, HI(cx));
	    rtc_write(CMOS_YEAR, LO(cx));
	    rtc_write(CMOS_MONTH, HI(dx));
	    rtc_write(CMOS_DOM, LO(dx));
	    g_printf("INT1A: RTC set date %04x/%02x/%02x\n", LWORD(ecx),
		     HI(dx), LO(dx));
	}
	NOCARRY;
	break;

	/* Notes: the alarm occurs every 24 hours until turned off, invoking INT 4A
	   each time the BIOS does not check for invalid values for the time, so
	   the CMOS clock chip's "don't care" setting (any values between C0h
	   and FFh) may be used for any or all three parts.  For example, to
	   create an alarm once a minute, every minute, call with CH=FFh, CL=FFh,
	   and DH=00h. (R.Brown)
	 */
    case 6:			/* set alarm */
	{
	    unsigned char h, m, s;

	    if (rtc_read(CMOS_STATUSB) & 0x20) {
		CARRY;
	    } else {
		rtc_write(CMOS_HOURALRM, (h = _CH));
		rtc_write(CMOS_MINALRM, (m = _CL));
		rtc_write(CMOS_SECALRM, (s = _DH));
		r_printf("RTC: set alarm to %02d:%02d:%02d\n", h, m, s);	/* BIN! */
		/* This has been VERIFIED on an AMI BIOS -- AV */
		rtc_write(CMOS_STATUSB, rtc_read(CMOS_STATUSB) | 0x20);
		NOCARRY;
	    }
	    break;
	}

    case 7:			/* clear alarm but NOT PIC mask */
	/* This has been VERIFIED on an AMI BIOS -- AV */
	rtc_write(CMOS_STATUSB, rtc_read(CMOS_STATUSB) & ~0x20);
	break;

    case 0xb1:			/* Intel PCI BIOS v 2.0c */
	pci_bios();
	break;

    default:
	g_printf("WARNING: unsupported INT0x1a call 0x%02x\n", HI(ax));
	CARRY;
    }				/* End switch(HI(ax)) */

    return 1;
}

/* ========================================================================= */
/*
 * DANG_BEGIN_FUNCTION ms_dos
 *
 * int0x21 call
 *
 * we trap this for two functions: simulating the EMMXXXX0 device and
 * fudging the CONFIG.XXX and AUTOEXEC.XXX bootup files.
 *
 * note that the emulation herein may cause problems with programs
 * that like to take control of certain int 21h functions, or that
 * change functions that the true int 21h functions use.  An example
 * of the latter is ANSI.SYS, which changes int 10h, and int 21h
 * uses int 10h.  for the moment, ANSI.SYS won't work anyway, so it's
 * no problem.
 *
 * DANG_END_FUNCTION
 */
static int msdos(void)
{
    ds_printf
	("INT21 at %04x:%04x: AX=%04x, BX=%04x, CX=%04x, DX=%04x, DS=%04x, ES=%04x\n",
	SREG(cs), LWORD(eip), LWORD(eax), LWORD(ebx),
	 LWORD(ecx), LWORD(edx), SREG(ds), SREG(es));

#if 1
    if (HI(ax) == 0x3d) {
	char *p = MK_FP32(SREG(ds), LWORD(edx));
	int i;

	ds_printf("INT21: open file \"");
	for (i = 0; i < 64 && p[i]; i++)
	    ds_printf("%c", p[i]);
	ds_printf("\"\n");
    }
#endif

    switch (HI(ax)) {
/* the below idle handling moved to int10 */
#if 0
    case 0x06:
	if (LO(dx) == 0xff)
	    return 0;
	/* fallthrough */
    case 0x02:
    case 0x04:
    case 0x05:
    case 0x09:
    case 0x40:			/* output functions: reset idle */
	reset_idle(0);
	return 0;
#endif

    case 0x2C:{		/* get time & date */
	    idle_enable(100, 0, "dos_time");
	    return 0;
	}

    case 0x4B:{		/* program load */
	    char *ptr;
	    const char *tmp_ptr;
	    char cmdname[256];
	    char *cmd = SEG_ADR((char *), ds, dx);
	    char *str = cmd;
	    struct param4a *pa4 = SEG_ADR((struct param4a *), es, bx);
	    struct lowstring *args = FARt_PTR(pa4->cmdline);

	    switch (LO(ax)) {
	    case 0x00:
	    case 0x01:
		snprintf(cmdname, _min(sizeof cmdname, args->len + 1), "%s", args->s);
		ds_printf
		    ("INT21 4B: load/execute program=\"%s\", L(cmdline=\"%s\")=%i\n",
		     str, cmdname, args->len);
		break;

	    case 0x03:		/* AL=03h:load overlay have no cmdline in EPB */
		snprintf(cmdname, sizeof cmdname, "%s", cmd);
		ds_printf("INT21 4B: load overlay=\"%s\"\n", str);
		break;

	    case 0x80:		/* DR-DOS run already loaded kernel file, no cmdline */
		snprintf(cmdname, sizeof cmdname, "%s", cmd);
		ds_printf("INT21 4B80: run already loaded file=\"%s\"\n", str);
		break;

	    default:		/* Assume no cmdline, log AL */
		snprintf(cmdname, sizeof cmdname, "%s", cmd);
		ds_printf("INT21 4B: AL=%02x, cmdname=\"%s\"\n", LO(ax), str);
		break;
	    }

	    /* for old DOSes without INSTALL= support, we need this */
	    if (config.force_redir && !redir_state &&
		    strcasestr(cmd, "\\command.com")) {
		ds_printf("INT21: open of command processor triggering post_boot\n");
		if (do_redirect(1))
		    redir_state++;
	    }

#if WINDOWS_HACKS
	    if (strstrDOS(cmd, "\\SYSTEM\\KRNL386.EXE"))
		win3x_mode = ENHANCED;
	    if (strstrDOS(cmd, "\\SYSTEM\\KRNL286.EXE"))
		win3x_mode = STANDARD;
	    if (strstrDOS(cmd, "\\SYSTEM\\KERNEL.EXE"))
		win3x_mode = RM;
	    if ((ptr = strstrDOS(cmd, "\\SYSTEM\\DOSX.EXE")) ||
		(ptr = strstrDOS(cmd, "\\SYSTEM\\WIN386.EXE"))) {
		int have_args = 0;
		tmp_ptr = strstr(cmdname, "krnl386");
		if (!tmp_ptr)
		    tmp_ptr = strstr(cmdname, "krnl286");
		if (!tmp_ptr)
		    tmp_ptr = (ptr[8] == 'd' ? "krnl286" : "krnl386");
		else
		    have_args = 1;
#if 1
		/* ignore everything and use krnl386.exe */
		memcpy(ptr + 8, "krnl386", 7);
#else
		memcpy(ptr + 8, tmp_ptr, 7);
#endif
		strcpy(ptr + 8 + 7, ".exe");
		win3x_mode = tmp_ptr[4] - '0';
		if (have_args) {
		    tmp_ptr = strchr(tmp_ptr, ' ');
		    if (tmp_ptr) {
			strcpy(args->s, tmp_ptr);
			args->len -= tmp_ptr - cmdname;
		    }
		}

		/* the below is the winos2 mouse driver hook */
		SETIVEC(0x66, BIOSSEG, INT_OFF(0x66));
		int_handlers[0x66].interrupt_function[NO_REVECT] = _int66_;
	    }

	    if (win3x_mode != INACTIVE) {
		if ((ptr = strstrDOS(cmd, "\\SYSTEM\\DS")) &&
		    !strstrDOS(cmd, ".EXE")) {
		    error
			("Windows-3.1 stack corruption detected, fixing dswap.exe\n");
		    strcpy(ptr, "\\system\\dswap.exe");
		}
		if ((ptr = strstrDOS(cmd, "\\SYSTEM\\WS")) &&
		    !strstrDOS(cmd, ".EXE")) {
		    error
			("Windows-3.1 stack corruption detected, fixing wswap.exe\n");
		    strcpy(ptr, "\\system\\wswap.exe");
		}

		sprintf(win3x_title, "Windows 3.x in %i86 mode",
			win3x_mode);
		str = win3x_title;
	    }
#endif

	    if (!Video->change_config)
		return 0;
	    if ((!title_hint[0] || strcmp(title_current, title_hint) != 0)
		&& str != win3x_title)
		return 0;

	    ptr = strrchr(str, '\\');
	    if (!ptr)
		ptr = str;
	    else
		ptr++;
	    ptr += strspn(ptr, " \t");
	    if (!ptr[0])
		return 0;
	    tmp_ptr = ptr;
	    while (*tmp_ptr) {	/* Check whether the name is valid */
		if (iscntrlDOS(*tmp_ptr++))
		    return 0;
	    }
	    strncpy(cmdname, ptr, TITLE_APPNAME_MAXLEN - 1);
	    cmdname[TITLE_APPNAME_MAXLEN - 1] = 0;
	    ptr = strchr(cmdname, '.');
	    if (ptr && str != win3x_title)
		*ptr = 0;
	    /* change the title */
	    strlcpy(title_current, cmdname, sizeof(title_current));
	    change_window_title(title_current);
	    can_change_title = 0;
	    return 0;
	}
    }
    return 0;
}

static void do_ret_from_int(int inum, const char *pfx)
{
    unsigned int ssp, sp;
    u_short flgs;

    ssp = SEGOFF2LINEAR(SREG(ss), 0);
    sp = LWORD(esp);

    _IP = popw(ssp, sp);
    _CS = popw(ssp, sp);
    flgs = popw(ssp, sp);
    _SP += 6;
    if (flgs & IF)
	set_IF();
    else
	clear_IF();
    REG(eflags) |= (flgs & (TF_MASK | NT_MASK));
    debug_int(pfx, inum);
}

static void ret_from_int(int tid, void *arg, void *arg2)
{
    do_ret_from_int(tid - int_tid, "RET");
}

static void do_int_iret(Bit16u i, HLT_ARG(arg))
{
    do_ret_from_int((uintptr_t)arg, "iret");
}

static void do_int_disp(Bit16u i, HLT_ARG(arg))
{
    int inum = (uintptr_t)arg;
    uint16_t seg, off;

    switch (inum) {
#define SW_I(n) \
    case 0x##n: \
        seg = READ_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_cs_##n)); \
        off = READ_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_ip_##n)); \
        break
    SW_I(21);
    SW_I(28);
    SW_I(2f);
    SW_I(33);
    default:
      return;
    }
    /* decide if to trace iret or not.
     * We can't trace int2f as it uses stack for data exchange,
     * and we can't trace int21h/26h as it uses CS as input.
     * We are not interested in tracing int28h and int33h. */
    if (!config.trace_irets || inum != 0x21 || _AH == 0x26 ||
            _AH == 0x31 || _AH == 0x4c || _AH == 0 || _AH == 0x4b)
        fake_iret();
    jmp_to(seg, off);
}

#define RVC_SETUP(x) \
static int int##x##_hooked; \
static void _int##x##_rvc_setup(uint16_t seg, uint16_t offs) \
{ \
    WRITE_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_cs_##x), seg); \
    WRITE_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_ip_##x), offs); \
} \
static void int##x##_rvc_setup(void) \
{ \
    _int##x##_rvc_setup(ISEG(0x##x), IOFF(0x##x)); \
} \
static void int##x##_revect(void) \
{ \
    assert(!int##x##_hooked); \
    int##x##_rvc_setup(); \
    fake_int_to(INT_RVC_SEG, INT_RVC_##x##_OFF); \
} \
static uint16_t iret_##x##_hlt_off; \
static uint16_t disp_##x##_hlt_off; \
static void int##x##_rvc_init(void) \
{ \
    emu_hlt_t hlt_hdlr = HLT_INITIALIZER; \
    emu_hlt_t hlt_hdlr2 = HLT_INITIALIZER; \
    hlt_hdlr.name = "int" #x " iret"; \
    hlt_hdlr.func = do_int_iret; \
    hlt_hdlr.arg = (void *)0x##x; \
    iret_##x##_hlt_off = hlt_register_handler_vm86(hlt_hdlr); \
    hlt_hdlr2.name = "int" #x " disp"; \
    hlt_hdlr2.func = do_int_disp; \
    hlt_hdlr2.arg = (void *)0x##x; \
    disp_##x##_hlt_off = hlt_register_handler_vm86(hlt_hdlr2); \
} \
static void int##x##_rvc_post_init(void) \
{ \
    WRITE_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_ret_cs_##x), \
            BIOS_HLT_BLK_SEG); \
    WRITE_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_ret_ip_##x), \
            iret_##x##_hlt_off); \
    WRITE_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_disp_cs_##x), \
            BIOS_HLT_BLK_SEG); \
    WRITE_WORD(SEGOFF2LINEAR(INT_RVC_SEG, int_rvc_disp_ip_##x), \
            disp_##x##_hlt_off); \
}

/*
 * We support the following cases:
 * 1. The ints were already unrevectored by post_boot(), then return error.
 * 2. The ints were initially not revectored by vm86.int_revectored
 *    ($_force_int_revect = (off)). Then we allow setting them up. The
 *    care must be taken in mfs/lfn to not crash if this happens before
 *    the init of these subsystems. At the time of writing this, such
 *    care is taken. Make sure it stays so in the future. :)
 * 3. The ints were initially revectored and still are.
 *    Disable revectoring but set them to our handlers, effectively
 *    not changing anything.
 */
#define UNREV(x) \
RVC_SETUP(x) \
static far_t int##x##_unrevect(uint16_t seg, uint16_t offs) \
{ \
  far_t ret = {}; \
  if (int##x##_hooked) \
    return ret; \
  int##x##_hooked = 1; \
  di_printf("int_rvc: unrevect 0x%s\n", #x); \
  if (test_bit(0x##x, &vm86s.int_revectored)) { \
    if (!mhp_revectored(0x##x)) \
      clear_bit(0x##x, &vm86s.int_revectored); \
    else \
        mhp_adjust_revectored(0x##x); \
  } else { \
    di_printf("int_rvc: revectoring of 0x%s was not enabled\n", #x); \
  } \
  _int##x##_rvc_setup(seg, offs); \
  ret.segment = INT_RVC_SEG; \
  ret.offset = INT_RVC_##x##_OFF; \
  return ret; \
} \
static int int##x##_unrevect_simple(void) \
{ \
  if (int##x##_hooked || !int_handlers[0x##x].interrupt_function[REVECT]) \
    return 0; \
  int##x##_hooked = 1; \
  di_printf("int_rvc: unrevect 0x%s\n", #x); \
  clear_bit(0x##x, &vm86s.int_revectored); \
  int##x##_rvc_setup(); \
  SETIVEC(0x##x, INT_RVC_SEG, INT_RVC_##x##_OFF); \
  return 1; \
}

UNREV(21)
UNREV(28)
UNREV(2f)
UNREV(33)
UNREV(e6)

static void int_revect_init(void)
{
    int21_rvc_init();
    int28_rvc_init();
    int2f_rvc_init();
    int33_rvc_init();
    inte6_rvc_init();
}

static void int_revect_post_init(void)
{
    int21_rvc_post_init();
    int28_rvc_post_init();
    int2f_rvc_post_init();
    int33_rvc_post_init();
    inte6_rvc_post_init();
}

static void post_boot_unrevect(void)
{
    int21_unrevect_simple();
    int28_unrevect_simple();
    int2f_unrevect_simple();
    if (int33_unrevect_simple()) {
      /* This is needed here to revectoring the interrupt, after dos
       * has revectored it. --EB 1 Nov 1997 */
      SETIVEC(0x33, Mouse_SEG, Mouse_INT_OFF);
    }
    inte6_unrevect(BIOSSEG, INT_OFF(0xe6));
}

static far_t int33_unrevect_fixup(uint16_t seg, uint16_t offs)
{
  far_t ret = int33_unrevect(seg, offs);
  if (ret.offset != INT_RVC_33_OFF)
    return ret;
  ret.segment = Mouse_SEG;
  ret.offset = Mouse_INT_OFF;
  return ret;
}

static void inte6_revect_fixup(void)
{
    assert(!inte6_hooked);
    _inte6_rvc_setup(BIOSSEG, INT_OFF(0xe6));
    fake_int_to(INT_RVC_SEG, INT_RVC_e6_OFF);
}

static far_t inte6_unrevect_fixup(uint16_t seg, uint16_t offs)
{
    return inte6_unrevect(BIOSSEG, INT_OFF(0xe6));
}

static int inte6_rvc_dummy(int stk_offs, int revect)
{
    error("inte6: should not be here\n");
    return 0;
}

static int msdos_chainrevect(int stk_offs, int revect)
{
    switch (HI(ax)) {
    case 0x71:
	if (config.lfn)
	    return I_SECOND_REVECT;
	break;
    case 0x73:			/* fat32 API */
    case 0x6c:			/* extended open, needs mostly for LFNs */
    case 0x60:
	return I_SECOND_REVECT;
    }
    return msdos();
}

static void msdos_xtra(uint16_t old_ax, uint16_t old_flags)
{
    di_printf("int_rvc 0x21 call for ax=0x%04x %x\n", LWORD(eax), old_ax);

    CARRY;
    switch (HI_BYTE(old_ax)) {
    case 0x71:
	if (LWORD(eax) != 0x7100)
	    break;
	if (config.lfn) {
	    int ret;
	    LWORD(eax) = old_ax;
	    if (!(old_flags & CF))
		NOCARRY;
	    /* mfs_lfn() clears CF on success, sets on failure, preserves
	     * on unsupported */
	    ret = mfs_lfn();
	    if (!ret)
		LWORD(eax) = 0x7100;
	}
	break;
    case 0x73:
	if (LWORD(eax) != 0x7300)
	    break;
	LWORD(eax) = old_ax;
	if (!(old_flags & CF))
	    NOCARRY;
	mfs_fat32();
	break;
    case 0x6c:
	if (LWORD(eax) != 0x6c00)
	    break;
	LWORD(eax) = old_ax;
	if (!(old_flags & CF))
	    NOCARRY;
	msdos_remap_extended_open();
	break;
    case 0x60:
	if (isset_CF()) {
	    int cf;
	    /* OpenDOS 7.01 doesn't call local truename if QUALIFY_FILENAME
	     * failed. See https://github.com/dosemu2/dosemu2/issues/2211
	     */
	    pre_msdos();
	    _AX = 0x7160;
	    _CX = 0;
	    call_msdos();
	    cf = isset_CF();
	    post_msdos();
	    if (!cf)
	        NOCARRY;
	}
	break;
    }
}

static int msdos_xtra_norev(int stk_off, int revect)
{
    di_printf("int_norvc 0x21 call for ax=0x%04x\n", LWORD(eax));
    switch (HI(ax)) {
    case 0x71:
	if (config.lfn)
	    return mfs_lfn();
	else
	    CARRY;
	break;
    case 0x73:
	mfs_fat32();
	break;
    case 0x6c:
	msdos_remap_extended_open();
	break;
    }
    return 0;
}

void int42_hook(void)
{
    /* original int10 vector should point here until vbios swaps it with 0x42.
     * But our int10 never points here, so I doubt this is of any use. --stsp */
    fake_iret();
    int10();
}

/* ========================================================================= */

void real_run_int(int i)
{
    unsigned int ssp, sp;

    ssp = SEGOFF2LINEAR(_SS, 0);
    sp = _SP;

    pushw(ssp, sp, read_FLAGS());
    pushw(ssp, sp, _CS);
    pushw(ssp, sp, _IP);
    _SP -= 6;
    _CS = ISEG(i);
    _IP = IOFF(i);

    /* clear TF (trap flag, singlestep), VIF/IF (interrupt flag), and
     * NT (nested task) bits of EFLAGS
     * NOTE: IF-flag only, because we are not sure that we will test it in
     *       some of our own software (...we all are human beings)
     *       For vm86() 'VIF' is the candidate to reset in order to do CLI !
     */
    clear_TF();
    clear_NT();
    if (IS_CR0_AM_SET())
	clear_AC();
    clear_IF();
}

static void do_print_screen(void)
{
    int x_pos, y_pos;
    int li = READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1) + 1;
    int co = READ_WORD(BIOS_SCREEN_COLUMNS);
    unsigned base = screen_adr(READ_BYTE(BIOS_CURRENT_SCREEN_PAGE));

    g_printf("PrintScreen: base=%x, lines=%i columns=%i\n", base, li, co);
    if (printer_open(0) == -1)
	return;
    for (y_pos = 0; y_pos < li; y_pos++) {
	for (x_pos = 0; x_pos < co; x_pos++) {
	    uint8_t val = vga_read(base + 2 * (y_pos * co + x_pos));
	    if (val == 0)
		val = ' ';
	    printer_write(0, val);
	}
	printer_write(0, 0x0d);
	printer_write(0, 0x0a);
    }
    printer_close(0);
}

static int int05(void)
{
    /* FIXME does this test actually catch an unhandled bound exception */
    if (*SEG_ADR((Bit8u *), cs, ip) == 0x62) {	/* is this BOUND ? */
	/* avoid deadlock: eip is not advanced! */
	error("Unhandled BOUND exception!\n");
	leavedos(54);
    }
    g_printf("INT 5: PrintScreen\n");
    do_print_screen();
    return 1;
}

/* CONFIGURATION */
static int int11(void)
{
    LWORD(eax) = READ_WORD(BIOS_CONFIGURATION);
#if WINDOWS_HACKS
    if (win3x_mode != INACTIVE)
	LWORD(eax) &= ~CONF_MATHCO;  // work around win87em.dll's problem
#endif
    return 1;
}

/* MEMORY */
static int int12(void)
{
    LWORD(eax) = READ_WORD(BIOS_MEMORY_SIZE);
    return 1;
}

/* BASIC */
static int int18(void)
{
    k_printf("BASIC interrupt being attempted.\n");
    return 1;
}

/* LOAD SYSTEM */
static int int19(void)
{
    int stal;
    coopth_leave();
    dpmi_done0();
    if (clnup_handler)
	clnup_handler();
    clnup_handler = NULL;
    stal = coopth_flush_vm86();
    if (stal) {
        error("stalled %i threads on reboot\n", stal);
        coopth_unsafe_shutdown();
    }
    map_custom_bios();
    cpu_reset();
    jmp_to(0xffff, 0);
    return 1;
}

static uint16_t DoRedirectDevice(char *dStr, char *sStr,
                        uint8_t deviceType, uint16_t deviceOptions,
                        uint16_t userValue)
{
  uint16_t ret;

  pre_msdos();

  /* should verify strings before sending them down ??? */
  SREG(ds) = DOSEMU_LMHEAP_SEG;
  LWORD(esi) = DOSEMU_LMHEAP_OFFS_OF(dStr);
  SREG(es) = DOSEMU_LMHEAP_SEG;
  LWORD(edi) = DOSEMU_LMHEAP_OFFS_OF(sStr);
  LWORD(edx) = deviceOptions | REDIR_CLIENT_SIGNATURE;
  LWORD(ecx) = userValue;
  LWORD(ebx) = deviceType;
  LWORD(eax) = DOS_REDIRECT_DEVICE;

  call_msdos();

  ret = (LWORD(eflags) & CF) ? LWORD(eax) : CC_SUCCESS;

  post_msdos();
  return ret;
}

uint16_t RedirectDevice(char *dStr, char *sStr,
                        uint8_t deviceType, uint16_t deviceOptions)
{
  return DoRedirectDevice(dStr, sStr, deviceType, deviceOptions, 0);
}

static int DoRedirectDisk(int dsk, const char *resourceName, int flags,
    int uval)
{
  char *dStr = lowmem_alloc(16);
  char *rStr = lowmem_alloc(256);
  int ret;

  dStr[0] = dsk + 'A';
  dStr[1] = ':';
  dStr[2] = '\0';
  snprintf(rStr, 256, LINUX_RESOURCE "%s", resourceName);

  ret = DoRedirectDevice(dStr, rStr, REDIR_DISK_TYPE, flags, uval);

  lowmem_free(rStr);
  lowmem_free(dStr);
  return ret;
}

static int RedirectDisk(int dsk, const char *resourceName, int flags)
{
  return DoRedirectDisk(dsk, resourceName, flags, 0);
}

static int RedirectPrinter(int lptn)
{
  char *dStr = lowmem_alloc(16);
  char *rStr = lowmem_alloc(128);
  int ret;

  snprintf(dStr, 16, "LPT%i", lptn);
  snprintf(rStr, 128, LINUX_PRN_RESOURCE "\\%i", lptn);

  ret = RedirectDevice(dStr, rStr, REDIR_PRINTER_TYPE, 0);

  lowmem_free(rStr);
  lowmem_free(dStr);
  return ret;
}

static int redir_printers(void)
{
    int i;
    int max = lpt_get_max();

    for (i = NUM_LPTS; i < max; i++) {
	if (!lpt_is_configured(i))
	    continue;
	c_printf("redirecting LPT%i\n", i + 1);
	if (RedirectPrinter(i + 1) != CC_SUCCESS) {
	    printf("failure redirecting LPT%i\n", i + 1);
	    return 1;
	}
    }
    return 0;
}

/* drive for -K (aka system.com) */
struct drive_syscom {
    char *path;
    int mfs_idx;
#define SCUSERS 5
    uint8_t *drv_num[SCUSERS];
    int num_scusers;
};
static struct drive_syscom syscomdrv;
/* drive for -d */
struct drive_xtra {
    char *path;
    unsigned ro:1;
    unsigned cdrom:1;
    unsigned grp:1;
    int mfs_idx;
};
#define MAX_EXTRA_DRIVES 50
static struct drive_xtra extra_drives[MAX_EXTRA_DRIVES];
static int num_x_drives;

#define MK_R_FLAGS(ro, cdrom, prm, dsb, mfs_idx) ((ro) | ((cdrom) << 1) | \
      ((prm) ? REDIR_DEVICE_PERMANENT : 0) | \
      ((dsb) ? REDIR_DEVICE_DISABLED : 0) | \
      ((mfs_idx) << REDIR_DEVICE_IDX_SHIFT))

#define REDIR_F_GRP 1

void add_syscom_drive(char *path, uint8_t *user)
{
    assert(syscomdrv.num_scusers == 0);
    syscomdrv.path = expand_path(path);
    syscomdrv.mfs_idx = mfs_define_drive(syscomdrv.path);
    syscomdrv.drv_num[0] = user;
}

void add_syscom_user(uint8_t *user)
{
    assert(1 + syscomdrv.num_scusers < SCUSERS);
    syscomdrv.drv_num[1 + syscomdrv.num_scusers++] = user;
}

int add_extra_drive(char *path, int ro, int cd, int grp)
{
    struct drive_xtra *drv;
    if (num_x_drives >= MAX_EXTRA_DRIVES) {
	error("too many drives\n");
	return -1;
    }
    drv = &extra_drives[num_x_drives++];
    drv->path = expand_path(path);
    if (!drv->path) {
	error("Path %s does not exist\n", path);
	return -1;
    }
    if (!exists_dir(drv->path)) {
	error("Directory %s does not exist\n", drv->path);
	free(drv->path);
	return -1;
    }
    drv->ro = ro;
    drv->cdrom = cd;
    drv->grp = grp;
    drv->mfs_idx = mfs_define_drive(drv->path);
    return 0;
}

static int get_redirection_index(int drive, int *r_mfs_idx, uint16_t *r_udata)
{
    uint16_t redirIndex = 0, ccode;
    char dStr[MAX_DEVICE_STRING_LENGTH];
    char dStrSrc[MAX_DEVICE_STRING_LENGTH];
    char res_backup[128];
    uint16_t opts;

    snprintf(dStrSrc, MAX_DEVICE_STRING_LENGTH, "%c:", drive + 'A');
    while ((ccode = get_redirection(redirIndex, dStr, sizeof dStr,
                                       res_backup, sizeof(res_backup),
                                       r_udata, &opts, NULL)) ==
                                       CC_SUCCESS) {
        if (strcmp(dStrSrc, dStr) == 0) {
            if (r_mfs_idx)
                *r_mfs_idx = REDIR_DEVICE_IDX(opts);
            return redirIndex;
        }
        redirIndex++;
    }

    return -1;
}

static int is_redirection_exist(int drive)
{
    return (get_redirection_index(drive, NULL, NULL) != -1);
}

static int is_occupied_drive_letter(int drv)
{
  char *fname, *fcb;
  int ret;

  pre_msdos();

  /* Parse filename into FCB (physical, formatted or not, and network) */
  fname = lowmem_alloc(16);
  snprintf(fname, 16, "%c:FILENAME.EXT", 'A' + drv);
  fcb = lowmem_alloc(0x25);
  memset(fcb, 0, 0x25);

  HI(ax) = 0x29;	// Parse Filename
  LO(ax) = 0x00;	// Standard parsing
  SREG(ds) = DOSEMU_LMHEAP_SEG;
  LWORD(esi) = DOSEMU_LMHEAP_OFFS_OF(fname);
  SREG(es) = DOSEMU_LMHEAP_SEG;
  LWORD(edi) = DOSEMU_LMHEAP_OFFS_OF(fcb);
  call_msdos();

  lowmem_free(fcb);
  lowmem_free(fname);

  ret = (LO(ax) != 0xff); // 0xff == invalid drive

  post_msdos();

  if (!ret) {
    /* may be disabled redirection */
    ret = is_redirection_exist(drv);
  }
  return ret;
}

int find_free_drive(void)
{
  int drive;

  for (drive = 2; drive < 26; drive++) {
    if (is_occupied_drive_letter(drive))  // 0 = A etc
      continue;
    return drive;
  }

  return -1;
}

/********************************************
 * get_redirection - get next entry from list of redirected devices
 * ON ENTRY:
 *  redirIndex has the index of the next device to return
 *    this should start at 0, and be incremented between calls
 *    to retrieve all elements of the redirection list
 * ON EXIT:
 *  returns CC_SUCCESS if the operation was successful, and
 *  deviceStr has a string with the device name:
 *    either disk or printer (ex. 'D:' or 'LPT1')
 *  resourceStr has a string with the server and name of resource
 *    (ex. 'TIM\TOOLS')
 *  deviceType indicates the type of device which was redirected
 *    3 = printer, 4 = disk
 *  deviceUserData has the magic word passed during creation
 *  deviceOptions has Dosemu specifics (disabled, cdrom unit, read only)
 * NOTES:
 *
 ********************************************/
static uint16_t do_get_redirection(uint16_t redirIndex,
                            char *deviceStr, int deviceSize,
                            char *resourceStr, int resourceSize,
                            uint16_t *deviceUserData,
                            uint16_t *deviceOptions, uint8_t *deviceStatus,
                            uint16_t subfunc)
{
  char *dStr;
  char *rStr;
  uint16_t ret, deviceUserDataTemp, deviceOptionsTemp;
  uint8_t deviceStatusTemp;

  assert(resourceSize <= MAX_RESOURCE_LENGTH_EXT);
  dStr = lowmem_alloc(deviceSize);
  rStr = lowmem_alloc(resourceSize);
  pre_msdos();

  SREG(ds) = DOSEMU_LMHEAP_SEG;
  LWORD(esi) = DOSEMU_LMHEAP_OFFS_OF(dStr);
  SREG(es) = DOSEMU_LMHEAP_SEG;
  LWORD(edi) = DOSEMU_LMHEAP_OFFS_OF(rStr);

  LWORD(edx) = REDIR_CLIENT_SIGNATURE;
  LWORD(ecx) = resourceSize;
  LWORD(ebx) = redirIndex;
  LWORD(eax) = subfunc;

  call_msdos();

  ret = (LWORD(eflags) & CF) ? LWORD(eax) : CC_SUCCESS;

  if (LO(bx) != REDIR_DISK_TYPE)
    ret = 0x12; // NO_MORE_FILES
  deviceStatusTemp = HI(bx);
  deviceUserDataTemp = LWORD(ecx);
  deviceOptionsTemp = LWORD(edx);

  post_msdos();

  if (ret == CC_SUCCESS) {
    strlcpy(resourceStr, rStr, resourceSize);
    strlcpy(deviceStr, dStr, deviceSize);

    if (deviceUserData)
      *deviceUserData = deviceUserDataTemp;
    if (deviceOptions)
      *deviceOptions = deviceOptionsTemp;
    if (deviceStatus)
      *deviceStatus = deviceStatusTemp;
  }

  lowmem_free(rStr);
  lowmem_free(dStr);

  return ret;
}

uint16_t get_redirection(uint16_t redirIndex,
                            char *deviceStr, int deviceSize,
                            char *resourceStr, int resourceSize,
                            uint16_t *deviceUserData,
                            uint16_t *deviceOptions, uint8_t *deviceStatus)
{
  return do_get_redirection(redirIndex, deviceStr, deviceSize,
      resourceStr, resourceSize, deviceUserData,
      deviceOptions, deviceStatus, DOS_GET_REDIRECTION_EXT);
}

uint16_t get_redirection_ux(uint16_t redirIndex,
                            char *deviceStr, int deviceSize,
                            char *resourceStr, int resourceSize,
                            uint16_t *deviceUserData,
                            uint16_t *deviceOptions, uint8_t *deviceStatus)
{
  return do_get_redirection(redirIndex, deviceStr, deviceSize,
      resourceStr, resourceSize, deviceUserData,
      deviceOptions, deviceStatus, DOS_GET_REDIRECTION_EX6);
}

int get_lastdrive(void)
{
  int ld;
  pre_msdos();
  LWORD(eax) = 0x0e00;
  LWORD(edx) = 0xffff;
  call_msdos();
  ld = LO(ax);
  post_msdos();
  return ld;
}

int getCWD_r(int drive, char *rStr, int len)
{
#define DOS_GET_CWD            0x4700
    char *cwd;
    int cf, ax;

    cwd = lowmem_alloc(64);

    pre_msdos();
    LWORD(eax) = DOS_GET_CWD;
    LWORD(edx) = drive + 1;
    SREG(ds) = DOSEMU_LMHEAP_SEG;
    LWORD(esi) = DOSEMU_LMHEAP_OFFS_OF(cwd);
    call_msdos();
    cf = isset_CF();
    ax = LWORD(eax);
    post_msdos();
    if (cf) {
	lowmem_free(cwd);
	return (ax ?: -1);
    }

    if (cwd[0]) {
        snprintf(rStr, len, "%c:\\%s", 'A' + drive, cwd);
    } else {
        snprintf(rStr, len, "%c:", 'A' + drive);
    }
    lowmem_free(cwd);
    return 0;
}

int getCWD_cur(char *rStr, int len)
{
#define DOS_GET_DEFAULT_DRIVE  0x1900
    uint8_t drive;
    pre_msdos();
    LWORD(eax) = DOS_GET_DEFAULT_DRIVE;
    call_msdos();
    drive = LO(ax);
    post_msdos();
    return getCWD_r(drive, rStr, len);
}

char *getCWD(int drive)
{
    static char dcwd[MAX_PATH_LENGTH];
    int err = getCWD_r(drive, dcwd, MAX_PATH_LENGTH);
    if (err)
        return NULL;
    return dcwd;
}

int get_redirection_root(int drive, char *presourceStr, int resourceLength)
{
    uint16_t redirIndex = 0, ccode;
    char dStr[MAX_DEVICE_STRING_LENGTH];
    char dStrSrc[MAX_DEVICE_STRING_LENGTH];
    char res_backup[128];
    char *resStr = resourceLength > 0 ? presourceStr : res_backup;
    int resLen = resourceLength > 0 ? resourceLength : sizeof(res_backup);

    snprintf(dStrSrc, MAX_DEVICE_STRING_LENGTH, "%c:", drive + 'A');
    while ((ccode = do_get_redirection(redirIndex, dStr, sizeof dStr,
                                       resStr, resLen,
                                       NULL, NULL, NULL,
                                       DOS_GET_REDIRECTION_EX6)) ==
                                       CC_SUCCESS) {
      if (strcmp(dStrSrc, dStr) == 0)
        return strlen(resStr);
      redirIndex++;
    }

    return -1;
}

static int get_redirection_drive(char *presourceStr)
{
    uint16_t redirIndex = 0, ccode;
    char dStr[MAX_DEVICE_STRING_LENGTH];
    char resourceStr[MAX_RESOURCE_LENGTH_EXT];

    while ((ccode = do_get_redirection(redirIndex, dStr, sizeof(dStr),
                                       resourceStr, sizeof(resourceStr),
                                       NULL, NULL, NULL,
                                       DOS_GET_REDIRECTION_EX6)) ==
                                       CC_SUCCESS) {
      if (strcmp(resourceStr, presourceStr) == 0)
        return (dStr[0] - 'A');
      redirIndex++;
    }

    return -1;
}

int is_redirection_ro(int drive)
{
    uint16_t redirIndex = 0, ccode;
    char dStr[MAX_DEVICE_STRING_LENGTH];
    char dStrSrc[MAX_DEVICE_STRING_LENGTH];
    char res_backup[128];
    uint16_t opts;

    snprintf(dStrSrc, MAX_DEVICE_STRING_LENGTH, "%c:", drive + 'A');
    while ((ccode = get_redirection(redirIndex, dStr, sizeof dStr,
                                       res_backup, sizeof(res_backup),
                                       NULL, &opts, NULL)) ==
                                       CC_SUCCESS) {
      if (strcmp(dStrSrc, dStr) == 0)
        return !!(opts & REDIR_DEVICE_READ_ONLY);
      redirIndex++;
    }

    return -1;
}

/*
 * Turn all simulated FAT devices into network drives.
 */
static void redirect_drives(void)
{
  int i, ret;

  FOR_EACH_HDISK(i, {
    if (hdisktab[i].type == DIR_TYPE && hdisktab[i].fatfs) {
      ret = RedirectDisk(HDISK_NUM(i) + hdisktab[i].log_offs,
          hdisktab[i].dev_name, hdisktab[i].rdonly +
          (hdisktab[i].mfs_idx << REDIR_DEVICE_IDX_SHIFT));
      if (ret != CC_SUCCESS) {
        error("INT21: redirecting %c: failed (err = %d)\n", i + 'C', ret);
        if (config.boot_dos == FATFS_FD_D && ret == 0xf)
          error("@Unsupported FreeDOS version\n");
        leavedos(8);
      }
      else
        ds_printf("INT21: redirecting %c: ok\n", i + 'C');
    }
  });
}

static int redir_one_drive(const char *path, int ro, int cdrom, int prm,
    int grp, int mfs_idx)
{
  int ret;
  int drv = find_free_drive();
  if (drv < 0) {
    error("no free drives\n");
    if (get_lastdrive() < 7)
      error("@Set LASTDRIVE=Z in your fdconfig.sys\n");
    else if (config.boot_dos == FATFS_FD_D) {
      error("@-d/-K is not supported with this freedos version\n");
      leavedos(26);
    }
    return -1;
  }
  ret = DoRedirectDisk(drv, path, MK_R_FLAGS(ro, cdrom, prm, grp, mfs_idx),
      grp ? REDIR_F_GRP : 0);
  if (ret != CC_SUCCESS) {
    error("INT21: redirecting %s failed (err = %d)\n", path, ret);
    if (get_lastdrive() < 7)
      error("@Set LASTDRIVE=Z in your fdconfig.sys\n");
    else if (config.boot_dos == FATFS_FD_D && (ret == 0x55 /* duplicate redirect */
        || ret == 0xf /* invalid drive */)) {
      error("-d/-K is not supported with this freedos version\n");
      leavedos(26);
    }
    return -1;
  } else {
    ds_printf("INT21: redirecting %s ok\n", path);
  }
  return drv;
}

char *com_strdup(const char *s)
{
	struct lowstring *p;
	int len = strlen(s);
	if (len > 254) {
		error("lowstring too long: %i bytes.\n", len);
		len = 254;
	}

	p = (void *)lowmem_alloc(len + 1 + sizeof(struct lowstring));
	if (!p) return 0;
	p->len = len;
	memcpy(p->s, s, len);
	p->s[len] = 0;
	return p->s;
}

void com_strfree(char *s)
{
	struct lowstring *p = (void *)(s - 1);
	lowmem_free((char *)p);
}

uint16_t cancel_redirection(const char *deviceStr)
{
  char *dStr = com_strdup(deviceStr);
  uint16_t ret;

  pre_msdos();

  SREG(ds) = DOSEMU_LMHEAP_SEG;
  LWORD(esi) = DOSEMU_LMHEAP_OFFS_OF(dStr);
  LWORD(eax) = DOS_CANCEL_REDIRECTION;

  call_msdos();

  ret = (LWORD(eflags) & CF) ? LWORD(eax) : CC_SUCCESS;

  post_msdos();

  com_strfree(dStr);

  return ret;
}

static int add_drive_group(const char *path, int ro, int mfs_idx)
{
#ifdef __linux__
#define IS_RO(e, b) ((e ? 0 : !!(b.f_flags & ST_RDONLY)) || ro)
#define IS_CD(e, b) (e ? 0 : !!(b.f_type == ISOFS_SUPER_MAGIC || \
        b.f_type == UDF_SUPER_MAGIC))
#else
#define IS_RO(e, b) 0
#define IS_CD(e, b) 0
#endif
    char *wild;
    glob_t p;
    int i, err;
    int cnt = 0;

    asprintf(&wild, "%s/*", path);
    glob(wild, 0, NULL, &p);
    free(wild);
    for (i = 0; i < p.gl_pathc; i++) {
#ifdef __linux__
        struct statfs sb;
#endif
        struct stat st;
        err = stat(p.gl_pathv[i], &st);
        if (err) {
            error("error stat %s\n", p.gl_pathv[i]);
            break;
        }
        if (!S_ISDIR(st.st_mode))
            continue;
        /* see if drive is already there */
        err = get_redirection_drive(p.gl_pathv[i]);
        if (err != -1)
            continue;
#ifdef __linux__
        err = statfs(p.gl_pathv[i], &sb);
        if (err == -1) {
            perror("statfs()");
            break;
        }
#endif
        err = redir_one_drive(p.gl_pathv[i], IS_RO(err, sb), IS_CD(err, sb),
                0, 0, mfs_idx);
        if (err < 0)
            break;
        cnt++;
    }
    globfree(&p);

    return cnt;
}

static int add_redir_group(int redirIdx, int mfs_idx)
{
    char dStr[MAX_DEVICE_STRING_LENGTH];
    char resourceStr[MAX_RESOURCE_LENGTH_EXT];
    uint16_t rc, opts;

    rc = get_redirection_ux(redirIdx, dStr, sizeof(dStr),
            resourceStr, sizeof(resourceStr), NULL, &opts, NULL);
    if (rc != CC_SUCCESS)
        return -1;
    return add_drive_group(resourceStr,
            !!(opts & REDIR_DEVICE_READ_ONLY), mfs_idx);
}

static void update_group(uint16_t redirIndex, int mfs_idx)
{
    char dStr[MAX_DEVICE_STRING_LENGTH];
    char rStr[MAX_RESOURCE_LENGTH_EXT];
    uint16_t ccode;
    struct stat st;
    uint16_t opts, udata;
    int err;
    int redirIndex2 = redirIndex + 1;

    while ((ccode = get_redirection_ux(redirIndex2,
                                       dStr, sizeof(dStr),
                                       rStr, sizeof(rStr),
                                       &udata, &opts, NULL)) ==
                                       CC_SUCCESS) {
        if (REDIR_DEVICE_IDX(opts) != mfs_idx) {
            redirIndex2++;
            continue;
        }
        err = stat(rStr, &st);
        if (err || !S_ISDIR(st.st_mode))
            cancel_redirection(dStr);
        else
            redirIndex2++;
    }
    add_redir_group(redirIndex, mfs_idx);
}

int update_redir_group(int drive)
{
    int mfs_idx;
    uint16_t udata;
    int redirIdx = get_redirection_index(drive, &mfs_idx, &udata);
    if (redirIdx == -1 || !(udata & REDIR_F_GRP))
        return -1;
    update_group(redirIdx, mfs_idx);
    return 0;
}

static int rehash_redir_groups(void)
{
    uint16_t redirIndex = 0, ccode;
    char dGrpStr[MAX_DEVICE_STRING_LENGTH];
    char grpRes[128];
    uint16_t opts, udata;
    int cnt = 0;

    while ((ccode = get_redirection(redirIndex, dGrpStr, sizeof(dGrpStr),
                                       grpRes, sizeof(grpRes),
                                       &udata, &opts, NULL)) ==
                                       CC_SUCCESS) {
        if (udata & REDIR_F_GRP) {
            update_group(redirIndex, REDIR_DEVICE_IDX(opts));
            cnt++;
        }
        redirIndex++;
    }

    return cnt;
}

static void redir_extra_drives(void)
{
  int i, ret, drv;

  if (syscomdrv.path) {
    drv = redir_one_drive(syscomdrv.path, 0, 0, 1, 0, syscomdrv.mfs_idx);
    if (drv < 0) {
      leavedos(26);
      return;
    }
    /* notify all users about this drive */
    for (i = 0; i < 1 + syscomdrv.num_scusers; i++)
      *syscomdrv.drv_num[i] = drv;
  }

  for (i = 0; i < num_x_drives; i++) {
    struct drive_xtra *d = &extra_drives[i];
    ret = redir_one_drive(d->path, d->ro, d->cdrom, 1, d->grp, d->mfs_idx);
    if (ret < 0)
      break;
    if (d->grp)
      add_drive_group(d->path, d->ro, d->mfs_idx);
  }
}

/*
 * redirect everything else.
 * must be done after config.sys processing.
 */
static void redirect_devices(void)
{
  if (redir_state2)
    return;
  redir_state2++;
  redir_extra_drives();
  redir_printers();
}

static int do_redirect(int old_only)
{
    uint16_t lol_lo, lol_hi, sda_lo, sda_hi, sda_size, redver, mosver;
    uint8_t major, minor;
    int is_MOS;
    int is_cf;

    /*
     * To start up the redirector we need
     * (1) the list of list,
     * (2) the DOS version and
     * (3) the swappable data area.
     */
    pre_msdos();

    LWORD(eax) = 0x1100;
    do_int_call_back(0x2f);
    if (LO(ax) != 0xff) {
	error("Redirector unavailable\n");
	_post_msdos();
	return 0;
    }

    LWORD(eax) = 0x5200;
    call_msdos();
    ds_printf
	("INT21 +1 at %04x:%04x: AX=%04x, BX=%04x, CX=%04x, DX=%04x, DS=%04x, ES=%04x\n",
	 SREG(cs), LWORD(eip), LWORD(eax), LWORD(ebx),
	 LWORD(ecx), LWORD(edx), SREG(ds), SREG(es));
    lol_lo = LWORD(ebx);
    lol_hi = SREG(es);

    LWORD(eax) = LWORD(ebx) = LWORD(ecx) = LWORD(edx) = 0x3099; // MOS version trigger
    call_msdos();
    ds_printf
	("INT21 +2b at %04x:%04x: AX=%04x, BX=%04x, CX=%04x, DX=%04x, DS=%04x, ES=%04x\n",
	 SREG(cs), LWORD(eip), LWORD(eax), LWORD(ebx),
	 LWORD(ecx), LWORD(edx), SREG(ds), SREG(es));
    mosver = LWORD(eax);
    LWORD(eax) = 0x3000;                                        // DOS version std request
    call_msdos();
    ds_printf
	("INT21 +2 at %04x:%04x: AX=%04x, BX=%04x, CX=%04x, DX=%04x, DS=%04x, ES=%04x\n",
	  SREG(cs), LWORD(eip), LWORD(eax), LWORD(ebx),
	 LWORD(ecx), LWORD(edx), SREG(ds), SREG(es));
    major = LO(ax);
    minor = HI(ax);
    is_MOS = (LWORD(eax) != mosver); // different result!

    LWORD(eax) = 0x5d06;
    call_msdos();
    ds_printf
	("INT21 +3 at %04x:%04x: AX=%04x, BX=%04x, CX=%04x, DX=%04x, DS=%04x, ES=%04x\n",
	 SREG(cs), LWORD(eip), LWORD(eax), LWORD(ebx),
	 LWORD(ecx), LWORD(edx), SREG(ds), SREG(es));
    sda_lo = LWORD(esi);
    sda_hi = SREG(ds);
    sda_size = LWORD(ecx);

    ds_printf("INT21: lol = %04x:%04x\n", lol_hi, lol_lo);
    ds_printf("INT21: sda = %04x:%04x, size = 0x%04x\n", sda_hi, sda_lo, sda_size);
    ds_printf("INT21: ver = 0x%02x, 0x%02x\n", major, minor);

    /* Figure out the redirector version */
    if (is_MOS) {
        redver = REDVER_NONE;
    } else {
        if (major == 3)
            if (minor <= 9)
                redver = (sda_size == SDASIZE_CQ30) ? REDVER_CQ30 : REDVER_PC30;
            else
                redver = REDVER_PC31;
        else
            redver = REDVER_PC40;	/* Most common redirector format */
    }

    if (old_only && redver == REDVER_PC40) {
	_post_msdos();
	return 0;
    }
    /* Try to init the redirector. */
    HI(bx) = redver;
    LWORD(ecx) = lol_hi;
    LWORD(edx) = lol_lo;
    LWORD(esi) = sda_hi;
    LWORD(edi) = sda_lo;
    LO(bx) = DOS_SUBHELPER_MFS_REDIR_INIT;
    LWORD(eax) = DOS_HELPER_MFS_HELPER;
    do_int_call_back(DOS_HELPER_INT);
    is_cf = isset_CF();
    post_msdos();
    if (!is_cf)
	redirect_drives();	/* We have a functioning redirector so use it */
    else
	ds_printf("INT21: this DOS has an incompatible redirector\n");
    return !is_cf;
}

static int redir_it(void)
{
    if (redir_state)
	return 0;
    if (do_redirect(0)) {
	redir_state++;
	return 1;
    }
    return 0;
}

static int enable_redirect(void)
{
    int is_cf;
    uint16_t lol_lo, lol_hi, sda_lo, sda_hi;
    pre_msdos();

    LWORD(eax) = 0x5200;
    call_msdos();
    lol_lo = LWORD(ebx);
    lol_hi = SREG(es);

    LWORD(eax) = 0x5d06;
    call_msdos();
    sda_lo = LWORD(esi);
    sda_hi = SREG(ds);

    LWORD(ecx) = lol_hi;
    LWORD(edx) = lol_lo;
    LWORD(esi) = sda_hi;
    LWORD(edi) = sda_lo;
    LO(bx) = DOS_SUBHELPER_MFS_REDIR_RESET;
    HI(bx) = 0;
    LWORD(eax) = DOS_HELPER_MFS_HELPER;
    do_int_call_back(DOS_HELPER_INT);
    is_cf = isset_CF();
    if (is_cf)
        goto done;
    LWORD(eax) = DOS_SET_REDIRECTION_MODE;
    LO(bx) = REDIR_DISK_TYPE;
    HI(bx) = 1;    // enable
    call_msdos();
    is_cf = isset_CF();
done:
    post_msdos();
    return !is_cf;
}

void dos_post_boot_reset(void)
{
    revect_setup();
    post_boot = 0;
    redir_state = 0;
    redir_state2 = 0;
    if (clnup_handler)
	clnup_handler();
    clnup_handler = NULL;
    syscomdrv.num_scusers = 0;
#ifdef USE_MHPDBG
    mhp_reset_hma();
#endif
}

static void dos_post_boot(void)
{
    if (!post_boot) {
	post_boot = 1;
	post_boot_unrevect();
	/* call vint_setup() again to avoid DOS STACKS hooks, see
	 * https://github.com/dosemu2/dosemu2/issues/1607 */
	vint_setup();
	if (config.force_redir) {
	    if (!redir_state) {
		redir_it();
		redirect_devices();
	    } else {
		enable_redirect();
	    }
	}
#ifdef USE_MHPDBG
        /*
         * Let's set up the hma start address for dosdebug use. There's no
         * specific reason it should be here other than it needs to be done
         * sometime after DOS has started.
         */
        mhp_init_hma();
#endif
	start_pre_strokes();
    }
}

/* KEYBOARD BUSY LOOP */
static int int28(void)
{
    idle_enable(50, 0, "int28");
    return 1;
}

/* FAST CONSOLE OUTPUT */
static int int29(void)
{
    /* char in AL */
    char_out(*(char *) &REG(eax), READ_BYTE(BIOS_CURRENT_SCREEN_PAGE));
    return 1;
}

struct ae00_tab {
    uint8_t max_len;
    struct lowstring cmd;
    char _filler[0];
} __attribute__((packed));

static char *ae00_cmd;

static int run_program_ae00(const char *command)
{
    free(ae00_cmd);
    ae00_cmd = strdup(command);
    return 0;
}

static int run_program_ae01(void)
{
    char *p;
    int cmd_len;
    struct lowstring *name = SEG_ADR((struct lowstring *), ds, si);
    struct ae00_tab *cmd = SEG_ADR((struct ae00_tab *), ds, bx);

    name->len = 0;
    if (!ae00_cmd)
	return 0;
    cmd_len = strlen(ae00_cmd);
    if (cmd_len + 2 > cmd->max_len) {
	error("too long cmd line, %i have %i\n", cmd_len + 2, cmd->max_len);
	goto done;
    }
    strcpy(cmd->_filler, ae00_cmd);
    cmd->_filler[cmd_len] = '\r';
    cmd->cmd.len = cmd_len;
    p = strchr(ae00_cmd, ' ');
    if (p && strlen(p) > 1)
	*p++ = '\0';
    strupperDOS(ae00_cmd);
    strcpy(name->s, ae00_cmd);
    name->len = strlen(ae00_cmd);

done:
    free(ae00_cmd);
    ae00_cmd = NULL;
    return 0;
}

static unsigned short do_get_psp(int parent)
{
    /* don't care about parent, as command.com is primary */
    return sda_cur_psp(sda);
}

static int do_run_cmd(struct lowstring *str, struct ae00_tab *cmd)
{
    char arg0[256];
    char cmdbuf[256];
    int rc;

    memcpy(arg0, str->s, str->len);
    arg0[str->len] = '\0';
    memcpy(cmdbuf, cmd->cmd.s, cmd->cmd.len);
    cmdbuf[cmd->cmd.len] = '\0';
    NOCARRY;
    rc = run_command_plugin(arg0, NULL, cmdbuf, run_program_ae00, do_get_psp);
    if (rc != 0)
	_AL = 0xff; // built-in
    if (rc < 0)
	CARRY;
    return rc;
}

static int int2f(int stk_offs, int revect)
{
    reset_idle(0);
#if 1
    ds_printf
	("INT2F at %04x:%04x: AX=%04x, BX=%04x, CX=%04x, DX=%04x, DS=%04x, ES=%04x\n",
	 SREG(cs), LWORD(eip), LWORD(eax), LWORD(ebx), LWORD(ecx),
	 LWORD(edx), SREG(ds), SREG(es));
#endif

    switch (LWORD(eax)) {
#ifdef IPX
    case INT2F_DETECT_IPX:	/* TRB - detect IPX in int2f() */
	if (config.ipxsup && IPXInt2FHandler())
	    return 1;
	break;
#endif

    case 0xae00:{
	    char cmdname[TITLE_APPNAME_MAXLEN];
	    char appname[TITLE_APPNAME_MAXLEN + 5];
	    struct lowstring *str = SEG_ADR((struct lowstring *), ds, si);
	    struct ae00_tab *cmd = SEG_ADR((struct ae00_tab *), ds, bx);
	    u_short psp_seg;
	    struct MCB *mcb;
	    int len, i;
	    char *ptr, *tmp_ptr;

	    dos_post_boot();

	    if (_DX != 0xffff)
		break;

	    if (!sda)
		break;
	    psp_seg = sda_cur_psp(sda);
	    if (!psp_seg)
		break;
	    if (do_run_cmd(str, cmd) != 0)
		return I_HANDLED;
	    mcb = (struct MCB *) SEG2UNIX(psp_seg - 1);
	    if (!mcb)
		break;

	    if (!Video->change_config)
		break;

	    /* Check whether the program name is invalid (DOS < 4.0) */
	    for (i=0; i<8 && mcb->name[i]; i++) {
		if (iscntrlDOS(mcb->name[i])) {
		    snprintf(title_hint, sizeof title_hint, "COMMAND");
		    goto hint_done;
		}
	    }
	    strncpy(title_hint, mcb->name, 8);
	    title_hint[8] = 0;
hint_done:

	    len = _min(str->len, (unsigned char) (TITLE_APPNAME_MAXLEN - 1));
	    memcpy(cmdname, str->s, len);
	    cmdname[len] = 0;
	    ptr = cmdname + strspn(cmdname, " \t");
	    tmp_ptr = ptr;
	    while (*tmp_ptr) {	/* Check whether the name is valid */
		if (iscntrlDOS(*tmp_ptr++))
		    return 0;
	    }
	    strcpy(title_current, title_hint);
	    if (!ptr[0]) {
		change_window_title(title_current);
		return 0;
	    }
	    snprintf(appname, sizeof(appname), "%s ( %s )",
		     title_current, strlowerDOS(ptr));
	    change_window_title(appname);
	}
	break;
    case 0xae01:
	if (_DX != 0xffff)
	    break;
	run_program_ae01();
	break;
    }

    switch (HI(ax)) {
    case 0x11:			/* redirector call? */
	if (LO(ax) == 0x23)
	    subst_file_ext(SEG_ADR((char *), ds, si));
	if (mfs_redirector(&REGS, MK_FP32(_SS, _SP + stk_offs), revect))
	    return 1;
	break;

    case 0x15:
	if (mscdex())
	    return 1;
	break;

    case 0x16:			/* misc PM/Win functions */
	switch (LO(ax)) {
	case 0x00:		/* WINDOWS ENHANCED MODE INSTALLATION CHECK */
	    if (dpmi_active() && win3x_mode != INACTIVE) {
		D_printf
		    ("WIN: WINDOWS ENHANCED MODE INSTALLATION CHECK: %i\n",
		     win3x_mode);
		if (win3x_mode == ENHANCED)
		    LWORD(eax) = 0x0a03;
		else
		    LWORD(eax) = 0;
		return 1;
	    }
	    break;

	case 0x05:		/* Win95 Initialization Notification */
	    LWORD(ecx) = 0xffff;	/* say it`s NOT ok to run under Win */
	case 0x06:		/* Win95 Termination Notification */
	case 0x07:		/* Win95 Device CallOut */
	case 0x08:		/* Win95 Init Complete Notification */
	case 0x09:		/* Win95 Begin Exit Notification */
	    if (dpmi_active())
		return 1;
	    break;

	case 0x0a:		/* IDENTIFY WINDOWS VERSION AND TYPE */
	    if (dpmi_active() && win3x_mode != INACTIVE) {
		D_printf("WIN: WINDOWS VERSION AND TYPE\n");
		LWORD(eax) = 0;
		LWORD(ebx) = 0x030a;	/* 3.10 */
		LWORD(ecx) = win3x_mode;
		return 1;
	    }
	    break;

	case 0x80:	/* give up time slice */
	    idle_enable(100, 0, "int2f_idle_magic");
	    if (config.hogthreshold) {
		LO(ax) = 0;
		return 1;
	    }
	    break;


	case 0x83:
	    if (dpmi_active() && win3x_mode != INACTIVE) {
		LWORD(ebx) = 1;	/* W95: number of virtual machine */
		return 1;
	    }
	    break;
	case 0x81:		/* W95: enter critical section */
	    if (dpmi_active() && win3x_mode != INACTIVE) {
		D_printf("WIN: enter critical section\n");
		/* LWORD(eax) = 0;  W95 DDK says no return value */
		return 1;
	    }
	    break;
	case 0x82:		/* W95: exit critical section */
	    if (dpmi_active() && win3x_mode != INACTIVE) {
		D_printf("WIN: exit critical section\n");
		/* LWORD(eax) = 0;  W95 DDK says no return value */
		return 1;
	    }
	    break;

	case 0x84:		/* Win95 Get Device Entry Point */
	    LWORD(edi) = 0;
	    WRITE_SEG_REG(es, 0);	/* say NO to Win95 ;-) */
	    return 1;
	case 0x85:		/* Win95 Switch VM + Call Back */
	    CARRY;
	    LWORD(eax) = 1;
	    return 1;

	case 0x86:
	    D_printf("DPMI CPU mode check in real mode.\n");
	    /* for protected-mode counterpart see do_dpmi_int() */
	    return 1;

	case 0x87:		/* Call for getting DPMI entry point */
	    dpmi_get_entry_point();
	    return 1;
	}
	break;

#define DE2_CLIP_MAGIC 0x3244  // "D2"
    case 0x17:			/* MS Windows WINOLDAP functions */
        switch (LO(ax)) {
            case 0x00:		/* IDENTIFY WinOldAp VERSION */
                LO(ax) = 0x01;  // major version
                HI(ax) = 0x00;  // minor version
                v_printf("Check for WinOldAp\n"); // Installed == (AX != 1700)
                return 1;
            case 0x01:          /* OPEN CLIPBOARD */
                v_printf("Open clipboard\n");
                if (Clipboard && Clipboard->open) {
                    int ok = Clipboard->open();
                    LWORD(eax) = (ok ? DE2_CLIP_MAGIC : 0);
                } else
                    LWORD(eax) = 0;
                return 1;
            case 0x02:          /* EMPTY CLIPBOARD */
                v_printf("Clear clipboard\n");
                if (Clipboard && Clipboard->clear)
                    LWORD(eax) = Clipboard->clear();
                else
                    LWORD(eax) = 0;
                return 1;
            case 0x03:          /* WRITE CLIPBOARD */
                v_printf("Write clipboard\n");
                if (Clipboard && Clipboard->write) {
                    char *pbuf = MK_FP32(SREG(es), LWORD(ebx));
                    unsigned int bsize = LWORD(esi) << 16 | LWORD(ecx);
                    LWORD(eax) = Clipboard->write(LWORD(edx), pbuf, bsize);
                } else
                    LWORD(eax) = 0;
                return 1;
            case 0x04:           /* GET CLIPBOARD DATA SIZE */
                v_printf("Get clipboard size\n");
                if (Clipboard && Clipboard->getsize) {
                    unsigned clipb_size = Clipboard->getsize(LWORD(edx));
                    LWORD(edx) = (clipb_size >> 16);
                    LWORD(eax) = (clipb_size & 0xffff);
                } else {
                    LWORD(edx) = LWORD(eax) = 0;
                }
                return 1;
            case 0x05:           /* GET CLIPBOARD DATA */
                v_printf("Get clipboard data\n");
                if (Clipboard && Clipboard->getdata) {
                    unsigned clipb_size = 0xffff;
                    char *pbuf = MK_FP32(SREG(es), LWORD(ebx));
                    if (LWORD(edi) == DE2_CLIP_MAGIC)
                        clipb_size = LWORD(ecx);
                    LWORD(eax) = Clipboard->getdata(LWORD(edx), pbuf, clipb_size);
                } else
                    LWORD(eax) = 0;
                return 1;
            case 0x08:          /* CLOSE CLIPBOARD */
                v_printf("Close clipboard\n");
                if (Clipboard && Clipboard->close)
                    Clipboard->close();
                LWORD(eax) = 1;
                return 1;
            case 0x09:          /* COMPACT CLIPBOARD */
                // SI:CX = desired size in bytes
                // DX:AX = number of bytes in largest block of free memory
                LWORD(edx) = 0;
                LWORD(eax) = 0x1000; // 4K should indicate enough room
                v_printf("Compact clipboard\n");
                return 1;
            case 0x0a:          /* GET DEVICE CAPABILITIES */
                LWORD(eax) = 0; // Not sure if we should "support" this
                                // function, but doesn't seem possible to
                                // indicate otherwise.
                v_printf("Get device capabilities (0x%02x)\n", LWORD(edx));
                return 1;
            default:
                v_printf("BAD WinOldAp func int 2f/ax=0x%04x\n", LWORD(eax));
        }
        break;

    case INT2F_XMS_MAGIC:
	if (!xms_intdrv())
	    break;
	switch (LO(ax)) {
	case 0:		/* check for XMS */
	    x_printf("Check for XMS\n");
	    LWORD(eax) = xms_install_check();
	    break;
	case 0x10:
	    x_printf("Get XMSControl address\n");
	    /* SREG(es) = XMSControl_SEG; */
	    WRITE_SEG_REG(es, XMSControl_SEG);
	    LWORD(ebx) = XMSControl_OFF;
	    break;
	default:
	    x_printf("BAD int 0x2f XMS function:0x%02x\n", LO(ax));
	}
	return 1;

    case 0x4a:
	switch (LO(ax)) {
	case 0:
	    if (HI(dx) >= config.fdisks) {
		LWORD(ecx) = 0xffff;	// no such floppy
		return 1;
	    }
	    break;
	}
	break;
    }

    return 0;
}

static void int33_check_hog(void);

/* mouse */
static int int33(void)
{
/* New code introduced by Ed Sirett (ed@cityscape.co.uk)  26/1/95 to give
 * garrot control when the dos app is polling the mouse and the mouse is
 * taking a break. */

/* Firstly do the actual mouse function. */
/* N.B. This code lets the real mode mouse driver return at a HLT, so
 * after it returns the hogthreshold code can do its job.
 */
    mouse_int();
    int33_check_hog();
    return 1;
}

static void int33_check_hog(void)
{
    static unsigned short int oldx = 0, oldy = 0;

/* It seems that the only mouse sub-function that could be plausibly used to
 * poll the mouse is AX=3 - get mouse buttons and position.
 * The mouse driver should have left AX=3 unaltered during its call.
 * The correct response should have the buttons in the low 3 bits in BX and
 * x,y in CX,DX.
 * Some programs seem to interleave calls to read mouse with various other
 * sub-functions (Esp. 0x0b  0x05 and 0x06)
 * As a result we do not reset the  trigger value in these cases.
 * Sadly, some programs use the user-specified mouse-event handler function (0x0c)
 * after which they then wait for mouse events presumably in a tight loop, I think
 * that we won't be able to stop these programs from burning CPU cycles.
 */
    if (LWORD(eax) == 0x0003) {
	if (LWORD(ebx) == 0 && oldx == LWORD(ecx) && oldy == LWORD(edx))
	    trigger_idle();
	else {
	    reset_idle(0);
	    oldx = LWORD(ecx);
	    oldy = LWORD(edx);
	}
    }
    m_printf("Called/ing the mouse with AX=%x \n", LWORD(eax));
    /* Ok now we test to see if the mouse has been taking a break and we can let the
     * system get on with some real work. :-) */
    idle(200, 20, 20, "mouse");
}

static int int66(void)
{
    switch (_AH) {
    case 0x80:
	m_printf("mouse: int66 ah=0x80, si=%x\n", _SI);
	mouse_set_win31_mode();
	break;
    default:
	CARRY;
	break;
    }
    return 0;
}

static int int67(void)
{
    return ems_fn(&REGS);
}

static void debug_int(const char *s, int i)
{
    di_printf
	    ("%s INT0x%02x eax=0x%08x ebx=0x%08x ss=0x%04x esp=0x%08x\n"
	     "           ecx=0x%08x edx=0x%08x ds=0x%04x  cs=0x%04x ip=0x%04x\n"
	     "           esi=0x%08x edi=0x%08x es=0x%04x flg=0x%08x\n", s,
	     i, _EAX, _EBX, _SS, _ESP, _ECX, _EDX, _DS, _CS, _IP, _ESI,
	     _EDI, _ES, (int) read_EFLAGS());
}

static void do_int_from_thr(void *arg)
{
    u_char i = (long) arg;
    run_caller_func(i, NO_REVECT, 6);
/* for now dosdebug uses int_revect feature, so this should be disabled
 * or it will display the same entry twice */
#if 0
#ifdef USE_MHPDBG
    mhp_debug(DBG_INTx + (i << 8), 0, 0);
#endif
#endif
}

/*
 * DANG_BEGIN_FUNCTION DO_INT
 *
 * description:
 * DO_INT is used to deal with interrupts returned to DOSEMU by the
 * kernel.
 *
 * DANG_END_FUNCTION
 */

static void do_int_from_hlt(Bit16u i, HLT_ARG(arg))
{
    if (debug_level('#') > 2)
	debug_int("Do", i);

    /* Always use the caller function: I am calling into the
       interrupt table at the start of the dosemu bios */
    if (int_handlers[i].interrupt_function[NO_REVECT])
	coopth_start(int_tid + i, (void *) (long) i);
    else
	fake_iret();
}

static void do_rvc_chain(int i, int stk_offs)
{
    int ret = run_caller_func(i, REVECT, stk_offs);
    clear_SF();
    switch (ret) {
    case I_SECOND_REVECT:
	di_printf("int_rvc 0x%02x setup\n", i);
	if (int_handlers[i].secrevect_function) {
	    /* if function supported, CF will be cleared on success, but for
	     * unsupported functions it will stay unchanged */
	    CARRY;
	}
	set_SF();
	/* no break */
    case I_NOT_HANDLED:
	di_printf("int 0x%02x, ax=0x%04x\n", i, LWORD(eax));
	set_ZF();
	break;
    case I_HANDLED:
	clear_ZF();
	break;
    }
}

static void do_basic_revect_thr(void *arg)
{
    int i = (long) arg;
    run_caller_func(i, REVECT, 0);
}

void do_int(int i)
{
#if 1
    /* we must clear the AC flag here since real mode INT instructions
       do that too. An IRET (not IRETD) instruction then does not set AC
       because the AC flag is in the high part of the eflags.
       DOS applications usually set AC to try to detect the presence of
       a 486. They hopefully protect this test using cli and sti, or
       hardware INTs will mess things up.
     */
    if (IS_CR0_AM_SET())
	clear_AC();
#else
    fake_int_to(BIOS_HLT_BLK_SEG, iret_hlt_off);
#endif

#if 1
    /* try to catch jumps to 0:0 (e.g. uninitialized user interrupt vectors),
       which sometimes can crash the whole system, not only dosemu... */
    if (SEGOFF2LINEAR(ISEG(i), IOFF(i)) < 1024) {
	error
	    ("OUCH! attempt to execute interrupt table - quickly dying\n");
    /* unfortunately World-Wide-Rally game puts trampoline into IVT,
     * likely because of some NULL deref bug. So do not exit. */
//	leavedos(57);
    }
#endif

    if (test_bit(i, &vm86s.int_revectored) && !mhp_revectored(i)) {
	assert(int_handlers[i].interrupt_function[REVECT]);
	if (debug_level('#') > 2)
	    debug_int("Do rvc", i);
	if (int_handlers[i].revect_function)
	    int_handlers[i].revect_function();
	else
	    coopth_start(int_rvc_tid, (void *) (long) i);
    } else {
	di_printf("int 0x%02x, ax=0x%04x\n", i, LWORD(eax));
	if (IS_IRET(i)
#ifdef USE_MHPDBG
		    && !mhpdbg.active
#endif
	) {
	    if ((i != 0x2a) && (i != 0x28))
		g_printf("just an iret 0x%02x\n", i);
	} else {
	    real_run_int(i);
	}
    }
}

void fake_int(int cs, int ip)
{
    unsigned int ssp, sp;

    g_printf("fake_int: CS:IP %04x:%04x\n", cs, ip);
    ssp = SEGOFF2LINEAR(SREG(ss), 0);
    sp = LWORD(esp);

    pushw(ssp, sp, vflags);
    pushw(ssp, sp, cs);
    pushw(ssp, sp, ip);
    LWORD(esp) -= 6;

    clear_TF();
    clear_NT();
    if (IS_CR0_AM_SET())
	clear_AC();
    clear_IF();
}

void fake_int_to(int cs, int ip)
{
    fake_int(SREG(cs), LWORD(eip));
    SREG(cs) = cs;
    REG(eip) = ip;
}

void fake_call(int cs, int ip)
{
    unsigned int ssp, sp;

    ssp = SEGOFF2LINEAR(SREG(ss), 0);
    sp = LWORD(esp);

    g_printf("fake_call() CS:IP %04x:%04x\n", cs, ip);
    pushw(ssp, sp, cs);
    pushw(ssp, sp, ip);
    LWORD(esp) -= 4;
}

void fake_call_to(int cs, int ip)
{
    fake_call(SREG(cs), LWORD(eip));
    SREG(cs) = cs;
    REG(eip) = ip;
}

void fake_pusha(void)
{
    unsigned int ssp, sp;

    ssp = SEGOFF2LINEAR(SREG(ss), 0);
    sp = LWORD(esp);

    pushw(ssp, sp, LWORD(eax));
    pushw(ssp, sp, LWORD(ecx));
    pushw(ssp, sp, LWORD(edx));
    pushw(ssp, sp, LWORD(ebx));
    pushw(ssp, sp, LWORD(esp));
    pushw(ssp, sp, LWORD(ebp));
    pushw(ssp, sp, LWORD(esi));
    pushw(ssp, sp, LWORD(edi));
    LWORD(esp) -= 16;
    pushw(ssp, sp, SREG(ds));
    pushw(ssp, sp, SREG(es));
    LWORD(esp) -= 4;
}

void fake_retf(void)
{
    unsigned int ssp, sp;

    ssp = SEGOFF2LINEAR(SREG(ss), 0);
    sp = LWORD(esp);

    _IP = popw(ssp, sp);
    _CS = popw(ssp, sp);
    _SP += 4;
}

void fake_iret(void)
{
    unsigned int ssp, sp;
#ifdef USE_MHPDBG
    int old_tf = isset_TF();
#endif

    ssp = SEGOFF2LINEAR(SREG(ss), 0);
    sp = LWORD(esp);

    _IP = popw(ssp, sp);
    _CS = popw(ssp, sp);
    set_FLAGS(popw(ssp, sp));
    _SP += 6;
#ifdef USE_MHPDBG
    if (mhpdbg.active && old_tf)
	set_TF();
#endif
}

void do_eoi_iret(void)
{
    _CS = BIOSSEG;
    _IP = EOI_OFF;
}

void do_eoi2_iret(void)
{
    _CS = BIOSSEG;
    _IP = EOI2_OFF;
}

void do_iret(void)
{
    _CS = BIOSSEG;
    _IP = IRET_OFF;
}

static void rvc_int_pre(int tid, void *arg, void *arg2)
{
    coopth_push_user_data(tid, (void *) (long) get_FLAGS(REG(eflags)));
    clear_TF();
    clear_NT();
#if 0
    /* AC already cleared in do_int() */
    if (IS_CR0_AM_SET())
	clear_AC();
#endif
    clear_IF();
}

static void rvc_int_post(int tid, void *arg, void *arg2)
{
    u_short flgs = (long) coopth_pop_user_data(tid);
    if (flgs & IF)
	set_IF();
    else
	clear_IF();
    REG(eflags) |= (flgs & (TF_MASK | NT_MASK));
}

/* dummy sleep handler for sanity checks */
static void rvc_int_sleep(int tid, int sl_state)
{
    /* new revect code: all CHAIN_REVECT handlers share the same
     * coopth tid. This means that deep sleeps should be forbidden
     * because we can't awake thread on the same tid. */
    assert(sl_state != COOPTH_SL_SLEEP);
}

#define INT_WRP(n) \
static int _int##n##_(int stk_offs, int revect) \
{ \
  return int##n(); \
}

/* Needed for int16, which can clash with type name.
 * Under clang, both _int16 and __int16 are also occupied. */
#define INT_WRP2(n) \
static int _int##n##_(int stk_offs, int revect) \
{ \
  return ___int##n(); \
}

INT_WRP(05)
INT_WRP(10)
INT_WRP(11)
INT_WRP(12)
INT_WRP(13)
INT_WRP(14)
INT_WRP(15)
INT_WRP2(16)
INT_WRP(17)
INT_WRP(18)
INT_WRP(19)
INT_WRP(1a)
INT_WRP(28)
INT_WRP(29)
INT_WRP(33)
INT_WRP(66)
INT_WRP(67)

static int _ipx_int7a(int stk_offs, int revect)
{
#ifdef IPX
    return ipx_int7a();
#else
    return 0;
#endif
}

static void revect_setup(void)
{
    int i;

    memset(&vm86s.int_revectored, 0x00, sizeof(vm86s.int_revectored));
    if (config.force_revect != 0) {
	for (i = 0; i < 0x100; i++) {
	    if (int_handlers[i].interrupt_function[REVECT])
		set_bit(i, &vm86s.int_revectored);
	}
    }

    int21_hooked = 0;
    int28_hooked = 0;
    int2f_hooked = 0;
    int33_hooked = 0;
    inte6_hooked = 0;
    int_revect_post_init();
}

/*
 * DANG_BEGIN_FUNCTION setup_interrupts
 *
 * description:
 * SETUP_INTERRUPTS is used to initialize the interrupt_function
 * array which directs handling of interrupts in protected mode and
 * also initializes the base vector for interrupts in real mode.
 *
 * DANG_END_FUNCTION
 */

void setup_interrupts(void)
{
    int i;
    emu_hlt_t hlt_hdlr = HLT_INITIALIZER;

#define SIFU(n, r, h) do { \
    int_handlers[n].interrupt_function_arr[NO_REVECT][r] = h; \
    int_handlers[n].interrupt_function_arr[REVECT][r] = h; \
} while (0)
#define SI2FU(n, h) do { \
    int_handlers[n].interrupt_function_arr[NO_REVECT][NO_REVECT] = h; \
    int_handlers[n].interrupt_function_arr[REVECT][REVECT] = h; \
} while (0)
    /* init trapped interrupts called via jump */
    for (i = 0; i < 256; i++) {
	SIFU(i, NO_REVECT, NULL);
	SIFU(i, REVECT, NULL);
    }

    SIFU(5, NO_REVECT, _int05_);
    /* This is called only when revectoring int10 */
    SIFU(0x10, NO_REVECT, _int10_);
    SIFU(0x11, NO_REVECT, _int11_);
    SIFU(0x12, NO_REVECT, _int12_);
    SIFU(0x13, NO_REVECT, _int13_);
    SIFU(0x14, NO_REVECT, _int14_);
    SIFU(0x15, NO_REVECT, _int15_);
    SIFU(0x16, NO_REVECT, _int16_);
    SIFU(0x17, NO_REVECT, _int17_);
    SIFU(0x18, NO_REVECT, _int18_);
    SIFU(0x19, NO_REVECT, _int19_);
    SIFU(0x1a, NO_REVECT, _int1a_);
    SIFU(0x67, NO_REVECT, _int67_);

    int_handlers[0x21].revect_function = int21_revect;
    SIFU(0x21, REVECT, msdos_chainrevect);
    int_handlers[0x21].secrevect_function = msdos_xtra;
    int_handlers[0x21].interrupt_function_arr[NO_REVECT][NO_REVECT] =
	    msdos_xtra_norev;
    int_handlers[0x21].unrevect_function = int21_unrevect;
    SI2FU(0x28, _int28_);
    int_handlers[0x28].revect_function = int28_revect;
    int_handlers[0x28].unrevect_function = int28_unrevect;
    SIFU(0x29, NO_REVECT, _int29_);
    int_handlers[0x2f].revect_function = int2f_revect;
    SI2FU(0x2f, int2f);
    int_handlers[0x2f].unrevect_function = int2f_unrevect;
    if (config.mouse.intdrv) {
	/* only for unrevect_fixup, otherwise unneeded revect */
	int_handlers[0x33].revect_function = int33_revect;
	SIFU(0x33, REVECT, _int33_);
	int_handlers[0x33].unrevect_function = int33_unrevect_fixup;
    }
    if (config.ipxsup)
	SIFU(0x7a, NO_REVECT, _ipx_int7a);
    SIFU(DOS_HELPER_INT, NO_REVECT, dos_helper);
    /* Multitasking DOS 4.0 hooks all ints, including 0xe6, so we revect it. */
    int_handlers[DOS_HELPER_INT].revect_function = inte6_revect_fixup;
    /* hook even in !set_int_hooks mode */
    SIFU(DOS_HELPER_INT, REVECT, inte6_rvc_dummy);
    int_handlers[DOS_HELPER_INT].unrevect_function = inte6_unrevect_fixup;
    /* those are unused */
    (void)inte6_revect;
    (void)inte6_unrevect_simple;

    /* set up relocated video handler (interrupt 0x42) */
    if (config.dualmon == 2) {
	int_handlers[0x42] = int_handlers[0x10];
    }

    hlt_hdlr.name = "interrupts";
    hlt_hdlr.len = 256;
    hlt_hdlr.func = do_int_from_hlt;
    hlt_off = hlt_register_handler_vm86(hlt_hdlr);

    int_tid = coopth_create_multi("ints thread non-revect", 256,
	do_int_from_thr);
    coopth_set_permanent_post_handler(int_tid, ret_from_int);
    int_rvc_tid = coopth_create("ints thread revect", do_basic_revect_thr);
    coopth_set_ctx_handlers(int_rvc_tid, rvc_int_pre, rvc_int_post, NULL);
    coopth_set_sleep_handlers(int_rvc_tid, rvc_int_sleep, NULL);

    int_revect_init();
}

void int_try_disable_revect(void)
{
    int i;

    if (config.force_revect != -1)
	return;
    config.force_revect = 0;
    for (i = 0; i < 256; i++) {
	if (test_bit(i, &vm86s.int_revectored) && !mhp_revectored(i))
	    clear_bit(i, &vm86s.int_revectored);
    }
}

void update_xtitle(void)
{
    char cmdname[9];
    char *cmd_ptr, *tmp_ptr;
    u_short psp_seg;
    struct MCB *mcb;
    int force_update;

    if (!sda)
	return;
    psp_seg = sda_cur_psp(sda);
    if (!psp_seg)
	return;
    mcb = (struct MCB *) SEG2UNIX(psp_seg - 1);
    if (!mcb)
	return;
    force_update = !title_hint[0];

    MEMCPY_P2UNIX(cmdname, (unsigned char *) mcb->name, 8);
    cmdname[8] = 0;
    cmd_ptr = tmp_ptr = cmdname + strspn(cmdname, " \t");
    while (*tmp_ptr) {		/* Check whether the name is valid */
	if (iscntrlDOS(*tmp_ptr++))
	    return;
    }

    if (win3x_mode != INACTIVE && memcmp(cmd_ptr, "krnl", 4) == 0) {
	cmd_ptr = win3x_title;
	force_update = 1;
    }

    if (force_update || strcmp(title_current, title_hint) != 0) {
	if (force_update || strcmp(cmd_ptr, title_hint) == 0) {
	    if (force_update || can_change_title) {
		if (strcmp(title_current, cmd_ptr) == 0)
		    return;
		strlcpy(title_current, cmd_ptr, sizeof(title_current));
		change_window_title(title_current);
	    }
	} else {
	    can_change_title = 1;
	}
    }
}

static int msdos_remap_extended_open_(void)
{
  uint16_t _si_ = LWORD(esi);
  uint8_t _bl_ = LO(bx);
  uint8_t _dl_ = LO(dx);
  char *src = MK_FP32(SREG(ds), _si_);
  int found, create_truncate;

  ds_printf("INT21: extended open '%s'\n", src);

  /* Get file attributes */
  LWORD(eax) = 0x4300;
  LWORD(edx) = _si_;                           // Filename passed in DS:DX
  call_msdos();
  if ((REG(eflags) & CF) && LWORD(eax) != 0x02)
    return 0;
  found = !(REG(eflags) & CF);

  ds_printf("INT21: extended open file does%s exist\n", found ? "" : " not");
  ds_printf("INT21: extended open _dl == 0x%02x\n", _dl_);

  /*
   * Bitfields for action:
   * Bit(s)	Description	)
   *  7-4	action if file does not exist
   *  	        0000 fail
   *  		0001 create
   *  3-0	action if file exists
   *  		0000 fail
   *  		0001 open
   *  		0010 replace/open
   */
  if (!found) {
    if (!(_dl_ & 0b00010000)) {          // fail if doesn't exist
      LWORD(eax) = 0x02;
      return 0;
    }
  } else {
    if (!(_dl_ & 0b00000011)) {          // fail if exists
      LWORD(eax) = 0x50;
      return 0;
    }
  }

  create_truncate = (_dl_ & 0b00010010);

  /* Choose Create/Truncate or Open function */
  HI(ax) = create_truncate ? 0x3c : 0x3d;

  /* Map the extended open into something the underlying DOS can understand */
  /*
     low byte of BX (only bit 2 lost)
       bit 2 read-only no update access time

     high byte of BX (all bits lost)
       bit 8 disable buffering
       bit 9 disable file compression
       bit 10 use _DI value for file name mangling
       bit 13 return error instead of int 24
       bit 14 synchronous writes
   */
  LO(ax) = (_bl_ & 0b11110011);

  /* Filename passed in DS:DX */
  LWORD(edx) = _si_;

  call_msdos();

  if (REG(eflags) & CF)                        // we failed
    return 0;

  /* Tell the caller what action was taken
       (According to RBIL these values are lost when open uses redirector)
     0x0001 - file opened
     0x0002 - file created
     0x0003 - file truncated
   */
  if (!create_truncate)
    LWORD(ecx) = 0x0001;
  else
    LWORD(ecx) = !found ? 0x0002 : 0x0003;

  return 1;
}

static int msdos_remap_extended_open(void)
{
  int carry, ret;

  carry = (REG(eflags) & CF);
  ret = msdos_remap_extended_open_();
  /* preserve carry if we forward the request */
  if (ret == 0 && carry)
    CARRY;
  return ret;
}

far_t get_int_vector(int vec)
{
    far_t addr;

    if (test_bit(vec, &vm86s.int_revectored)) {
	addr.segment = INT_RVC_SEG;
	switch (vec) {
	case 0x21:
	    int21_rvc_setup();
	    addr.offset = INT_RVC_21_OFF;
	    return addr;
	case 0x2f:
	    int2f_rvc_setup();
	    addr.offset = INT_RVC_2f_OFF;
	    return addr;
	case 0x33:
	    int33_rvc_setup();
	    addr.offset = INT_RVC_33_OFF;
	    return addr;
	}
    }
    addr.segment = ISEG(vec);
    addr.offset = IOFF(vec);
    return addr;
}
