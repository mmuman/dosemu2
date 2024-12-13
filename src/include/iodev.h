#ifndef _EMU_IODEV_H
#define _EMU_IODEV_H

#include <sys/time.h>
#include "port.h"
#include "timers.h"

#include "lpt.h"
#include "pic.h"

/*******************************************************************
 * Global initialization routines                                  *
 *******************************************************************/

extern void iodev_init(void);
extern void iodev_reset(void);
extern void iodev_term(void);
extern void iodev_register(const char *name, void (*init_func)(void),
	void (*reset_func)(void), void (*term_func)(void));
void iodev_unregister(const char *name);
void iodev_add_device(const char *dev_name);

extern void  pit_init(void);
extern void  pit_done(void);
extern void  pit_reset(void);

/*******************************************************************
 * Real Time Clock (RTC) chip                                      *
 *******************************************************************/

#define RTC_SEC       0
#define RTC_SECALRM   1
#define RTC_MIN       2
#define RTC_MINALRM   3
#define RTC_HOUR      4
#define RTC_HOURALRM  5
#define RTC_DOW       6
#define RTC_DOM       7
#define RTC_MON       8
#define RTC_YEAR      9
#define RTC_REGA      0xa
#define RTC_REGB      0xb
#define RTC_REGC      0xc
#define RTC_REGD      0xd

#define SECS_PER_MIN     60
#define SECS_PER_HOUR    3600
#define SECS_PER_DAY     86400
#define SECS_PER_MONTH   2592000     /* yes, I know, it's not right */
#define SECS_PER_YEAR    31557600    /* any trivia buffs? */

extern void  rtc_init(void);
extern void  rtc_setup(void);
extern void  rtc_reset(void);
extern void  rtc_update(void);

/*******************************************************************
 * CMOS support                                                    *
 *******************************************************************/

#include "cmos.h"

extern unsigned long last_ticks;		/* for int1a */
extern long  sys_base_ticks;		/* signed, could go negative */
extern long  usr_delta_ticks;

/*******************************************************************
 * For centralised DOS/CMOS time from gettimeofday()               *
 *******************************************************************/

/* For int.c int1a(void) configuring. */
#define TM_BIOS     0   /* INT-1A AH=0 returns BIOS count of ticks (INT-8), DOS & RTC can be set. */
#define TM_PIT      1   /* INT-1A AH=0 returns PIC timer based 'ticks', can't set RTC */
#define TM_LINUX    2   /* INT-1A AH=0 returns LINUX time always, cant set DOS/RTC time */

unsigned long get_linux_ticks(int set_cmos, int *day_rollover);

/*******************************************************************
 * Dummy hardware support stubs                                    *
 *******************************************************************/

extern void  pos_init(void);
extern void  pos_reset(void);

extern void  hdisk_init(void);
extern void  hdisk_reset(void);

extern void  floppy_init(void);
extern void  floppy_reset(void);

void mt32_init(void);
void mt32_reset(void);
void mt32_done(void);

void emutcp_init(void);
void emutcp_reset(void);
void emutcp_done(void);

#endif /* _EMU_IODEV_H */
