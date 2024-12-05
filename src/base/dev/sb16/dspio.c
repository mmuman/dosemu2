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
 * Purpose:
 *   DSP I/O layer, DMA and DAC handling. Also MIDI sometimes.
 *   Currently used by SB16, but may be used with anything, e.g. GUS.
 *
 * Author: Stas Sergeev.
 *
 */

#include "emu.h"
#include "timers.h"
#include "sig.h"
#include "sound/sound.h"
#include "sound/midi.h"
#include "sound.h"
#include "adlib.h"
#include "dma.h"
#include "sb16.h"
#include "dspio.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#define DAC_BASE_FREQ 8000
#define PCM_MAX_BUF 512

struct dspio_dma {
    unsigned int running:1;
    int num;
    int broken_hdma;
    int rate;
    int is16bit;
    int stereo;
    int samp_signed;
    int adpcm;
    int adpcm_need_ref;
    uint8_t adpcm_ref;
    int adpcm_step;
    int input;
    int silence;
    int dsp_fifo_enabled;
    hitimer_t time_cur;
};

struct dspio_state {
    double input_time_cur, midi_time_cur;
    int dma_strm, dac_strm;
    unsigned int input_running:1, output_running:1, dac_running:1, speaker:1;
    unsigned int pcm_input_running:1, lin_input_running:1, mic_input_running:1;
    int i_handle, i_started;
#define DSP_FIFO_SIZE 64
    struct rng_s fifo_in;
    struct rng_s fifo_out;
#define DSP_OUT_FIFO_TRIGGER 32
#define DSP_IN_FIFO_TRIGGER 32
    struct dspio_dma dma;
};

static void dma_get_silence(int is_signed, int is16bit, void *ptr)
{
    if (is16bit) {
	Bit16u *tmp16 = ptr;
	*tmp16 = is_signed ? 0 : 0x8000;
    } else {
	Bit8u *tmp8 = ptr;
	*tmp8 = is_signed ? 0 : 0x80;
    }
}

void dspio_toggle_speaker(struct dspio_state *dspio, int on)
{
    if (!on && dspio->speaker) {
	if (dspio->dac_running) {
	    pcm_flush(dspio->dac_strm);
	    dspio->dac_running = 0;
	}
	/* we don't flush PCM stream here because DSP uses PCM layer
	 * for timing, and timing is needed even when speaker is disabled... */
    }
    dspio->speaker = on;
}

int dspio_get_speaker_state(struct dspio_state *dspio)
{
    return dspio->speaker;
}

static void run_sound(void)
{
    if (!config.sound)
	return;
    dspio_run_synth();
    pcm_timer();
}

static int dspio_out_fifo_len(struct dspio_dma *dma)
{
    return dma->dsp_fifo_enabled ? DSP_OUT_FIFO_TRIGGER : 2;
}

static int dspio_in_fifo_len(struct dspio_dma *dma)
{
    return dma->dsp_fifo_enabled ? DSP_IN_FIFO_TRIGGER : 2;
}

static int dspio_output_fifo_filled(struct dspio_state *state)
{
    return rng_count(&state->fifo_out) >= dspio_out_fifo_len(&state->dma);
}

static int dspio_input_fifo_filled(struct dspio_state *state)
{
    return rng_count(&state->fifo_in) >= dspio_in_fifo_len(&state->dma);
}

static int dspio_input_fifo_empty(struct dspio_state *state)
{
    return !rng_count(&state->fifo_in);
}

static int dspio_get_dma_data(struct dspio_state *state, void *ptr, int is16bit)
{
    static int warned;
    if (sb_get_dma_data(ptr, is16bit))
	return 1;
    if (rng_count(&state->fifo_in)) {
	if (is16bit) {
	    rng_get(&state->fifo_in, ptr);
	} else {
	    Bit16u tmp;
	    rng_get(&state->fifo_in, &tmp);
	    *(Bit8u *) ptr = tmp;
	}
	return 1;
    }
    if (!warned) {
	error("SB: input fifo empty, adjust input and volume with SB mixer\n");
	warned++;
    }
    return 0;
}

