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
#include <sys/socket.h>
#include <searpc-server.h>
#include <searpc-utils.h>
#include "utilities.h"
#include "searpc-signature.h"
#include "searpc-marshal.h"
#include "test-object.h"
#include "fsrpcdefs.h"
#include "fssvc.h"

#define MAX_PATHS 50
static char *paths[MAX_PATHS];
static int num_paths;
static char *paths_ex[MAX_PATHS];
static int num_paths_ex;
static int sealed;
static int transp_fd;
static int sock_tx;
static plist_idx_t plist_idx_cb;
static setattr_t setattr_cb;
static getattr_t getattr_cb;
static char *plist;
static int exiting;

#define ASSERT0(x) do {if (!(x)) { return -1; }} while(0)

static int add_path_1_svc(const char *path)
{
    int len;

    ASSERT0(num_paths < MAX_PATHS);
    ASSERT0(!sealed);
    len = strlen(path);
    ASSERT0(len > 0);
    if (path[len - 1] == '/') {
        paths[num_paths] = strdup(path);
    } else {
        char *new_path = malloc(len + 2);
        memcpy(new_path, path, len + 1);
        new_path[len] = '/';
        new_path[len + 1] = '\0';
        paths[num_paths] = new_path;
    }
    return num_paths++;
}

static int add_path_ex_1_svc(const char *path)
{
    int len;

    ASSERT0(num_paths_ex < MAX_PATHS);
    ASSERT0(!sealed);
    len = strlen(path);
    ASSERT0(len > 0);
    paths_ex[num_paths_ex++] = strdup(path);
    return 0;
}

static int add_path_list_1_svc(const char *clist)
{
    ASSERT0(!sealed);
    plist = strdup(clist);
    return 0;
}

static int seal_1_svc(void)
{
    ASSERT0(!sealed);
    sealed = 1;
    return num_paths;
}

#define CHK(x) do { if (!(x)) return FALSE; } while(0)

static int path_ok(int idx, const char *path)
{
    int len;

    CHK(sealed);
    if (idx < 0) {
        int i;
        for (i = 0; i < num_paths_ex; i++) {
            if (strcmp(path, paths_ex[i]) == 0)
                return TRUE;
        }
        return FALSE;
    }
    if (idx >= num_paths)
        return (plist && plist_idx_cb(plist, path) + num_paths == idx);
    len = strlen(paths[idx]);
    assert(len && paths[idx][len - 1] == '/');
    if (strlen(path) == len - 1)
        len--;  // no trailing slash
    CHK(strncmp(path, paths[idx], len) == 0);
    return TRUE;
}

#define ASSERT_E(x, e, c) do { \
  if (!(x)) { \
    ret->ret = -1; \
    ret->args_err = (e); \
    ret->errn = 0; \
    c; \
    return G_OBJECT(ret); \
  } \
} while (0)

#define ASSERT_P(x) ASSERT_E(x, EPERM, \
  fprintf(stderr, "reject %s %i\n", path, idx))
#define ASSERT_P2(x) ASSERT_E(x, EPERM,)
#define ASSERT_I(x) ASSERT_E(x, EINVAL,)

#define ASSERT_C(x) do { \
  if (!(x)) { \
    ret->ret = -1; \
    ret->args_err = 0; \
    ret->errn = errno; \
    return G_OBJECT(ret); \
  } \
} while (0)

#define CALL(x) do { \
  ret->ret = (x); \
  ret->args_err = 0; \
  ret->errn = errno; \
} while (0)

static GObject* open_1_svc(int idx, char *path, int flags)
{
    int fd;
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    fd = open(path, flags);
    ASSERT_C(fd >= 0);
    CALL(send_fd(sock_tx, fd));
    close(fd);
    return G_OBJECT(ret);
}

static GObject* creat_1_svc(int idx, char *path, int flags, int mode)
{
    int fd;
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    ASSERT_I(flags & O_CREAT);
    fd = open(path, flags, mode);
    ASSERT_C(fd >= 0);
    CALL(send_fd(sock_tx, fd));
    close(fd);
    return G_OBJECT(ret);
}

static GObject* unlink_1_svc(int idx, char *path)
{
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    CALL(unlink(path));
    return G_OBJECT(ret);
}

static GObject* setxattr_1_svc(int idx, char *path, int attr)
{
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    CALL(setattr_cb(path, attr));
    return G_OBJECT(ret);
}

static GObject* getxattr_1_svc(int idx, char *path)
{
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    CALL(getattr_cb(path));
    return G_OBJECT(ret);
}

