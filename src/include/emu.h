/* dos emulator, Matthias Lautner
 * Extensions by Robert Sanders, 1992-93
 *
 */

#ifndef EMU_H
#define EMU_H

#include <stdio.h>
#include <sys/types.h>
/* androind NDK hack */
#ifdef HAVE_BTHREAD_H
#include <pthread.h>
#include <bthread.h>
#endif
#include "types.h"
#include "cpu.h"
#include "priv.h"
#include "mouse.h"
#include "dosemu_config.h"

#define MULTICORE_EXAMPLE 0
#if MULTICORE_EXAMPLE
#define __TLS __thread
#else
#define __TLS
#endif

extern char * const *dosemu_envp;

int vm86_init(void);
int vm86_fault(unsigned trapno, unsigned err, dosaddr_t cr2);
#ifdef __i386__
void true_vm86_fault(sigcontext_t *scp);
#endif

#define BIT(x)  	(1<<x)

#define CC_SUCCESS			0x00

/*
 * DANG_BEGIN_REMARK
   The `vm86_struct` is used to pass all the necessary status/registers to
   DOSEMU when running in vm86 mode.
 * DANG_END_REMARK
*/

union vm86_union
{
  struct vm86_struct vm86ps;
  unsigned char b[sizeof(struct vm86_struct)];
  unsigned short w[sizeof(struct vm86_struct)/2];
  unsigned int d[sizeof(struct vm86_struct)/4];
  struct vm86plus_struct vm86compat;
};

extern __TLS union vm86_union vm86u;
#define vm86s (vm86u.vm86ps)

int signal_pending(void);
extern volatile int fault_cnt;
extern int terminal_pipe;
extern int terminal_fd;
extern int kernel_version_code;

#if 0
/*
 * 1) this stuff is unused
 * 2) it should be FORBIDDEN to use global names less than 4 chars long!
 */
extern char *cl,		/* clear screen */
*le,				/* cursor left */
*cm,				/* goto */
*ce,				/* clear to end */
*sr,				/* scroll reverse */
*so,				/* stand out start */
*se,				/* stand out end */
*md,				/* hilighted */
*mr,				/* reverse */
*me,				/* normal */
*ti,				/* terminal init */
*te,				/* terminal exit */
*ks,				/* init keys */
*ke,				/* ens keys */
*vi,				/* cursor invisible */
*ve;				/* cursor normal */
#endif

/* the fd for the keyboard */
extern int console_fd;
extern int no_local_video; /* used by virtual port code */
/* the file descriptor for /dev/mem mmap'ing */
extern int mem_fd;
extern volatile int in_vm86;

extern FILE *real_stderr;

void dos_ctrl_alt_del(void);	/* disabled */

extern void vm86_helper(void);
extern void run_vm86(void);
extern void loopstep_run_vm86(void);
extern int do_call_back(Bit16u cs, Bit16u ip);
extern int do_int_call_back(int intno);

void getKeys(void);

#include "dosemu_debug.h"

void char_out(u_char, int);
void keybuf_clear(void);

/* this macro can be safely wrapped around a system call with no side
 * effects; using a feature of GCC, it returns the same value as the
 * function call argument inside.
 *
 * this is best used in places where the errors can't be sanely handled,
 * or are not expected...
 */
#define DOS_SYSCALL(sc) ({ int s_tmp = (int)sc; \
  if (s_tmp == -1) \
    error("SYSCALL ERROR: %d, *%s* in file %s, line %d: expr=\n\t%s\n", \
	  errno, strerror(errno), __FILE__, __LINE__, #sc); \
  s_tmp; })

#define SILENT_DOS_SYSCALL(sc) sc

#if 1
#define RPT_SYSCALL(sc) ({ int s_tmp, s_err; \
   do { \
	  s_tmp = sc; \
	  s_err = errno; \
      } while ((s_tmp == -1) && (s_err == EINTR)); \
  s_tmp; })
#else
#define RPT_SYSCALL(sc) (sc)
#endif

typedef struct vesamode_type_struct {
  struct vesamode_type_struct *next;
  unsigned width, height, color_bits;
} vesamode_type;