static void dspio_put_dma_data(struct dspio_state *state, void *ptr, int is16bit)
{
    if (dspio_output_fifo_filled(state)) {
	error("SB: output fifo overflow\n");
	return;
    }
    if (is16bit) {
	rng_put(&state->fifo_out, ptr);
    } else {
	Bit16u tmp = *(Bit8u *) ptr;
	rng_put(&state->fifo_out, &tmp);
    }
}

/* https://wiki.multimedia.cx/index.php/Creative_8_bits_ADPCM */
static uint8_t decode_adpcm2(struct dspio_dma *dma, uint8_t val)
{
    int sign = (val & 2) ? -1 : 1;
    int value = val & 1;
    int sample = dma->adpcm_ref + sign * (value << (dma->adpcm_step + 2));
    if (sample < 0)
	sample = 0;
    if (sample > 255)
	sample = 255;
    dma->adpcm_ref = sample;
    if (value >= 1)
	dma->adpcm_step++;
    else if (value == 0)
	dma->adpcm_step--;
    if (dma->adpcm_step < 0)
	dma->adpcm_step = 0;
    if (dma->adpcm_step > 3)
	dma->adpcm_step = 3;
    return sample;
}

static uint8_t decode_adpcm3(struct dspio_dma *dma, uint8_t val)
{
    int sign = (val & 4) ? -1 : 1;
    int value = val & 3;
    int sample = dma->adpcm_ref + sign * (value << dma->adpcm_step);
    if (sample < 0)
	sample = 0;
    if (sample > 255)
	sample = 255;
    dma->adpcm_ref = sample;
    if (value >= 3)
	dma->adpcm_step++;
    else if (value == 0)
	dma->adpcm_step--;
    if (dma->adpcm_step < 0)
	dma->adpcm_step = 0;
    if (dma->adpcm_step > 3)
	dma->adpcm_step = 3;
    return sample;
}

static uint8_t decode_adpcm4(struct dspio_dma *dma, uint8_t val)
{
    int sign = (val & 8) ? -1 : 1;
    int value = val & 7;
    int sample = dma->adpcm_ref + sign * (value << dma->adpcm_step);
    if (sample < 0)
	sample = 0;
    if (sample > 255)
	sample = 255;
    dma->adpcm_ref = sample;
    if (value >= 5)
	dma->adpcm_step++;
    else if (value == 0)
	dma->adpcm_step--;
    if (dma->adpcm_step < 0)
	dma->adpcm_step = 0;
    if (dma->adpcm_step > 3)
	dma->adpcm_step = 3;
    return sample;
}

static int dspio_get_output_sample(struct dspio_state *state,
	sndbuf_t buf[PCM_MAX_BUF][SNDBUF_CHANS], int i, int j)
{
    int k;
    uint16_t val;
    int cnt = rng_count(&state->fifo_out);
    if (!cnt)
	return 0;
    rng_get(&state->fifo_out, &val);
    switch (state->dma.adpcm) {
	case 0:
	    buf[i][j] = val;
	    return 1;
	case 2:
	    assert(!j);
	    if (i + 4 > PCM_MAX_BUF)
		return 0;
	    for (k = 0; k < 4; k++)
		buf[i + k][j] = decode_adpcm2(&state->dma,
			(val >> (6 - k * 2)) & 0x3);
	    return k;
	case 3:
	    assert(!j);
	    if (i + 3 > PCM_MAX_BUF)
		return 0;
	    /* 2.6bits, not 3, see dosbox */
	    for (k = 0; k < 2; k++)
		buf[i + k][j] = decode_adpcm3(&state->dma,
			(val >> (5 - k * 3)) & 0x7);
	    buf[i + k][j] = decode_adpcm3(&state->dma, (val & 0x3) << 1);
	    return k + 1;
	case 4:
	    assert(!j);
	    if (i + 2 > PCM_MAX_BUF)
		return 0;
	    for (k = 0; k < 2; k++)
		buf[i + k][j] = decode_adpcm4(&state->dma,
			(val >> (4 - k * 4)) & 0xf);
	    return k;
    }
    error("should not be here, %i\n", state->dma.adpcm);
    return 0;
}

