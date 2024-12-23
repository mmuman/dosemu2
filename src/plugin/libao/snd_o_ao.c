/*
 *  Copyright (C) 2015 Stas Sergeev <stsp@users.sourceforge.net>
 *
 * The below copyright strings have to be distributed unchanged together
 * with this file. This prefix can not be modified or separated.
 */

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
 * Purpose: libao sound output interface to "live" targets (pulseaudio).
 *
 * File-based targets are handled elsewhere.
 * libao only pretends to provide the unification. The fact is, the
 * entirely different code is needed to handle the file-based targets.
 *
 * Author: Stas Sergeev.
 */

#include "emu.h"
#include "init.h"
#include "sound/sound.h"
#include "ao_init.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ao/ao.h>
#include <pthread.h>
#if defined(__APPLE__) || defined(__ANDROID__) /* to redefine sem_init() and related functions */
#include "utilities.h"
#else
#include <semaphore.h>
#endif

#define aosnd_name "ao"
#define aosnd_longname "Sound Output: libao"
static ao_device *ao;
static struct player_params params;
static int started;
static pthread_mutex_t start_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t start_sem;
static sem_t stop_sem;
static pthread_t write_thr;
static void *aosnd_write(void *arg);


static int aosnd_cfg(void *arg)
{
    if (config.libao_sound == 1)
	return PCM_CF_ENABLED;
    return pcm_parse_cfg(config.sound_driver, aosnd_name);
}

static int aosnd_open(void *arg)
{
    ao_sample_format info = {};
#if 0
    ao_option opt = {};
#endif
    int id;

    ao_init();

    params.rate = 44100;
    params.format = PCM_FORMAT_S16_LE;
    params.channels = 2;
    info.channels = params.channels;
    info.rate = params.rate;
    info.byte_format = AO_FMT_LITTLE;
    info.bits = 16;
    id = ao_default_driver_id();
    if (id == -1) {
	S_printf("libao: default driver not specified, trying alsa\n");
	id = ao_driver_id("alsa");
    }
    if (id == -1) {
	error("libao: unable to get driver id\n");
	return 0;
    }
#if 0
    /* for alsa the default settings are fine, but for pulse we
     * need to manually increase buffer_time to avoid clicks...
     * https://bugzilla.redhat.com/show_bug.cgi?id=1193688
     */
    opt.key = "buffer_time";
    opt.value = "40";
    ao = ao_open_live(id, &info, &opt);
    if (!ao) {
	/* because of this bug:
	 * https://bugs.launchpad.net/ubuntu/+source/libao/+bug/1525776
	 * we need to retry without options... libao is so lame, do you
	 * remember that? */
	ao = ao_open_live(id, &info, NULL);
    }
#else
    ao = ao_open_live(id, &info, NULL);
#endif
    if (!ao) {
	error("libao: unable to open output device\n");
	return 0;
    }

    pcm_setup_hpf(&params);

    sem_init(&start_sem, 0, 0);
    sem_init(&stop_sem, 0, 0);
    pthread_create(&write_thr, NULL, aosnd_write, NULL);
#if defined(HAVE_PTHREAD_SETNAME_NP) && defined(__GLIBC__)
    pthread_setname_np(write_thr, "dosemu: libao");
#endif
    return 1;
}

static void aosnd_close(void *arg)
{
    pthread_cancel(write_thr);
    pthread_join(write_thr, NULL);
    sem_destroy(&start_sem);
    sem_destroy(&stop_sem);
    ao_close(ao);
}

static void *aosnd_write(void *arg)
{
    #define BUF_SIZE 4096
    char buf[BUF_SIZE];
    uint32_t size;
    int l_started;
    while (1) {
	sem_wait(&start_sem);
	while (1) {
	    pthread_mutex_lock(&start_mtx);
	    l_started = started;
	    pthread_mutex_unlock(&start_mtx);
	    if (!l_started)
		break;
	    size = pcm_data_get(buf, sizeof(buf), &params);
	    if (!size) {
		usleep(10000);
		continue;
	    }
	    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	    ao_play(ao, buf, size);
	    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	    pthread_testcancel();
	}
	sem_post(&stop_sem);
    }
    return NULL;
}

static void aosnd_start(void *arg)
{
    pthread_mutex_lock(&start_mtx);
    started = 1;
    sem_post(&start_sem);
    pthread_mutex_unlock(&start_mtx);
}

static void aosnd_stop(void *arg)
{
    pthread_mutex_lock(&start_mtx);
    if (!started) {
	pthread_mutex_unlock(&start_mtx);
	return;
    }
    started = 0;
    pthread_mutex_unlock(&start_mtx);
    sem_wait(&stop_sem);
}

static const struct pcm_player player
#ifdef __cplusplus
{
    aosnd_name,
    aosnd_longname,
    aosnd_cfg,
    aosnd_open,
    aosnd_close,
    NULL,
    aosnd_start,
    aosnd_stop,
    0,
    PCM_ID_P,
    0
};
#else
= {
    .name = aosnd_name,
    .longname = aosnd_longname,
    .get_cfg = aosnd_cfg,
    .open = aosnd_open,
    .close = aosnd_close,
    .start = aosnd_start,
    .stop = aosnd_stop,
    .id = PCM_ID_P,
};
#endif

CONSTRUCTOR(static void aosnd_init(void))
{
    params.handle = pcm_register_player(&player, NULL);
}