static GObject* rename_1_svc(int idx1, char *oldpath, int idx2, char *newpath)
{
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P2(path_ok(idx1, oldpath));
    ASSERT_P2(path_ok(idx2, newpath));
    CALL(rename(oldpath, newpath));
    return G_OBJECT(ret);
}

static GObject* mkdir_1_svc(int idx, char *path, int mode)
{
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    CALL(mkdir(path, mode));
    return G_OBJECT(ret);
}

static GObject* rmdir_1_svc(int idx, char *path)
{
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    CALL(rmdir(path));
    return G_OBJECT(ret);
}

static GObject* utime_1_svc(int idx, char *path, uint64_t atime,
        uint64_t mtime)
{
    struct utimbuf ut = { .actime = atime, .modtime = mtime };
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    ASSERT_P(path_ok(idx, path));
    CALL(utime(path, &ut));
    return G_OBJECT(ret);
}

static int path_ok_1_svc(int idx, char *path)
{
    return path_ok(idx, path);
}

static int exit_1_svc(void)
{
    int i;

    for (i = 0; i < num_paths; i++)
        free(paths[i]);
    free(plist);
    exiting++;
    return 0;
}

static GObject* shm_open_1_svc(const char *name, int oflag, int mode)
{
    int fd;
    TestObject *ret = g_object_new (TEST_OBJECT_TYPE, NULL);
    fd = shm_open(name, oflag, mode);
    ASSERT_C(fd >= 0);
    CALL(send_fd(sock_tx, fd));
    close(fd);
    return G_OBJECT(ret);
}

static int shm_unlink_1_svc(const char *name)
{
    return shm_unlink(name);
}

static const char *svc_name = "fsrpc";

int fsrpc_srv_init(int tr_fd, int fd, plist_idx_t pi, setattr_t sa,
    getattr_t ga)
{
    transp_fd = tr_fd;
    sock_tx = fd;
    plist_idx_cb = pi;
    setattr_cb = sa;
    getattr_cb = ga;
    searpc_server_init(register_marshals);
    searpc_create_service(svc_name);

    searpc_server_register_function(svc_name, add_path_1_svc, "add_path_1",
            searpc_signature_int__string());
    searpc_server_register_function(svc_name, add_path_ex_1_svc, "add_path_ex_1",
            searpc_signature_int__string());
    searpc_server_register_function(svc_name, add_path_list_1_svc,
            "add_path_list_1",
            searpc_signature_int__string());
    searpc_server_register_function(svc_name, seal_1_svc, "seal_1",
            searpc_signature_int__void());
    searpc_server_register_function(svc_name, open_1_svc, "open_1",
            searpc_signature_object__int_string_int());
    searpc_server_register_function(svc_name, creat_1_svc, "creat_1",
            searpc_signature_object__int_string_int_int());
    searpc_server_register_function(svc_name, unlink_1_svc, "unlink_1",
            searpc_signature_object__int_string());
    searpc_server_register_function(svc_name, setxattr_1_svc, "setxattr_1",
            searpc_signature_object__int_string_int());
    searpc_server_register_function(svc_name, getxattr_1_svc, "getxattr_1",
            searpc_signature_object__int_string());
    searpc_server_register_function(svc_name, rename_1_svc, "rename_1",
            searpc_signature_object__int_string_int_string());
    searpc_server_register_function(svc_name, mkdir_1_svc, "mkdir_1",
            searpc_signature_object__int_string_int());
    searpc_server_register_function(svc_name, rmdir_1_svc, "rmdir_1",
            searpc_signature_object__int_string());
    searpc_server_register_function(svc_name, utime_1_svc, "utime_1",
            searpc_signature_object__int_string_int64_int64());
    searpc_server_register_function(svc_name, path_ok_1_svc, "path_ok_1",
            searpc_signature_int__int_string());
    searpc_server_register_function(svc_name, exit_1_svc, "exit_1",
            searpc_signature_int__void());
    searpc_server_register_function(svc_name, shm_open_1_svc, "shm_open_1",
            searpc_signature_int__string_int_int());
    searpc_server_register_function(svc_name, shm_unlink_1_svc, "shm_unlink_1",
            searpc_signature_int__string());
    return 0;
}

void fsrpc_svc_run(void)
{
    while (1) {
        char buf[4096];
        gchar *json;
        gsize len = 0;
        ssize_t rd = recv(transp_fd, buf, sizeof(buf), 0);
        if (rd <= 0)
            exit(0);
        json = searpc_server_call_function(svc_name, buf, rd, &len);
        if (!len)
            exit(0);
        rd = send(transp_fd, json, len, 0);
        free(json);
        if (rd <= 0)
            exit(0);
        if (exiting)
            exit(0);
    }
}