static int dspio_put_input_sample(struct dspio_state *state, void *ptr,
	int is16bit)
{
    int ret;
    if (!sb_input_enabled())
	return 0;
    if (dspio_input_fifo_filled(state)) {
	S_printf("SB: ERROR: input fifo overflow\n");
	return 0;
    }
    if (is16bit) {
	ret = rng_put(&state->fifo_in, ptr);
    } else {
	Bit16u tmp = *(Bit8u *) ptr;
	ret = rng_put(&state->fifo_in, &tmp);
    }
    return ret;
}

void dspio_clear_fifos(struct dspio_state *dspio)
{
    rng_clear(&dspio->fifo_in);
    rng_clear(&dspio->fifo_out);
    dspio->dma.dsp_fifo_enabled = 1;
}

static void dspio_i_start(void *arg)
{
    struct dspio_state *state = arg;
    state->i_started = 1;
}

static void dspio_i_stop(void *arg)
{
    struct dspio_state *state = arg;
    state->i_started = 0;
}

static const struct pcm_player player
#ifdef __cplusplus
{
    "SB REC",
    NULL, NULL, NULL, NULL, NULL,
    dspio_i_start,
    dspio_i_stop,
    PCM_F_PASSTHRU,
    PCM_ID_R,
    0
};
#else
= {
    .name = "SB REC",
    .start = dspio_i_start,
    .stop = dspio_i_stop,
    .flags = PCM_F_PASSTHRU,
    .id = PCM_ID_R,
};
#endif

static double dspio_get_volume(int id, int chan_dst, int chan_src, void *arg);
static int dspio_is_connected(int id, void *arg);
static int dspio_checkid2(void *id2, void *arg);

struct dspio_state *dspio_init(void)
{
    struct dspio_state *state;
    state = malloc(sizeof(struct dspio_state));
    if (!state)
	return NULL;
    memset(&state->dma, 0, sizeof(struct dspio_dma));
    state->input_running = state->pcm_input_running =
	state->lin_input_running = state->mic_input_running =
	state->output_running = state->dac_running = state->speaker =
	state->i_started = 0;
    state->dma.dsp_fifo_enabled = 1;

    rng_init(&state->fifo_in, DSP_FIFO_SIZE, 2);
    rng_init(&state->fifo_out, DSP_FIFO_SIZE, 2);

    state->i_handle = pcm_register_player(&player, state);
    pcm_init();

    pcm_set_volume_cb(dspio_get_volume);
    pcm_set_connected_cb(dspio_is_connected);
    pcm_set_checkid2_cb(dspio_checkid2);
    state->dac_strm = pcm_allocate_stream(1, "SB DAC", (void*)MC_VOICE);
    pcm_set_flag(state->dac_strm, PCM_FLAG_RAW);
    state->dma_strm = pcm_allocate_stream(2, "SB DMA", (void*)MC_VOICE);
    pcm_set_flag(state->dma_strm, PCM_FLAG_SLTS);

    midi_init();

    sigalrm_register_handler(run_sound);
    return state;
}

void dspio_reset(struct dspio_state *dspio)
{
}

void dspio_done(struct dspio_state *dspio)
{
    midi_done();
    /* shutdown midi before pcm as midi may use pcm */
    pcm_done();

    rng_destroy(&dspio->fifo_in);
    rng_destroy(&dspio->fifo_out);

    free(dspio);
}

void dspio_stop_midi(struct dspio_state *dspio)
{
    dspio->midi_time_cur = GETusTIME(0);
    midi_stop();
}

Bit32u dspio_get_midi_in_time(struct dspio_state *dspio)
{
    Bit32u delta = GETusTIME(0) - dspio->midi_time_cur;
    S_printf("SB: midi clock, delta=%i\n", delta);
    return delta;
}

static void dspio_start_output(struct dspio_state *state)
{
    if (state->output_running)
	return;
    S_printf("SB: starting output\n");
    pcm_prepare_stream(state->dma_strm);
    state->output_running = 1;
}

static void dspio_stop_output(struct dspio_state *state)
{
    if (!state->output_running)
	return;
    S_printf("SB: stopping output\n");
    pcm_flush(state->dma_strm);
    state->output_running = 0;
}

