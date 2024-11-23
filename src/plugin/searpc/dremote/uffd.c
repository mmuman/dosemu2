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
/*
 * Purpose: uffd-based VGA acceleration for remote DPMI.
 *
 * Author: @stsp
 *
 * TODO: investigate UFFD_FEATURE_WP_ASYNC
 */
#include <stdio.h>
#include <fcntl.h>             /* Definition of O_* constants */
#include <sys/syscall.h>       /* Definition of SYS_* constants */
#include <linux/userfaultfd.h> /* Definition of UFFD_* constants */
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <sys/ioctl.h>
#include "vgaemu.h"
#include "utilities.h"
#include "ioselect.h"
#include "uffd.h"

static int ffds[VGAEMU_MAX_MAPPINGS];

static int uffd_create(int flags)
{
    return syscall(SYS_userfaultfd, flags);
}

static int uffd_preinit(int fd)
{
    struct uffdio_api uffdio_api;
    int err;

    uffdio_api.api = UFFD_API;
#if 0
    /* this seems broken */
    uffdio_api.features = 0;
    err = ioctl(fd, UFFDIO_API, &uffdio_api);
    if (err) {
        perror("ioctl(UFFDIO_API)");
        return err;
    }
    if (!(uffdio_api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        fprintf(stderr, "UFFD_FEATURE_PAGEFAULT_FLAG_WP not supported\n");
        return -1;
    }
    if (!(uffdio_api.features & UFFD_FEATURE_WP_HUGETLBFS_SHMEM)) {
        fprintf(stderr, "UFFD_FEATURE_WP_HUGETLBFS_SHMEM not supported\n");
        return -1;
    }
#endif
    uffdio_api.api = UFFD_API;
    uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP |
            UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
    err = ioctl(fd, UFFDIO_API, &uffdio_api);
    if (err) {
        perror("ioctl(UFFDIO_API 2)");
        return err;
    }
    return 0;
}

static void uffd_async(int fd, void *arg)
{
    struct uffd_msg msg;
    int rv, j;
    int i = (uintptr_t)arg;
    unsigned page_fault;

    rv = read(fd, &msg, sizeof(msg));
    if (rv != sizeof(msg) && errno != EAGAIN) {
        perror("read()");
        return;
    }
    page_fault = DOSADDR_REL((void *)(uintptr_t)msg.arg.pagefault.address) >> 12;

    j = page_fault - vga.mem.map[i].base_page;
    if (j >= 0 && j < vga.mem.map[i].pages) {
        int vga_page = j + vga.mem.map[i].first_page;
        vga_emu_adjust_protection(vga_page, page_fault, VGA_PROT_RW, 1, 0);
    }
}

static int do_attach_lfb(void)
{
    struct uffdio_register uffdio_register;
    int err;

    uffdio_register.mode = UFFDIO_REGISTER_MODE_WP;
    uffdio_register.range.start = (uintptr_t)MEM_BASE32(vga.mem.lfb_base);
    uffdio_register.range.len = vga.mem.size;
    err = ioctl(ffds[VGAEMU_MAP_LFB_MODE], UFFDIO_REGISTER, &uffdio_register);
    if (err) {
        perror("ioctl(UFFDIO_REGISTER lfb)");
        return err;
    }
    return 0;
}

static int do_attach_bank(void)
{
    struct uffdio_register uffdio_register;
    int err;

    uffdio_register.mode = UFFDIO_REGISTER_MODE_WP;
    uffdio_register.range.start = (uintptr_t)MEM_BASE32(vga.mem.graph_base);
    uffdio_register.range.len = vga.mem.graph_size;
    err = ioctl(ffds[VGAEMU_MAP_BANK_MODE], UFFDIO_REGISTER, &uffdio_register);
    if (err) {
        perror("ioctl(UFFDIO_REGISTER bank)");
        return err;
    }
    return 0;
}

int uffd_attach(void)
{
    int err;

    err = do_attach_lfb();
    if (err)
        return err;
    err = do_attach_bank();
    if (err)
        return err;
    return 0;
}

int uffd_open(int sock)
{
    int i, err;

    for (i = 0; i < VGAEMU_MAX_MAPPINGS; i++) {
        int err;
        int ffd = uffd_create(O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (ffd == -1) {
            perror("uffd()");
            break;
        }
        err = uffd_preinit(ffd);
        if (err)
            break;
        ffds[i] = ffd;
    }
    if (i < VGAEMU_MAX_MAPPINGS) {
        /* undo */
        while (--i >= 0)
            close(ffds[i]);
        return -1;
    }
    err = uffd_attach();
    if (err)
        return err;
    for (i = 0; i < VGAEMU_MAX_MAPPINGS; i++)
        send_fd(sock, ffds[i]);
    return 0;
}

void uffd_init(int sock)
{
    int i;

    for (i = 0; i < VGAEMU_MAX_MAPPINGS; i++) {
        ffds[i] = recv_fd(sock);
        add_to_io_select_threaded(ffds[i], uffd_async, (void *)(uintptr_t)i);
    }
}

int uffd_reattach(int sock, void *addr, size_t len)
{
    int i, j;
    unsigned page_fault = DOSADDR_REL(addr) >> 12;

    for (i = 0; i < VGAEMU_MAX_MAPPINGS; i++) {
        j = page_fault - vga.mem.map[i].base_page;
        if (j >= 0 && j < vga.mem.map[i].pages) {
            switch (i) {
            case VGAEMU_MAP_BANK_MODE:
                do_attach_bank();
                break;
            case VGAEMU_MAP_LFB_MODE:
                do_attach_lfb();
                break;
            }
        }
    }
    return 0;
}

int uffd_wp(int idx, void *addr, size_t len, int wp)
{
    int err;
    struct uffdio_writeprotect wpdata;

    wpdata.range.start = (uintptr_t)addr;
    wpdata.range.len = len;
    wpdata.mode = (wp ? UFFDIO_WRITEPROTECT_MODE_WP : 0);
    assert(idx < VGAEMU_MAX_MAPPINGS);
    err = ioctl(ffds[idx], UFFDIO_WRITEPROTECT, &wpdata);
    if (err)
        perror("ioctl()");
    return err;
}
