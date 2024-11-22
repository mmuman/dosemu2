/*
 *  Copyright (C) 2024  stsp
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <utime.h>
#include <assert.h>
#include <alloca.h>
#include <errno.h>
#include <sys/mman.h>
#include <searpc-server.h>
#include <searpc-utils.h>
#include "cpu.h"
#include "dnative.h"
#include "plugin_config.h"
#include "utilities.h"
#include "emu.h"
#include "searpc-signature.h"
#include "searpc-marshal.h"
#include "dnrpcdefs.h"

static int sock_rx;

static int mmap_1_svc(uint64_t addr, uint64_t length, int prot, int flags,
        uint64_t offset)
{
    int fd = recv_fd(sock_rx);
    void *ret;

    if (fd == -1)
        return -1;
    ret = mmap((void *)(uintptr_t)addr, length, prot, flags, fd, offset);
    if (ret == MAP_FAILED)
        perror("mmap()");
    close(fd);
    return (ret == MAP_FAILED ? -1 : 0);
}

static int mprotect_1_svc(uint64_t addr, uint64_t length, int prot)
{
    return mprotect((void *)(uintptr_t)addr, length, prot);
}

static int madvise_1_svc(uint64_t addr, uint64_t length, int flags)
{
    return madvise((void *)(uintptr_t)addr, length, flags);
}

static int setup_1_svc(void)
{
    return dnops->setup();
}

static int done_1_svc(void)
{
    dnops->done();
    return 0;
}

static int control_1_svc(void)
{
    cpuctx_t scp;
    int ret;

    recv_state(&scp);
    ret = dnops->control(&scp);
    send_state(&scp);
    return ret;
}

static int exit_1_svc(void)
{
    cpuctx_t scp;
    int ret;

    recv_state(&scp);
    ret = dnops->exit(&scp);
    send_state(&scp);
    return ret;
}

static int read_ldt_1_svc(int bytecount)
{
    return dnops->read_ldt(rpc_shared_page, bytecount);
}

static int write_ldt_1_svc(int bytecount)
{
    return dnops->write_ldt(rpc_shared_page, bytecount);
}

static int check_verr_1_svc(int selector)
{
    return dnops->check_verr(selector);
}

static int debug_breakpoint_1_svc(int op, int err)
{
    cpuctx_t scp;
    int ret;

    recv_state(&scp);
    ret = dnops->debug_breakpoint(op, &scp, err);
    send_state(&scp);
    return ret;
}


int dnrpc_srv_init(const char *svc_name, int fd)
{
    void *plu;
    sock_rx = fd;
    searpc_server_init(register_marshals);
    searpc_create_service(svc_name);

    searpc_server_register_function(svc_name, mmap_1_svc, "mmap_1",
            searpc_signature_int__int64_int64_int_int_int64());
    searpc_server_register_function(svc_name, mprotect_1_svc, "mprotect_1",
            searpc_signature_int__int64_int64_int());
    searpc_server_register_function(svc_name, madvise_1_svc, "madvise_1",
            searpc_signature_int__int64_int64_int());

    searpc_server_register_function(svc_name, setup_1_svc, "setup_1",
            searpc_signature_int__void());
    searpc_server_register_function(svc_name, done_1_svc, "done_1",
            searpc_signature_int__void());
    searpc_server_register_function(svc_name, control_1_svc, "control_1",
            searpc_signature_int__void());
    searpc_server_register_function(svc_name, exit_1_svc, "exit_1",
            searpc_signature_int__void());
    searpc_server_register_function(svc_name, read_ldt_1_svc, "read_ldt_1",
            searpc_signature_int__int());
    searpc_server_register_function(svc_name, write_ldt_1_svc, "write_ldt_1",
            searpc_signature_int__int());
    searpc_server_register_function(svc_name, check_verr_1_svc, "check_verr_1",
            searpc_signature_int__int());
    searpc_server_register_function(svc_name, debug_breakpoint_1_svc,
            "debug_breakpoint_1",
            searpc_signature_int__int_int());

    signal_init();
    dnops = NULL;
    plu = load_plugin("dnative");
    return (plu ? 0 : -1);
}