static void dspio_start_input(struct dspio_state *state)
{
    if (state->input_running)
	return;
    S_printf("SB: starting input\n");
    state->input_time_cur = GETusTIME(0);
    state->input_running = 1;
    if (!state->dma.rate) {
	S_printf("SB: not starting recorder\n");
	return;
    }
    if (!state->pcm_input_running) {
	pcm_reset_player(state->i_handle);
	state->pcm_input_running = 1;
    }
}

static void dspio_stop_input(struct dspio_state *state)
{
    if (!state->input_running)
	return;
    S_printf("SB: stopping input\n");
    state->input_running = 0;
    if (!state->dma.rate) {
	S_printf("SB: not stopping recorder\n");
	return;
    }
    if (!sb_dma_active())
	state->pcm_input_running = 0;
}

int dspio_input_enable(struct dspio_state *dspio, enum MixChan mc)
{
    struct dspio_state *state = dspio;
    switch (mc) {
    case MC_LINE:
	if (state->lin_input_running)
	    return 0;
	pcm_start_input((void *)MC_LINE);
	state->lin_input_running = 1;
	S_printf("SB: enabled LINE\n");
	break;
    case MC_MIC:
	if (state->mic_input_running)
	    return 0;
	pcm_start_input((void *)MC_MIC);
	state->mic_input_running = 1;
	S_printf("SB: enabled MIC\n");
	break;
    default:
	return 0;
    }
    return 1;
}

int dspio_input_disable(struct dspio_state *dspio, enum MixChan mc)
{
    struct dspio_state *state = dspio;
    switch (mc) {
    case MC_LINE:
	if (!state->lin_input_running)
	    return 0;
	pcm_stop_input((void *)MC_LINE);
	state->lin_input_running = 0;
	S_printf("SB: disabled LINE\n");
	break;
    case MC_MIC:
	if (!state->mic_input_running)
	    return 0;
	pcm_stop_input((void *)MC_MIC);
	state->mic_input_running = 0;
	S_printf("SB: disabled MIC\n");
	break;
    default:
	return 0;
    }
    return 1;
}

static int do_run_dma(struct dspio_state *state)
{
    Bit8u dma_buf[2];
    struct dspio_dma *dma = &state->dma;

    dma_get_silence(dma->samp_signed, dma->is16bit, dma_buf);
    if (!dma->silence) {
	if (dma->input)
	    dspio_get_dma_data(state, dma_buf, dma->is16bit);
	if (dma_pulse_DRQ(dma->num, dma_buf) != DMA_DACK) {
	    S_printf("SB: DMA %i doesn't DACK!\n", dma->num);
	    return 0;
	}
	if (dma->broken_hdma) {
	    if (dma_pulse_DRQ(dma->num, dma_buf + 1) != DMA_DACK) {
		S_printf("SB: DMA (broken) %i doesn't DACK!\n", dma->num);
		return 0;
	    }
	}
    }
    if (!dma->input) {
	if (dma->adpcm && dma->adpcm_need_ref) {
	    dma->adpcm_ref = dma_buf[0];
	    dma->adpcm_step = 0;
	    dma->adpcm_need_ref = 0;
	}
	dspio_put_dma_data(state, dma_buf, dma->is16bit);
    }
    return 1;
}

static int dspio_run_dma(struct dspio_state *state)
{
#define DMA_TIMEOUT_US 100000
    int ret;
    struct dspio_dma *dma = &state->dma;
    hitimer_t now = GETusTIME(0);
    ret = do_run_dma(state);
    if (ret) {
	sb_handle_dma();
	dma->time_cur = now;
    } else {
	sb_dma_nack();
	if (now - dma->time_cur > DMA_TIMEOUT_US) {
	    S_printf("SB: Warning: DMA busy for too long, releasing\n");
//		error("SB: DMA timeout\n");
	    sb_handle_dma_timeout();
	}
    }
    return ret;
}

