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
 * Purpose: flipping priviledges
 *
 * Authors:
 *   Loosely based on dosemu1's priv.c by Hans Lermen, Eric Biederman
 *   and Bart Oldeman.
 *
 *   Rewritten for dosemu2 by @stsp
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif
#include <limits.h>
#ifdef HAVE_SYS_IO_H
#include <sys/io.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "emu.h"
#include "priv.h"
#include "dosemu_config.h"
#include "utilities.h"
#ifdef X86_EMULATOR
#include "cpu-emu.h"
#endif

/* Some handy information to have around */
static uid_t uid,euid;
static gid_t gid,egid;
static uid_t cur_euid;
static gid_t cur_egid;
static int suid, sgid;

static int skip_priv_setting;
static int groups_dropped;

int can_do_root_stuff;
int under_root_login;
int using_sudo;
int current_iopl;

#define PRIVS_ARE_ON (euid == cur_euid)
#define PRIVS_ARE_OFF (uid == cur_euid)

static int _priv_on(void)
{
  if (seteuid(euid)) {
    error("Cannot turn privs on!\n");
    return 0;
  }
  cur_euid = euid;
  if (setegid(egid)) {
    error("Cannot turn privs on!\n");
    return 0;
  }
  cur_egid = egid;
  return 1;
}

static int _priv_off(void)
{
  if (seteuid(uid)) {
    error("Cannot turn privs off!\n");
    return 0;
  }
  cur_euid = uid;
  if (setegid(gid)) {
    error("Cannot turn privs off!\n");
    return 0;
  }
  cur_egid = gid;
  return 1;
}

int real_enter_priv_on(void)
{
  if (skip_priv_setting) return 1;
  assert(PRIVS_ARE_OFF);
  return _priv_on();
}

int real_leave_priv_setting(void)
{
  if (skip_priv_setting) return 1;
  assert(PRIVS_ARE_ON);
  return _priv_off();
}

int priv_iopl(int pl)
{
#ifdef HAVE_SYS_IO_H
  int ret;
  assert(PRIVS_ARE_OFF);
  _priv_on();
  ret = iopl(pl);
  _priv_off();
#ifdef X86_EMULATOR
  if (config.cpu_vm == CPUVM_EMU) e_priv_iopl(pl);
#endif
  if (ret == 0)
    current_iopl = pl;
  return ret;
#else
  return -1;
#endif
}

uid_t get_orig_uid(void)
{
  return uid;
}

gid_t get_orig_gid(void)
{
  return gid;
}

uid_t get_suid(void)
{
  assert(suid);
  return euid;
}

gid_t get_sgid(void)
{
  assert(sgid);
  return egid;
}

static int do_drop(void)
{
  /* We set the same values as they are now.
   * The trick is that if the first arg != -1 then saved-euid is reset.
   * This allows to avoid the use of non-standard setresuid(). */
  if (setreuid(uid, uid)) {
    error("Cannot drop uid!\n");
    return -1;
  }
  if (!groups_dropped && setregid(gid, gid)) {
    error("Cannot drop gid!\n");
    return -1;
  }
  /* Now check that saved-euids are actually reset: privs should fail. */
  if (seteuid(euid) == 0) {
    error("privs were not dropped\n");
    return -1;
  }
  return 0;
}

void priv_drop_root(void)
{
  int err;

  if (skip_priv_setting)
    return;
  assert(PRIVS_ARE_OFF);
  err = do_drop();
  if (err) {
    leavedos(3);
    return;
  }
  skip_priv_setting = 1;
  if (uid) can_do_root_stuff = 0;
}

static int caps_present(void)
{
#ifdef HAVE_LIBCAP
    int rc;
    cap_t cap, ecap;

    cap = cap_get_proc();
    if (!cap)
        return 0;
    ecap = cap_init();
    if (!ecap) {
        cap_free(cap);
        return 0;
    }
    rc = cap_compare(cap, ecap);
    cap_free(cap);
    cap_free(ecap);
    return rc;
#else
    return 0;
#endif
}

static int drop_caps(void)
{
#ifdef HAVE_LIBCAP
    int rc;
    cap_t cap;

    cap = cap_init();
    if (!cap)
        return -1;
    cap_clear(cap);
    rc = cap_set_proc(cap);
    cap_free(cap);
    return rc;
#else
    return 0;
#endif
}

int priv_drop(void)
{
  int err;

  assert(!caps_present());
  priv_drop_root();
  if (suid != 1) {
    assert(suid == sgid);
    return 0;
  }
  err = do_drop();
  if (err)
    return err;
  suid = 0;
  sgid = 0;
  return 0;
}

static void init_groups(uid_t uid, gid_t gid)
{
#ifdef HAVE_LIBCAP
  int err;
  struct passwd *pw = getpwuid(uid);
  if (!pw) {
    error("cannot get pw for %i\n", uid);
    err = cap_setgroups(gid, 0, NULL);
    assert(!err);
  } else {
    gid_t groups[NGROUPS_MAX];
    int ng = NGROUPS_MAX;
    int rc = getgrouplist(pw->pw_name, gid, groups, &ng);
    assert(rc > 0);
    err = cap_setgroups(gid, ng, groups);
    assert(!err);
  }
#endif
}

