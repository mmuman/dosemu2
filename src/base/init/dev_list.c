/*
 * DANG_BEGIN_MODULE
 *
 * REMARK
 * Description:
 *  Manages a list of the available I/O devices.  It will automatically
 *  call their initialization and termination routines.
 *  The current I/O device list includes:
 * VERB
 *     Fully emulated:      pit, pic, cmos, serial
 *     Partially emulated:  rtc, keyb, lpt
 *     Unemulated:          dma, hdisk, floppy, pos
 * /VERB
 * /REMARK
 *
 * Maintainers: Scott Buchholz
 *
 * DANG_END_MODULE
 *
 */

#include <string.h>
#include <stdlib.h>

#include "emu.h"
#include "iodev.h"
#include "int.h"
#include "port.h"
#include "pic.h"
#include "chipset.h"
#include "serial.h"
#include "mouse.h"
#include "lpt.h"
#include "disks.h"
#include "dma.h"
#include "dosemu_debug.h"
#include "pktdrvr.h"
#include "ne2000.h"
#include "ipx.h"
#include "sound.h"
#include "joystick.h"
#include "emm.h"
#include "xms.h"
#include "emudpmi.h"
#include "virq.h"
#include "vint.h"
#include "vtmr.h"
#include "dos2linux.h"

struct io_dev_struct {
  const char *name;
  void (* init_func)(void);
  void (* reset_func)(void);
  void (* term_func)(void);
};
#define MAX_IO_DEVICES 30

#define MAX_DEVICES_OWNED 50
struct owned_devices_struct {
  const char *dev_names[MAX_DEVICES_OWNED];
  int devs_owned;
} owned_devices[MAX_IO_DEVICES];

static int current_device = -1;

static struct io_dev_struct io_devices[MAX_IO_DEVICES] = {
  { "cmos",    cmos_init,    cmos_reset,    NULL },
  { "serial",  serial_init,  serial_reset,  serial_close },
  { "pic",     pic_init,     pic_reset,     NULL },
  { "chipset", chipset_init, NULL,          NULL },
  { "virq",    virq_init,    virq_reset,    NULL },
  { "vint",    vint_init,    NULL,          NULL },
  { "vtmr",    vtmr_init,    vtmr_reset,    vtmr_done },
  { "pit",     pit_init,     pit_reset,     pit_done },
  { "rtc",     rtc_init,     NULL,          NULL },
  { "lpt",     printer_init, NULL,	    NULL },
  { "dma",     dma_init,     dma_reset,     NULL },
#if 0
  { "floppy",  floppy_init,  floppy_reset,  NULL },
  { "hdisk",   hdisk_init,   hdisk_reset,   NULL },
#endif
  { "disks",   disk_init,    disk_reset,    NULL },
  { "video",   video_post_init, NULL, NULL },
  { "sound",   sound_init,   sound_reset,   sound_done },
  { "mt32",    mt32_init,    mt32_reset,    mt32_done },
  { "joystick", joy_init,    joy_reset,     joy_term },
#ifdef IPX
  { "ipx",      ipx_init,    NULL,          ipx_close },
#endif
  { "packet driver", pkt_init, pkt_reset,   pkt_term },
  { "tcp driver", tcp_init,  tcp_reset,     tcp_done },
  { "ne2000",  ne2000_init,  ne2000_reset,  ne2000_done },
  { "ems",     ems_init,     ems_reset,     NULL },
  { "xms",     xms_init,     xms_reset,     xms_done },
  { "dpmi",    dpmi_setup,   dpmi_reset,    NULL },
  { "mfs",     NULL,         mfs_reset,     mfs_done },
  { "cdrom",   NULL,         NULL,          cdrom_done },
  { "internal_mouse",  dosemu_mouse_init,   dosemu_mouse_reset, dosemu_mouse_close },
  { NULL,      NULL,         NULL,          NULL }
};


void iodev_init(void)        /* called at startup */
{
  int i;

  for (i = 0; i < MAX_IO_DEVICES; i++)
    if (io_devices[i].init_func) {
      current_device = i;
      io_devices[i].init_func();
    }

  current_device = -1;
}

void iodev_reset(void)        /* called at reboot */
{
  struct io_dev_struct *ptr;

  for (ptr = io_devices; ptr < &io_devices[MAX_IO_DEVICES]; ptr++)
    if (ptr->reset_func)
      ptr->reset_func();
}

void iodev_term(void)
{
  struct io_dev_struct *ptr;

  for (ptr = io_devices; ptr < &io_devices[MAX_IO_DEVICES]; ptr++)
    if (ptr->term_func)
      ptr->term_func();
}

void iodev_register(const char *name,
	void (*init_func)(void),
	void (*reset_func)(void),
	void (*term_func)(void))
{
	struct io_dev_struct *ptr;
	for(ptr = io_devices; ptr < &io_devices[MAX_IO_DEVICES -1]; ptr++) {
		if (ptr->name) {
			if (strcmp(ptr->name, name) == 0) {
				g_printf("IODEV: %s already registered\n",
					name);
				return;
			}
			continue;
		}
		ptr->name = name;
		ptr->init_func = init_func;
		ptr->reset_func = reset_func;
		ptr->term_func = term_func;
		return;
	}
	g_printf("IODEV: Could not find free slot for %s\n",
		name);
	return;
}

void iodev_unregister(const char *name)
{
	struct io_dev_struct *ptr;
	for(ptr = io_devices; ptr < &io_devices[MAX_IO_DEVICES -1]; ptr++) {
		if (!ptr->name || (strcmp(ptr->name, name) != 0)) {
			continue;
		}
		ptr->name = 0;
		ptr->init_func = 0;
		ptr->reset_func = 0;
		ptr->term_func = 0;
	}
}

static int find_device_owner(const char *dev_name)
{
	int i, j;
	for(i = 0; i < MAX_IO_DEVICES - 1; i++) {
	    for(j = 0; j < owned_devices[i].devs_owned; j++)
		if (strcmp(dev_name, owned_devices[i].dev_names[j]) == 0)
		    return i;
	}
	return -1;
}

void iodev_add_device(const char *dev_name)
{
	int dev_own;
	if (current_device == -1) {
	    error("add_device() is called not during the init stage!\n");
	    leavedos(10);
	}
	dev_own = find_device_owner(dev_name);
	if (dev_own != -1) {
	    error("Device conflict: Attempt to use %s for %s and %s\n",
		dev_name, io_devices[dev_own].name, io_devices[current_device].name);
	    config.exitearly = 1;
	}
	if (owned_devices[current_device].devs_owned >= MAX_DEVICES_OWNED) {
	    error("No free slot for device %s\n", dev_name);
	    config.exitearly = 1;
	}
	c_printf("registering %s for %s\n",dev_name,io_devices[current_device].name);
	owned_devices[current_device].dev_names[owned_devices[current_device].devs_owned++] = dev_name;
}
