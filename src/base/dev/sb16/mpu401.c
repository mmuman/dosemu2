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
 * Purpose: MPU401 emulation.
 *
 * Author: @stsp
 *
 */
#include <stdlib.h>
#include <assert.h>
#include "ringbuf.h"
#include "port.h"
#include "dosemu_debug.h"
#include "sound/midi.h"
#include "mpu401.h"

struct mpu401_s {
#define MIDI_FIFO_SIZE 32
  struct rng_s fifo_in;
  struct rng_s fifo_out;
  int uart:1;
  ioport_t base;
  struct mpu401_ops *ops;
};

static void mpu401_stop_midi(struct mpu401_s *mpu)
{
    midi_stop();
}

static void do_write_midi(struct mpu401_s *mpu, Bit8u value)
{
    rng_put(&mpu->fifo_out, &value);

    mpu401_process(mpu);
}

static int midi_output_empty(struct mpu401_s *mpu)
{
    return !rng_count(&mpu->fifo_out);
}

static Bit8u get_midi_data(struct mpu401_s *mpu)
{
    Bit8u val;
    int ret = rng_get(&mpu->fifo_out, &val);
    assert(ret == 1);
    return val;
}

static Bit8u get_midi_in_byte(struct mpu401_s *mpu)
{
    Bit8u val;
    int ret = rng_get(&mpu->fifo_in, &val);
    assert(ret == 1);
    return val;
}

static void put_midi_in_byte(struct mpu401_s *mpu, Bit8u val)
{
    rng_put_const(&mpu->fifo_in, val);
}

static int get_midi_in_fillup(struct mpu401_s *mpu)
{
    return rng_count(&mpu->fifo_in);
}

static void clear_midi_in_fifo(struct mpu401_s *mpu)
{
    rng_clear(&mpu->fifo_in);
}

void mpu401_process(struct mpu401_s *mpu)
{
    Bit8u data;

    /* no timing for now */
    while (!midi_output_empty(mpu)) {
	data = get_midi_data(mpu);
	midi_write(data);
    }

    while (midi_get_data_byte(&data))
	put_midi_in_byte(mpu, data);
#define MPU401_IN_FIFO_TRIGGER 1
     if (mpu->uart) {
	if (get_midi_in_fillup(mpu) == MPU401_IN_FIFO_TRIGGER)
	    mpu->ops->activate_irq();
    }
}

static Bit8u mpu401_io_read(ioport_t port, void *arg)
{
    struct mpu401_s *mpu = arg;
    ioport_t addr;
    Bit8u r = 0xff;

    addr = port - mpu->base;

    switch (addr) {
    case 0:
	/* Read data port */
	if (get_midi_in_fillup(mpu))
	    r = get_midi_in_byte(mpu);
	else
	    S_printf("MPU401: ERROR: No data to read\n");
	S_printf("MPU401: Read data port = 0x%02x, %i bytes still in queue\n",
	     r, get_midi_in_fillup(mpu));
	if (!get_midi_in_fillup(mpu))
	    mpu->ops->deactivate_irq();
	mpu->ops->run_irq();
	break;
    case 1:
	/* Read status port */
	/* 0x40=OUTPUT_AVAIL; 0x80=INPUT_AVAIL */
	r = 0xff & (~0x40);	/* Output is always possible */
	if (get_midi_in_fillup(mpu))
	    r &= (~0x80);
	S_printf("MPU401: Read status port = 0x%02x\n", r);
	break;
    }
    return r;
}

static void mpu401_io_write(ioport_t port, Bit8u value, void *arg)
{
    struct mpu401_s *mpu = arg;
    uint32_t addr;
    addr = port - mpu->base;

    switch (addr) {
    case 0:
	/* Write data port */
	if (debug_level('S') > 5)
		S_printf("MPU401: Write 0x%02x to data port\n", value);
	do_write_midi(mpu, value);
	if (!mpu->uart && debug_level('S') > 5)
		S_printf("MPU401: intelligent mode write unhandled\n");
	break;
    case 1:
	/* Write command port */
	S_printf("MPU401: Write 0x%02x to command port\n", value);
	clear_midi_in_fifo(mpu);
	/* the following doc:
	 * http://www.piclist.com/techref/io/serial/midi/mpu.html
	 * says 3f does not need ACK. But dosbox sources say that
	 * it does. Someone please try on a real HW? */
	put_midi_in_byte(mpu, 0xfe);	/* A command is sent: MPU_ACK it next time */
	switch (value) {
	case 0x3f:		// 0x3F = UART mode
	    mpu->uart = 1;
	    break;
	case 0xff:		// 0xFF = reset MPU
	    mpu->uart = 0;
	    mpu401_stop_midi(mpu);
	    break;
	case 0x80:		// Clock ??
	    break;
	case 0xac:		// Query version
	    put_midi_in_byte(mpu, 0x15);
	    break;
	case 0xad:		// Query revision
	    put_midi_in_byte(mpu, 0x1);
	    break;
	}
	mpu->ops->activate_irq();
	break;
    }
}

struct mpu401_s *mpu401_init(ioport_t base, struct mpu401_ops *ops)
{
    emu_iodev_t io_device;
    struct mpu401_s *mpu;

    S_printf("MPU401: MPU-401 Initialisation\n");

    mpu = malloc(sizeof(*mpu));
    assert(mpu);

    /* This is the MPU-401 */
    io_device.read_portb = mpu401_io_read;
    io_device.write_portb = mpu401_io_write;
    io_device.read_portw = NULL;
    io_device.write_portw = NULL;
    io_device.read_portd = NULL;
    io_device.write_portd = NULL;
    io_device.handler_name = "Midi Emulation";
    io_device.start_addr = base;
    io_device.end_addr = base + 0x001;
    io_device.arg = mpu;
    if (port_register_handler(io_device, 0) != 0)
	error("MPU-401: Cannot registering port handler\n");

    S_printf("MPU401: MPU-401 Initialisation - Base 0x%03x \n", base);

    rng_init(&mpu->fifo_in, MIDI_FIFO_SIZE, 1);
    rng_init(&mpu->fifo_out, MIDI_FIFO_SIZE, 1);
    mpu->base = base;
    mpu->ops = ops;
    return mpu;
}

void mpu401_reset(struct mpu401_s *mpu)
{
    mpu->uart = 0;
}

void mpu401_done(struct mpu401_s *mpu)
{
    rng_destroy(&mpu->fifo_in);
    rng_destroy(&mpu->fifo_out);
    free(mpu);
}

int mpu401_is_uart(struct mpu401_s *mpu)
{
    return mpu->uart;
}