typedef struct config_info {
       int hdiskboot;
       boolean swap_bootdrv;
       boolean alt_drv_c;
       uint8_t drive_c_num;
       uint32_t drives_mask;
       int try_freedos;
       uint64_t boot_dos;

#ifdef X86_EMULATOR
       #define EMU_V86() (config.cpu_vm == CPUVM_EMU)
       #define EMU_DPMI() (config.cpu_vm_dpmi == CPUVM_EMU)
       #define EMU_FULL() (EMU_V86() && EMU_DPMI())
       #define IS_EMU() (EMU_V86() || EMU_DPMI())
       boolean cpusim;
#endif
       int cpu_vm;
       int cpu_vm_dpmi;
       boolean dpmi_remote;
       int CPUSpeedInMhz;
       /* for video */
       int console_video;
       int term;
       char *term_size;
       int dumb_video;
       int tty_stderr;
       int clip_term;
       int vga;
       boolean X;
       boolean X_fullscreen;
       boolean sdl;
       boolean vga_fonts;
       int sdl_sound;
       int libao_sound;
       u_short cardtype;
       u_short chipset;
       boolean pci;
       boolean pci_video;
       long gfxmemsize;		/* for SVGA card, in K */
       u_short term_color;		/* Terminal color support on or off */
       u_short term_esc_char;	        /* ASCII value used to access slang help screen */
       char    *xterm_title;	        /* xterm/putty window title */
       char    *X_display;              /* X server to use (":0") */
       char    *X_title;                /* X window title */
       int X_title_show_appname;        /* show name of running app in caption */
       char    *X_icon_name;
       char    *X_font;
       char    *X_mgrab_key;		/* KeySym name to activate mouse grab */
					/* "" turns it of, NULL gives the default ("Home") */
       int     X_blinkrate;
       int     X_sharecmap;
       int     X_mitshm;                /* use MIT SHM extension */
       int     X_fixed_aspect;          /* keep initial aspect ratio while resizing windows */
       int     X_aspect_43;             /* set aspect ratio to 4:3 */
       int     X_lin_filt;              /* interpolate linear */
       int     X_bilin_filt;            /* dto, bilinear */
       int     X_winsize_x;             /* initial window width */
       int     X_mode13fact;            /* initial size factor for mode 0x13 */
       int     X_winsize_y;             /* initial window height */
       unsigned X_gamma;		/* gamma correction value */
       u_long vgaemu_memsize;		/* for VGA emulation */
       vesamode_type *vesamode_list;	/* chained list of VESA modes */
       int     X_lfb;			/* support VESA LFB modes */
       int     X_pm_interface;		/* support protected mode interface */
       int     X_background_pause;	/* pause xdosemu if it loses focus */
       boolean X_noclose;		/* hide the window close button, disable close menu entry */
       boolean X_noresize;		/* disable resize on window borders */
       boolean sdl_hwrend;		/* accelerate SDL with OpenGL */
       boolean sdl_wcontrols;		/* enable window controls */
       char    *sdl_fonts;		/* TTF font used in SDL2 */
       boolean sdl_clip_native;		/* enable native clipboard */
       boolean fullrestore;
       boolean force_vt_switch;         /* in case of console_video force switch to emu VT at start */
       int     dualmon;

       int     console_keyb;
       boolean kbd_tty;
       boolean X_keycode;	/* use keycode field of event structure */
       boolean exitearly;
       boolean quiet;
       int     realcpu;
       boolean mathco, smp, cpuprefetcht0, cpufxsr, cpusse, umip;
       boolean ipxsup;
       unsigned ipx_net;
       int     vnet;
       char   *ethdev;
       char   *tapdev;
       char   *vdeswitch;
       char   *slirp_args;
       char   *netsock;
       boolean pktdrv;
       boolean tcpdrv;
       char   *tcpiface;
       uint32_t tcpgw;
       boolean ne2k;
       boolean emuretrace;
       boolean mapped_bios;	/* video BIOS */
       char *vbios_file;	/* loaded VBIOS file */
       char *vgaemubios_file;	/* loaded VBIOS file */
       boolean vbios_copy;
       int vbios_seg;           /* VGA-BIOS-segment for mapping */
       int vbios_size;          /* size of VGA-BIOS (64K for vbios_seg=0xe000
       						     32K for vbios_seg=0xc000) */
       boolean vbios_post;

       int  fastfloppy;
       char *emusys;		/* map CONFIG.SYS to CONFIG.EMU */

       u_short speaker;		/* 0 off, 1 native, 2 emulated */
       u_short fdisks, hdisks;
       u_short num_lpt;
       u_short num_ser;
       mouse_t mouse;
       int num_serial_mices;

       int pktflags;		/* global flags for packet driver */

       int update, freq;	/* temp timer magic */
       unsigned long cpu_spd;		/* (1/speed)<<32 */
       unsigned long cpu_tick_spd;	/* (1.19318/speed)<<32 */

       int hogthreshold;

       int mem_size, ext_mem, xms_size, ems_size;
       int umb_a0, umb_b0, umb_b8, umb_f0, hma;
       unsigned int ems_frame;
       int ems_uma_pages, ems_cnv_pages;
       int dpmi, pm_dos_api, no_null_checks;
       uint32_t dpmi_base;
       int dos_up;

       int sillyint;            /* IRQ numbers for Silly Interrupt Generator
       				   (bitmask, bit3..15 ==> IRQ3 .. IRQ15) */

       int layout_auto;
       struct keytable_entry *keytable;
       struct keytable_entry *altkeytable;
       const char *internal_cset;
       const char *external_cset;
       int country;

       unsigned short detach;
       char *debugout;
       char *pre_stroke;        /* pointer to keyboard pre strokes */

       /* Lock File business */
       int file_lock_limit;
       char *tty_lockdir;	/* The Lock directory  */
       char *tty_lockfile;	/* Lock file pretext ie LCK.. */
       boolean tty_lockbinary;	/* Binary lock files ? */

       /* LFN support */
       boolean lfn;
       int int_hooks;
       int force_revect;
       int trace_irets;
       boolean force_redir;

       boolean dos_trace;	/* SWITCHES=/Y */

       /* type of mapping driver */
       char *mappingdriver;

       /* List of temporary hacks
        * (at minimum 16, will be increased when needed)
        *
        * If a 'features' becomes obsolete (problem solved) it will
        * remain dummy for a while before re-used.
        *
        * NOTE: 'features' are not subject to permanent documentation!
        *
        * Currently assigned:
        *
        *   (none)
        */
       int features[16];

       /* Time mode is TM_BIOS / TM_PIT / TM_LINUX, see iodev.h */
       int timemode;

       /* Sound emulation */
       int sound;
       uint16_t sb_base;
       uint8_t sb_dma;
       uint8_t sb_hdma;
       uint8_t sb_irq;
       uint16_t mpu401_base;
       int mpu401_irq;
       int mpu401_irq_mt32;
       uint16_t mpu401_base_mt32;
       char *midi_synth;
       char *sound_driver;
       char *midi_driver;
       char *fluid_sfont;
       int fluid_volume;
       char *munt_roms_dir;
       char *snd_plugin_params;
       boolean pcm_hpf;
       char *midi_file;
       char *wav_file;

       /* joystick */
       char *joy_device[2];

       /* range for joystick axis values */
       int joy_dos_min;		/* must be > 0 */
       int joy_dos_max;		/* avoid setting this to > 250 */

       int joy_granularity;	/* the higher, the less sensitive - for wobbly joysticks */
       int joy_latency;		/* delay between nonblocking linux joystick reads */

       int mmio_tracing;

       int cli_timeout;		/* cli timeout hack */

        char *dos_cmd;
        char *unix_path;
        char *dos_path;

        char *unix_exec;
        char *lredir_paths;
        char *fs_backend;

        char *opl2lpt_device;
        int opl2lpt_type;

        int timer_tweaks;
        int test_mode;
} config_t;


