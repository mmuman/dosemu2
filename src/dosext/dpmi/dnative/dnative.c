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
#include "cpu.h"
#include "dosemu_debug.h"
#include "plugin_config.h"
#include "utilities.h"
#include "dnative.h"

static const struct dnative_ops *dnops;

int native_dpmi_setup(void)
{
#ifdef DNATIVE
    if (!dnops)
        load_plugin("dnative");
#endif
    if (!dnops) {
        error("Native DPMI not compiled in\n");
        return -1;
    }
    return dnops->setup();
}

void native_dpmi_done(void)
{
    dnops->done();
}

int native_dpmi_control(cpuctx_t *scp)
{
    return dnops->control(scp);
}

int native_dpmi_exit(cpuctx_t *scp)
{
    return dnops->exit(scp);
}

void native_dpmi_enter(void)
{
    dnops->enter();
}

void native_dpmi_leave(void)
{
    dnops->leave();
}

int native_modify_ldt(int func, void *ptr, unsigned long bytecount)
{
    return dnops->modify_ldt(func, ptr, bytecount);
}

int native_check_verr(unsigned short selector)
{
    return dnops->check_verr(selector);
}

int native_debug_breakpoint(int op, cpuctx_t *scp, int err)
{
    return dnops->debug_breakpoint(op, scp, err);
}

int register_dnative_ops(const struct dnative_ops *ops)
{
    if (dnops)
        return -1;
    dnops = ops;
    return 0;
}