void priv_drop_total(void)
{
  int err;

  if (suid) {
    err = seteuid(euid);
    assert(!err);
    if (setreuid(euid, euid) != 0)
      error("Cannot drop suid: %s\n", strerror(errno));
    /* make sure privs were dropped */
    if (seteuid(uid) == 0) {
      error("suid: privs were not dropped\n");
      leavedos(3);
      return;
    }
    suid++;
  }
  if (sgid && !groups_dropped) {
    err = setegid(egid);
    assert(!err);
    if (setregid(egid, egid) != 0)
      error("Cannot drop sgid: %s\n", strerror(errno));
    /* make sure privs were dropped */
    if (setegid(gid) == 0) {
      error("sgid: privs were not dropped\n");
      leavedos(3);
      return;
    }
    sgid++;
  }

#ifdef __linux__
  if (!can_do_root_stuff) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
    prctl(PR_SET_DUMPABLE, 1);
  }
#endif
}

int running_suid_orig(void)
{
  if (!suid)
    return 0;
  assert(suid == 1);
  return 1;
}

int running_suid_changed(void)
{
  if (!suid)
    return 0;
  assert(suid == 2);
  return 1;
}

void priv_init(void)
{
  int err;
  int caps;
  const char *sh = getenv("SUDO_HOME"); // theoretical future var
  const char *h = getenv("HOME");

  uid = getuid();
  /* suid bit only sets euid & suid but not uid, sudo sets all 3 */
  if (!uid) under_root_login = 1;
  euid = cur_euid = geteuid();
  if (!euid) can_do_root_stuff = 1;
  if (!uid && !euid) skip_priv_setting = 1;
  gid = getgid();
  egid = cur_egid = getegid();

  /* must store the /proc/self/exe symlink contents before dropping
     privs! */
  dosemu_proc_self_exe = readlink_malloc("/proc/self/exe");
  /* For Fedora we must also save a file descriptor to /proc/self/maps */
  dosemu_proc_self_maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);

  caps = caps_present();
  if (euid && uid && euid != uid) {
    dbug_printf("suid %i detected\n", euid);
    suid++;
    err = seteuid(uid);
    assert(!err);
    /* We must not keep caps for entire init period, as this is insecure.
     * Also libpsx is broken, so we need to drop the caps before spawning
     * any thread. So set groups now and drop caps. Hopefully the logged-in
     * user doesn't need supplementary groups, having the ACLs set at login
     * time instead. */
    if (caps) {
      init_groups(euid, egid);
      groups_dropped = 1;
    }
  }
  if (caps)
    drop_caps();
  if (egid && gid && egid != gid) {
    mode_t um;

    dbug_printf("sgid %i detected\n", egid);
    sgid++;
    if (!groups_dropped) {
      err = setegid(gid);
      assert(!err);
       /* Remove S_IWGRP from umask to allow initial user to access dosemu2
       * files. Most needed if dosemu2 crashed and left stalled dirs in /tmp.
       * User should be able to clean them up. */
      um = umask(S_IWOTH);
      if (!(um & S_IWGRP))  // if S_IWGRP wasn't there, use old mask
        umask(um);
    } else
      sgid++;  // 2
  }

  if (!sh)
    sh = getenv("DOSEMU_SUDO_HOME");
  /* see if -E was used */
  if (under_root_login && sh && h && strcmp(sh, h) == 0) {
    /* check for sudo and set to original user */
    char *s = getenv("SUDO_GID");
    if (s) {
      gid = atoi(s);
      if (gid) {
        setregid(gid, egid);
      }
    }
    s = getenv("SUDO_UID");
    if (s) {
      uid = atoi(s);
      if (uid) {
        skip_priv_setting = 0;
        under_root_login = 0;
        using_sudo = 1;
        setreuid(uid, euid);
      }
    }
  }

  if (!can_do_root_stuff)
    skip_priv_setting = 1;

  if (!skip_priv_setting) _priv_off();

#ifdef __linux__
  if (!can_do_root_stuff) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
    prctl(PR_SET_DUMPABLE, 1);
  }
#endif
}

#ifdef SDL_SUPPORT
/* bug-fixer for gtk, see
 * https://gitlab.gnome.org/GNOME/gtk/-/issues/6629
 */
#include <dlfcn.h>
static int (*grsg)(gid_t *rgid, gid_t *egid, gid_t *sgid);

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
  int ret = -1;
  if (!grsg)
    grsg = dlsym(RTLD_NEXT, "getresgid");
  if (grsg)
    ret = grsg(rgid, egid, sgid);
  if (!running_suid_orig())
    return ret;
  dbug_printf("%s\n", __FUNCTION__);
  errno = ENOSYS;
  return -1;
}
#endif

#if defined(USE_ASAN) && !defined(__SANITIZE_LEAK__)
int __lsan_is_turned_off(void);
int __lsan_is_turned_off(void)
{
    /* lsan can't get its options from /proc/self/environ??? */
    return (suid || sgid);
}
#endif