static void get_dma_params(struct dspio_dma *dma)
{
    int dma_16bit = sb_dma_16bit();
    int dma_num = dma_16bit ? sb_get_hdma_num() : sb_get_dma_num();
    int broken_hdma = (dma_16bit && dma_num == -1);
    if (broken_hdma) {
	dma_num = sb_get_dma_num();
	S_printf("SB: Warning: HDMA is broken, using 8-bit DMA channel %i\n",
	     dma_num);
    }

    dma->num = dma_num;
    dma->is16bit = dma_16bit;
    dma->broken_hdma = broken_hdma;
    dma->rate = sb_get_dma_sampling_rate();
    dma->stereo = sb_dma_samp_stereo();
    dma->samp_signed = sb_dma_samp_signed();
    dma->input = sb_dma_input();
    dma->silence = sb_dma_silence();
    dma->dsp_fifo_enabled = sb_fifo_enabled();
    dma->adpcm = sb_dma_adpcm();
    dma->adpcm_need_ref = sb_dma_adpcm_ref();
}

static int dspio_fill_output(struct dspio_state *state)
{
    int dma_cnt = 0;
    while (state->dma.running && !dspio_output_fifo_filled(state)) {
	if (!dspio_run_dma(state))
	    break;
	dma_cnt++;
    }
#if 0
    if (!state->output_running && !sb_output_fifo_empty())
#else
    /* incomplete fifo needs a timeout, so lets not deal with it at all.
     * Instead, deal with the filled fifo only. */
    if (dspio_output_fifo_filled(state))
#endif
	dspio_start_output(state);
    return dma_cnt;
}

static int dspio_drain_input(struct dspio_state *state)
{
    int dma_cnt = 0;
    while (state->dma.running && !dspio_input_fifo_empty(state)) {
	if (!dspio_run_dma(state))
	    break;
	dma_cnt++;
    }
    return dma_cnt;
}

void dspio_start_dma(struct dspio_state *dspio)
{
    int dma_cnt = 0;
    dspio->dma.running = 1;
    dspio->dma.time_cur = GETusTIME(0);
    get_dma_params(&dspio->dma);

    if (dspio->dma.input) {
	dspio_start_input(dspio);
    } else {
	dma_cnt = dspio_fill_output(dspio);
	if (dspio->dma.running && dspio_output_fifo_filled(dspio))
	    S_printf("SB: Output filled, processed %i DMA cycles\n",
		     dma_cnt);
	else
	    S_printf("SB: Output fillup incomplete (%i %i %i)\n",
		     dspio->dma.running, dspio->output_running, dma_cnt);
    }
}

void dspio_stop_dma(struct dspio_state *dspio)
{
    dspio_stop_input(dspio);
    dspio->dma.running = 0;
}

static int calc_nframes(struct dspio_state *state,
	hitimer_t time_beg, hitimer_t time_dst)
{
    int nfr;

    if (time_dst < time_beg)
	return 0;
    if (state->dma.rate) {
	nfr = (time_dst - time_beg) / pcm_frame_period_us(state->dma.rate) + 1;
	if (nfr < 0)	// happens because of get_stream_time() hack
	    nfr = 0;
	if (nfr > PCM_MAX_BUF)
	    nfr = PCM_MAX_BUF;
    } else {
	nfr = 1;
    }
    return nfr;
}

