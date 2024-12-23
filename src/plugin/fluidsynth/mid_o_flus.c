/*
 *  Copyright (C) 2006 Stas Sergeev <stsp@users.sourceforge.net>
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
 * Purpose: fluidsynth midi synth
 *
 * Author: Stas Sergeev
 *
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fluidsynth.h>
#if defined(__APPLE__) || defined(__ANDROID__) /* to redefine sem_init() and related functions */
#include "utilities.h"
#else
#include <semaphore.h>
#endif
#include "seqbind.h"
#include "emu.h"
#include "init.h"
#include "timers.h"
#include "sound/midi.h"
#include "sound/sound.h"


#define midoflus_name "flus"
#define midoflus_longname "MIDI Output: FluidSynth device"
static const int flus_format = PCM_FORMAT_S16_LE;
static const float flus_srate = 44100.0;
#define FLUS_CHANNELS 2
#define FLUS_MAX_BUF 512
#define FLUS_MIN_BUF 128

static fluid_settings_t* settings;
static fluid_synth_t* synth;
static fluid_sequencer_t* sequencer;
static void *synthSeqID;
static int pcm_stream;
static int output_running, pcm_running;
static double mf_time_base;

static pthread_t syn_thr;
static sem_t syn_sem;
static pthread_mutex_t syn_mtx = PTHREAD_MUTEX_INITIALIZER;
static void *synth_thread(void *arg);

static int midoflus_init(void *arg)
{
    int ret;
    char *sfont = NULL;
    const char *def_sfonts[] = {
	"/usr/share/soundfonts/default.sf2",		// fedora
	PREFIX "/share/soundfonts/default.sf2",
	"/usr/share/sounds/sf2/default-GM.sf2",		// ubuntu
	"/usr/share/soundfonts/FluidR3_GM.sf2",		// fedora
	PREFIX "/share/soundfonts/FluidR3_GM.sf2",	// termux
	"/usr/share/sounds/sf2/FluidR3_GM.sf2.flac",	// ubuntu
	"/usr/share/sounds/sf2/FluidR3_GM.sf2",		// debian
	NULL };

    settings = new_fluid_settings();
    fluid_settings_setint(settings, "synth.lock-memory", 0);
    fluid_settings_setnum(settings, "synth.gain", config.fluid_volume / 4.0);
    fluid_settings_setnum(settings, "synth.sample-rate", flus_srate);
    if (config.fluid_sfont && config.fluid_sfont[0]) {
	if (access(config.fluid_sfont, R_OK) == 0) {
	    sfont = strdup(config.fluid_sfont);
	    ret = FLUID_OK;
	} else {
	    error("soundfont %s missing\n", config.fluid_sfont);
	    goto err1;
	}
    } else {
	ret = fluid_settings_dupstr(settings, "synth.default-soundfont",
		&sfont);
	if (ret == FLUID_FAILED) {
	    error("Your fluidsynth is too old\n");
	} else if (access(sfont, R_OK) != 0) {
	    warn("fluidsynth sound font unavailable at %s\n", sfont);
	    free(sfont);
	    sfont = NULL;
	}
	if (!sfont) {
	    int i = 0;

	    while (def_sfonts[i]) {
		if (access(def_sfonts[i], R_OK) == 0) {
		    sfont = strdup(def_sfonts[i]);
		    break;
		}
		i++;
	    }
	    if (!sfont) {
		error("soundfonts not found\n");
		goto err1;
	    }
	}
    }

    synth = new_fluid_synth(settings);
    ret = fluid_synth_sfload(synth, sfont, TRUE);
    if (ret == FLUID_FAILED) {
	error("fluidsynth: cannot load soundfont %s\n", sfont);
	free(sfont);
	goto err2;
    }
    S_printf("fluidsynth: loaded soundfont %s ID=%i\n", sfont, ret);
    free(sfont);
    fluid_settings_setstr(settings, "synth.midi-bank-select", "gm");
    sequencer = new_fluid_sequencer2(0);
    synthSeqID = fluid_sequencer_register_fluidsynth2(sequencer, synth);

    sem_init(&syn_sem, 0, 0);
    pthread_create(&syn_thr, NULL, synth_thread, NULL);
#if defined(HAVE_PTHREAD_SETNAME_NP) && defined(__GLIBC__)
    pthread_setname_np(syn_thr, "dosemu: fluid");
#endif
    pcm_stream = pcm_allocate_stream(FLUS_CHANNELS, "MIDI",
	    (void*)MC_MIDI);

    return 1;

err2:
    delete_fluid_synth(synth);
err1:
    delete_fluid_settings(settings);
    return 0;
}