enum { SPKR_OFF, SPKR_NATIVE, SPKR_EMULATED };
enum { CPUVM_VM86, CPUVM_KVM, CPUVM_EMU, CPUVM_NATIVE };

/*
 * Right now, dosemu only supports two serial ports.
 */
#define SIG_SER		SIGTTIN

#define IO_READ  1
#define IO_WRITE 2
#define IO_RDWR	 (IO_READ | IO_WRITE)

extern int port_readable(unsigned short);
extern int port_writeable(unsigned short);
extern unsigned char read_port(unsigned short);
extern int write_port(unsigned int, unsigned short);
extern void parent_nextscan(void);
extern void disk_close(void);
extern void cpu_setup(void);
extern void cpu_reset(void);
extern void raise_fpu_irq(void);
extern void real_run_int(int);
extern void mfs_reset(void);
extern void mfs_done(void);
extern int mfs_redirector(struct vm86_regs *regs, char *stk, int revect);
extern int mfs_fat32(void);
extern int mfs_lfn(void);
extern int int10(void);
extern int int13(void);
extern int ___int16(void);
extern int int17(void);
extern void irq_select(void);
extern int pd_receive_packet(void);
extern int printer_tick(u_long);
extern void floppy_tick(void);
#ifdef __linux__
extern void open_kmem(void);
extern void close_kmem(void);
#else
static inline void open_kmem(void) {}
static inline void close_kmem(void) {}
#endif
extern int parse_config(const char *, const char *, int);
extern void move_dosemu_local_dir(void);
extern void disk_init(void);
extern void disk_reset(void);
extern void serial_init(void);
extern void serial_reset(void);
extern void close_all_printers(void);
extern void serial_close(void);
extern void disk_close_all(void);
extern void init_all_printers(void);
extern int mfs_inte6(void);
extern int mfs_helper(struct vm86_regs *regs);
extern void pkt_helper(void);
extern short pop_word(struct vm86_regs *);
extern void __leavedos(int code, int sig, const char *s, int num);
#define leavedos(n) __leavedos(n, 0, __func__, __LINE__)
#define _leavedos_sig(s) __leavedos(0, s, __func__, __LINE__)
#define leavedos_once(n) { \
  static int __left; \
  if (!__left) { \
    __left = 1; \
    leavedos(n); \
  } \
}
extern void leavedos_from_sig(int sig);
extern void leavedos_from_thread(int code);
#define leavedos_main(n) __leavedos_main_wrp(n, 0, __func__, __LINE__)
#define _leavedos_main(n, s) __leavedos_main_wrp(n, s, __func__, __LINE__)
extern void __leavedos_main_wrp(int code, int sig, const char *s, int num);
extern void check_leavedos(void);