static void dspio_process_dma(struct dspio_state *state)
{
    int dma_cnt, nfr, in_fifo_cnt, out_fifo_cnt, i, j;
    unsigned long long time_dst;
    double output_time_cur = 0;
    int n[SNDBUF_CHANS];
    sndbuf_t buf[PCM_MAX_BUF][SNDBUF_CHANS];
    static int warned;

    dma_cnt = in_fifo_cnt = out_fifo_cnt = 0;

    if (state->dma.running) {
	state->dma.stereo = sb_dma_samp_stereo();
	state->dma.rate = sb_get_dma_sampling_rate();
	state->dma.samp_signed = sb_dma_samp_signed();
	state->dma.dsp_fifo_enabled = sb_fifo_enabled();
	dma_cnt += state->dma.input ? dspio_drain_input(state) :
	    dspio_fill_output(state);
    }

    if (!state->output_running && !state->input_running)
	return;

    time_dst = GETusTIME(0);
    if (state->output_running) {
	output_time_cur = pcm_get_stream_time(state->dma_strm);
	nfr = calc_nframes(state, output_time_cur, time_dst);
    } else {
	nfr = 0;
    }
    if (nfr > PCM_MAX_BUF)
	nfr = PCM_MAX_BUF;
    for (i = 0; i < nfr;) {
	memset(n, 0, sizeof(n));
	for (j = 0; j < state->dma.stereo + 1; j++) {
	    if (state->dma.running && !dspio_output_fifo_filled(state)) {
		if (!dspio_run_dma(state))
		    break;
		dma_cnt++;
	    }
	    n[j] = dspio_get_output_sample(state, buf, i, j);
	    if (!n[j]) {
		if (out_fifo_cnt && debug_level('S') >= 5)
		    S_printf("SB: no output samples\n");
		break;
	    }
	    if (j)
		assert(n[j] == n[0]);
#if 0
	    /* if speaker disabled, overwrite DMA data with silence */
	    /* on SB16 is not used */
	    if (!state->speaker)
		dma_get_silence(state->dma.samp_signed,
			state->dma.is16bit, &buf[i][j]);
#endif
	}
	if (j != state->dma.stereo + 1)
	    break;
	i += n[0];
    }
    out_fifo_cnt = i;
    if (out_fifo_cnt && state->dma.rate) {
	pcm_write_interleaved(buf, out_fifo_cnt, state->dma.rate,
			  pcm_get_format(state->dma.is16bit,
					 state->dma.samp_signed),
			  state->dma.stereo + 1, state->dma_strm);
	output_time_cur = pcm_get_stream_time(state->dma_strm);
	if (state->dma.running && output_time_cur > time_dst - 1) {
	    pcm_clear_flag(state->dma_strm, PCM_FLAG_POST);
	    warned = 0;
	}
    }
    if (out_fifo_cnt < nfr) {
	/* not enough samples, see why */
	if (!sb_dma_active()) {
	    dspio_stop_output(state);
	} else {
	    if (nfr && (!warned || debug_level('S') >= 9)) {
		S_printf("SB: Output FIFO exhausted while DMA is still active (ol=%f)\n",
			 time_dst - output_time_cur);
		warned = 1;
	    }
	    if (state->dma.running)
		S_printf("SB: Output FIFO exhausted while DMA is running (no DACK?)\n");
	    /* DMA is active but currently not running and the FIFO is
	     * already exhausted. Normally we should flush the channel
	     * and stop the output timing.
	     * HACK: try to not flush the channel for as long as possible
	     * in a hope the PCM buffers are large enough to hold till
	     * the DMA is restarted. */
	    pcm_set_flag(state->dma_strm, PCM_FLAG_POST);
	    /* awake dosemu */
	    reset_idle(0);
	}
    }

    /* TODO: sync also input time with PCM? */
    if (state->input_running)
	nfr = calc_nframes(state, state->input_time_cur, time_dst);
    else
	nfr = 0;
    if (nfr && state->i_started && sb_input_enabled()) {
	struct player_params params;
	params.rate = state->dma.rate;
	params.channels = state->dma.stereo + 1;
	params.format = pcm_get_format(state->dma.is16bit,
		state->dma.samp_signed);
	params.handle = state->i_handle;
	nfr = pcm_data_get_interleaved(buf, nfr, &params);
    }
    if (!state->i_started) {
	for (i = 0; i < nfr; i++) {
	    for (j = 0; j < state->dma.stereo + 1; j++)
		dma_get_silence(state->dma.samp_signed,
			state->dma.is16bit, &buf[i][j]);
	}
    }
    for (i = 0; i < nfr; i++) {
	for (j = 0; j < state->dma.stereo + 1; j++) {
	    if (sb_input_enabled()) {
		if (!dspio_put_input_sample(state, &buf[i][j],
			state->dma.is16bit))
		    break;
	    }
	}
	if (j == state->dma.stereo + 1)
	    in_fifo_cnt++;
	for (j = 0; j < state->dma.stereo + 1; j++) {
	    if (state->dma.running) {
		if (!dspio_run_dma(state))
		    break;
		dma_cnt++;
	    }
	}
	if (!state->input_running || (j != state->dma.stereo + 1))
	    break;
    }
    if (in_fifo_cnt) {
	if (state->dma.rate) {
	    state->input_time_cur += in_fifo_cnt *
		    pcm_frame_period_us(state->dma.rate);
	} else {
	    state->input_time_cur = time_dst;
	}
    }

    if (debug_level('S') >= 7 && (in_fifo_cnt || out_fifo_cnt || dma_cnt))
	S_printf("SB: Processed %i %i FIFO, %i DMA, or=%i dr=%i\n",
	     in_fifo_cnt, out_fifo_cnt, dma_cnt, state->output_running, state->dma.running);
}