static void midoflus_done(void *arg)
{
    pthread_cancel(syn_thr);
    pthread_join(syn_thr, NULL);
    sem_destroy(&syn_sem);

    delete_fluid_sequencer(sequencer);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
}

static void midoflus_start(void)
{
    S_printf("MIDI: starting fluidsynth\n");
    mf_time_base = GETusTIME(0);
    pthread_mutex_lock(&syn_mtx);
    pcm_prepare_stream(pcm_stream);
    fluid_sequencer_process(sequencer, 0);
    output_running = 1;
    pthread_mutex_unlock(&syn_mtx);
}

static void midoflus_write(unsigned char val)
{
    int ret;
    unsigned long long now = GETusTIME(0);
    int msec = (now - mf_time_base) / 1000;

    if (!output_running)
	midoflus_start();

    fluid_sequencer_process(sequencer, msec);
    ret = fluid_sequencer_add_midi_data_to_buffer(synthSeqID, &val, 1);
    if (ret != FLUID_OK)
	S_printf("MIDI: failed sending midi event\n");
}

static void mf_process_samples(int nframes)
{
    sndbuf_t buf[FLUS_MAX_BUF][FLUS_CHANNELS];
    int ret;
    ret = fluid_synth_write_s16(synth, nframes, buf, 0, 2, buf, 1, 2);
    if (ret != FLUID_OK) {
	error("MIDI: fluidsynth failed\n");
	return;
    }
    pcm_running = 1;
    pcm_write_interleaved(buf, nframes, flus_srate, flus_format,
	    FLUS_CHANNELS, pcm_stream);
}

static void process_samples(long long now, int min_buf)
{
    int nframes, retry;
    double period, mf_time_cur;
    mf_time_cur = pcm_get_stream_time(pcm_stream);
    do {
	retry = 0;
	period = pcm_frame_period_us(flus_srate);
	nframes = (now - mf_time_cur) / period;
	if (nframes > FLUS_MAX_BUF) {
	    nframes = FLUS_MAX_BUF;
	    retry = 1;
	}
	if (nframes >= min_buf) {
	    mf_process_samples(nframes);
	    mf_time_cur = pcm_get_stream_time(pcm_stream);
	    if (debug_level('S') >= 5)
		S_printf("MIDI: processed %i samples with fluidsynth\n", nframes);
	}
    } while (retry);
}

static void midoflus_stop(void *arg)
{
    long long now;
    int msec;
    pthread_mutex_lock(&syn_mtx);
    if (!output_running) {
	pthread_mutex_unlock(&syn_mtx);
	return;
    }
    now = GETusTIME(0);
    msec = (now - mf_time_base) / 1000;
    S_printf("MIDI: stopping fluidsynth at msec=%i\n", msec);
    /* advance past last event */
    fluid_sequencer_process(sequencer, msec);
    /* shut down all active notes */
    fluid_synth_system_reset(synth);
    if (pcm_running)
	pcm_flush(pcm_stream);
    pcm_running = 0;
    output_running = 0;
    pthread_mutex_unlock(&syn_mtx);
}

static void *synth_thread(void *arg)
{
    while (1) {
	sem_wait(&syn_sem);
	pthread_mutex_lock(&syn_mtx);
	if (!output_running) {
		pthread_mutex_unlock(&syn_mtx);
		continue;
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	process_samples(GETusTIME(0), FLUS_MIN_BUF);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_mutex_unlock(&syn_mtx);
    }
    return NULL;
}

static void midoflus_run(void)
{
    if (!output_running)
	return;
    sem_post(&syn_sem);
}

static int midoflus_cfg(void *arg)
{
    return pcm_parse_cfg(config.midi_driver, midoflus_name);
}

static const struct midi_out_plugin midoflus
#ifdef __cplusplus
={
    midoflus_name,
    midoflus_longname,
    midoflus_cfg,
    midoflus_init,
    midoflus_done,
    MIDI_W_PCM | MIDI_W_PREFERRED,
    midoflus_write,
    midoflus_stop,
    midoflus_run,
    ST_GM,
    0
};
#else
= {
    .name = midoflus_name,
    .longname = midoflus_longname,
    .get_cfg = midoflus_cfg,
    .open = midoflus_init,
    .close = midoflus_done,
    .weight = MIDI_W_PCM | MIDI_W_PREFERRED,
    .write = midoflus_write,
    .stop = midoflus_stop,
    .run = midoflus_run,
    .stype = ST_GM,
};
#endif

CONSTRUCTOR(static void midoflus_register(void))
{
    midi_register_output_plugin(&midoflus);
}
