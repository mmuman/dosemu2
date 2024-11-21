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
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <searpc-server.h>
#include <searpc-utils.h>
#include "utilities.h"
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


int dnrpc_srv_init(const char *svc_name, int fd)
{
    sock_rx = fd;
    searpc_server_init(register_marshals);
    searpc_create_service(svc_name);

    searpc_server_register_function(svc_name, mmap_1_svc, "mmap_1",
            searpc_signature_int__int64_int64_int_int_int64());
    searpc_server_register_function(svc_name, mprotect_1_svc, "mprotect_1",
            searpc_signature_int__int64_int64_int());
    searpc_server_register_function(svc_name, madvise_1_svc, "madvise_1",
            searpc_signature_int__int64_int64_int());
    return 0;
}