void dspio_run_synth(void)
{
    adlib_timer();
    midi_timer();
}

void dspio_timer(struct dspio_state *dspio)
{
    dspio_process_dma(dspio);
}

void dspio_write_dac(struct dspio_state *dspio, Bit8u samp)
{
    sndbuf_t buf[1][SNDBUF_CHANS];
#if 0
    /* on SB16 speaker control does not exist */
    if (!dspio->speaker)
	return;
#endif
    buf[0][0] = samp;
    dspio->dac_running = 1;
    pcm_write_interleaved(buf, 1, DAC_BASE_FREQ, PCM_FORMAT_U8,
			  1, dspio->dac_strm);
}

/* the volume APIs for sb16 and sndpcm are very different.
 * We need a lot of glue below to get them to work together.
 * I wonder if it is possible to design the APIs with fewer glue in between. */
static double dspio_get_volume(int id, int chan_dst, int chan_src, void *arg)
{
    double vol;
    enum MixSubChan msc;
    enum MixRet mr = MR_UNSUP;
    enum MixChan mc = (enum MixChan)(intptr_t)arg;
    int chans = sb_mixer_get_chan_num(mc);

    if (chan_src >= chans)
	return 0;
    if (mc == MC_NONE)
	return 1.0;
    switch (chan_dst) {
    case SB_CHAN_L:
	switch (chan_src) {
	case SB_CHAN_L:
	    msc = (chans == 1 ? MSC_MONO_L : MSC_L);
	    break;
	case SB_CHAN_R:
	    msc = MSC_RL;
	    break;
	default:
	    return 0;
	}
	break;
    case SB_CHAN_R:
	switch (chan_src) {
	case SB_CHAN_L:
	    msc = (chans == 1 ? MSC_MONO_R : MSC_LR);
	    break;
	case SB_CHAN_R:
	    msc = MSC_R;
	    break;
	default:
	    return 0;
	}
	break;
    default:
	return 0;
    }

    switch (id) {
    case PCM_ID_P:
	mr = sb_mixer_get_output_volume(mc, msc, &vol);
	break;
    case PCM_ID_R:
	mr = sb_mixer_get_input_volume(mc, msc, &vol);
	break;
    }

    if (mr != MR_OK)
	return 0;
    return vol;
}

/* FIXME: this is too slow! Needs caching to avoid re-calcs. */
double dspio_calc_vol(int val, int step, int init_db)
{
#define LOG_SCALE 0.02
#if HAVE_DECL_EXP10F
    return exp10f(LOG_SCALE * (val * step + init_db));
#else
    return pow(10, LOG_SCALE * (val * step + init_db));
#endif
}

static int dspio_is_connected(int id, void *arg)
{
    enum MixChan mc = (enum MixChan)(intptr_t)arg;

    if (mc == MC_NONE)	// connect anonymous streams only to playback (P)
	return (id == PCM_ID_P);
    switch (id) {
    case PCM_ID_P:
	return sb_is_output_connected(mc);
    case PCM_ID_R:
	return sb_is_input_connected(mc);
    }
    return 0;
}

static int dspio_checkid2(void *id2, void *arg)
{
    enum MixChan mc = (enum MixChan)(intptr_t)arg;
    enum MixChan mc2 = (enum MixChan)(intptr_t)id2;

    return (mc2 == MC_NONE || mc2 == mc);
}