/*
 * DANG_BEGIN_REMARK
 * The var `fatalerr` can be given a true value at any time to have DOSEMU
 * exit on the next return from vm86 mode.
 * DANG_END_REMARK
 */
extern int fatalerr;
extern int in_leavedos;

/*
 * DANG_BEGIN_REMARK
 * The var 'running_DosC' is set by the DosC kernel and is used to handle
 * some things differently, e.g. the redirector.
 * It interfaces via INTe6,0xDC (DOS_HELPER_DOSC), but only if running_DosC
 * is !=0. At the very startup DosC issues a INTe6,0xdcDC to set running_DosC
 * with the contents of BX (which is the internal DosC version).
 * DANG_END_REMARK
 */
extern void dump_config_status(void (*printfunc)(const char *, ...));
extern void signal_pre_init(void);
extern void signal_init(void);
extern void signal_done(void);
extern void device_init(void);
extern void memory_init(void);
extern void map_video_bios(void);
extern void map_custom_bios(void);
extern void stdio_init(void);
extern void timer_interrupt_init(void);
extern void map_memory_space(void);
extern void print_version(void);
extern void keyboard_flags_init(void);
extern void video_config_init(void);
extern void video_post_init(void);
extern void video_late_init(void);
extern void video_mem_setup(void);
extern void printer_init(void);
extern void printer_mem_setup(void);
extern void video_early_close(void);
extern void video_close(void);
extern void hma_exit(void);
extern void ems_helper(void);
extern int ems_fn(struct vm86_regs *);
extern void cdrom_helper(unsigned char *, unsigned char *, unsigned int);
extern void cdrom_done(void);
extern int mscdex(void);
extern void boot(void);
extern int ipx_int7a(void);
extern void read_next_scancode_from_queue (void);
extern unsigned short detach (void);
extern void disallocate_vt (void);
extern void restore_vt (unsigned short vt);
extern void HMA_init(void);
extern void hardware_run(void);
extern int register_exit_handler(void (*handler)(void));
void tcp_helper(struct vm86_regs *);
void ipx_helper(struct vm86_regs *);

typedef struct emu_hlt_s emu_hlt_t;
extern void *vm86_hlt_state;
extern Bit16u hlt_register_handler_vm86(emu_hlt_t handler);
extern int hlt_unregister_handler_vm86(Bit16u start_addr);

extern const char *Path_cdrom[];

extern struct mempool main_pool;

#endif /* EMU_H */
