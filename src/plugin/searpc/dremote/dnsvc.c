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
#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>
#include <pthread.h>
#include <searpc.h>
#include "plugin_config.h"
#include "utilities.h"
#include "mapping.h"
#include "init.h"
#include "emu.h"
#include "util.h"
#include "dnrpcdefs.h"

static SearpcClient *clnt;
static int sock_tx;
static int exited;
static pthread_mutex_t rpc_mtx = PTHREAD_MUTEX_INITIALIZER;

static int remote_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset);

static void svc_ex(void *arg)
{
    error("fssvc failed, exiting\n");
    exited++;
    leavedos(35);
}

static int dnsrv_init(const char *svc_name, int fd, void *arg)
{
    return dnrpc_srv_init(svc_name, fd);
}

static void bad_rpc(const char *func, const char *msg)
{
    fprintf(stderr, "RPC failure: %s: %s\n", func, msg);
    if (!exited) {
        exited++;
        leavedos(5);
    }
}

static int remote_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset)
{
    int ret;
    GError *error = NULL;
    send_fd(sock_tx, fd);
    pthread_mutex_lock(&rpc_mtx);
    ret = searpc_client_call__int(clnt, "mmap_1",
                                  &error, 5,
                                  "int64", &addr,
                                  "int64", &length, "int", prot,
                                  "int", flags, "int64", &offset);
    pthread_mutex_unlock(&rpc_mtx);
    CHECK_RPC(error);
    return ret;
}

static int remote_mprotect(void *addr, size_t length, int prot)
{
    int ret;
    GError *error = NULL;

    if (!clnt)
        return 0;
    pthread_mutex_lock(&rpc_mtx);
    ret = searpc_client_call__int(clnt, "mprotect_1",
                                  &error, 3,
                                  "int64", &addr,
                                  "int64", &length, "int", prot);
    pthread_mutex_unlock(&rpc_mtx);
    CHECK_RPC(error);
    return ret;
}

static int remote_madvise(void *addr, size_t length, int flags)
{
    int ret;
    GError *error = NULL;

    if (!clnt)
        return 0;
    pthread_mutex_lock(&rpc_mtx);
    ret = searpc_client_call__int(clnt, "madvise_1",
                                  &error, 3,
                                  "int64", &addr,
                                  "int64", &length, "int", flags);
    pthread_mutex_unlock(&rpc_mtx);
    CHECK_RPC(error);
    return ret;
}

static const struct mapping_hook mhook = {
    .mmap = remote_mmap,
    .mprotect = remote_mprotect,
    .madvise = remote_madvise,
};

CONSTRUCTOR(static void dnsvc_init(void))
{
    clnt = clnt_init(&sock_tx, dnsrv_init, NULL, NULL, svc_ex, "dnrpc");
    if (!clnt) {
        fprintf(stderr, "failure registering RPC\n");
        return;
    }
    mapping_register_hook(&mhook);
}
