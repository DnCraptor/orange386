/*
 * orange386 - ALSA audio output.
 *
 * Port of murm386 drivers/audio/audio.c: the per-sample mixer of
 * timer_callback() (I2S output path) runs here on a playback thread at
 * 44.1 kHz, the role RP2350 core1 plays.  Devices are read from that thread
 * exactly as core1 reads them; the emulation thread owns all writes.
 */
#include "audio.h"
#include "ay_hw.h"
#include "pc.h"
#include "dss.h"
#include "adlib.h"
#include "config_save.h"
#include "csm.h"
#include "csm_psg.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

extern PC *pc;

#define AUDIO_RATE        SOUND_FREQUENCY
#define AUDIO_PERIOD      256        /* frames per write, ~5.8 ms */
#define AUDIO_LATENCY_US  40000u

static uint8_t volume = 0; // 0 - MAX vol, 16 - silence
static uint8_t prev = 0;
static int boot_audio_output = AUDIO_OUTPUT_AUTO;

static snd_pcm_t *pcm;
static pthread_t audio_thread;
static atomic_bool audio_running;
static bool audio_started;

/* audio_play_tone(): square wave mixed in by the playback thread */
static atomic_uint tone_frames;
static atomic_uint tone_half_period;   /* in frames */

void audio_set_boot_output(int output) {
    if (output >= AUDIO_OUTPUT_AUTO && output <= AUDIO_OUTPUT_HWAY8930)
        boot_audio_output = output;
}

void audio_set_enabled(bool v) {
    if (v) {
        volume = prev;
    } else {
        prev = volume;
        volume = 16;
    }
}

void audio_set_volume(uint8_t vol) {
    prev = 16 - vol;
}

uint8_t audio_get_volume(void) {
    return 16 - prev;
}

/* No hardware AY-3-8910 on this platform: the PSG is always emulated. */
bool audio_is_hway(void) {
    return false;
}

bool audio_is_hway8930(void) {
    return false;
}

void ay_hw_init(void) {}
void ay_hw_write_pcm(uint8_t sample) { (void)sample; }
void ay_hw_psg_select_register(uint8_t reg) { (void)reg; }
void ay_hw_psg_write_data(uint8_t value) { (void)value; }
void ay_hw_psg_write_registers(const uint8_t regs[16]) { (void)regs; }

void audio_play_tone(unsigned hz, unsigned ms) {
    if (!hz || !ms)
        return;
    unsigned half = AUDIO_RATE / (2u * hz);
    atomic_store(&tone_half_period, half ? half : 1u);
    atomic_store(&tone_frames, (unsigned)((uint64_t)AUDIO_RATE * ms / 1000u));
}

//=============================================================================
// Mixer (audio.c timer_callback, I2S output path)
//=============================================================================
static void mix_sample(int16_t *out_r, int16_t *out_l) {
    static uint32_t dss_phase = 0;
    static int dss_v = 0;

    if (!pc) {
        *out_r = *out_l = 0;
        return;
    }

    /* Disney Sound Source owns a fixed 7 kHz playback clock.  This mixer is
     * called at 44.1 kHz, so use a rational phase accumulator. */
    if (pc->dss_enabled) {
        dss_phase += 7000;
        if (dss_phase >= 44100) {
            dss_phase -= 44100;
            dss_v = dss_sample();
        }
    }
    // output:
    int b_v = 0;
    int r_v = 0;
    int l_v = 0;
    if (pc->pcspk_enabled) {
        b_v = pcspk_sample(pc->pcspk);
    }
    {
        int16_t sample = csm_getsample();
        r_v += sample;
        l_v += sample;
    }
    if (pc->covox_enabled != COVOX_DISABLED) {
        /* 8-bit unsigned DAC -> signed mixer. */
        int32_t sample = ((int32_t)pc->covox_sample - 128) << 8;
        r_v += sample;
        l_v += sample;
    }
    if (COVOX_IS_SOUND_MASTER(pc->covox_enabled)) {
        int16_t sample = csm_psg_sample();
        r_v += sample;
        l_v += sample;
    }
    if (pc->tandy_enabled) {
        int16_t sample = sn76489_sample(); // 16-bit
        r_v += sample;
        l_v += sample;
    }
    if (pc->mpu401_enabled) {
        int16_t sample = midi_sample();
        r_v += sample;
        l_v += sample;
    }
    if (pc->adlib_enabled) {
        int16_t sample = adlib_getsample(pc->adlib);
        r_v += sample;
        l_v += sample;
    }
    if (pc->sb16_enabled) {
        sb16_getsample(pc->sb16, &r_v, &l_v);
    }
    r_v += dss_v;
    l_v += dss_v;

    unsigned tone = atomic_load_explicit(&tone_frames, memory_order_relaxed);
    if (tone) {
        static unsigned tone_pos;
        unsigned half = atomic_load_explicit(&tone_half_period, memory_order_relaxed);
        int t = ((tone_pos++ / half) & 1u) ? -16384 : 16384;
        r_v += t;
        l_v += t;
        atomic_store_explicit(&tone_frames, tone - 1, memory_order_relaxed);
    }

    if (b_v) { r_v = l_v = 32767; }
    if (r_v > 32767) r_v = 32767;
    if (r_v < -32768) r_v = -32768;
    if (l_v > 32767) l_v = 32767;
    if (l_v < -32768) l_v = -32768;
    *out_r = (int16_t)(r_v >> volume);
    *out_l = (int16_t)(l_v >> volume);
}

static void *audio_main(void *arg) {
    (void)arg;
    int16_t buf[AUDIO_PERIOD * 2];
    while (atomic_load(&audio_running)) {
        for (int i = 0; i < AUDIO_PERIOD; ++i) {
            int16_t r, l;
            mix_sample(&r, &l);
            buf[i * 2 + 0] = l;
            buf[i * 2 + 1] = r;
        }
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, AUDIO_PERIOD);
        if (n < 0) {
            n = snd_pcm_recover(pcm, (int)n, 1);
            if (n < 0) {
                fprintf(stderr, "audio: %s\n", snd_strerror((int)n));
                break;
            }
        }
    }
    return NULL;
}

/*
 * Sound output device, first match:
 *   AUDIODEV in the environment (as SDL programs use it),
 *   audio_device= in config.ini,
 *   the HDMI card, which carries the picture as well,
 *   ALSA "default".
 * Root usually starts the emulator through sudo, which does not pass the
 * caller's environment on; config.ini and the HDMI default do not depend on it.
 */
static const char *audio_device_name(void) {
    const char *dev = getenv("AUDIODEV");
    if (dev && *dev)
        return dev;
    dev = config_get_audio_device();
    if (dev && *dev)
        return dev;
    if (snd_card_get_index("HDMI") >= 0)
        return "plughw:CARD=HDMI,DEV=0";
    return "default";
}

void audio_init(void) {
    if (audio_started)
        return;
    const char *dev = audio_device_name();
    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "audio: cannot open %s: %s (sound disabled)\n",
                dev, snd_strerror(err));
        pcm = NULL;
        return;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, AUDIO_RATE, 1, AUDIO_LATENCY_US);
    if (err < 0) {
        fprintf(stderr, "audio: %s: %s (sound disabled)\n", dev, snd_strerror(err));
        snd_pcm_close(pcm);
        pcm = NULL;
        return;
    }
    atomic_store(&audio_running, true);
    err = pthread_create(&audio_thread, NULL, audio_main, NULL);
    if (err) {
        fprintf(stderr, "audio: cannot start thread: %s\n", strerror(err));
        atomic_store(&audio_running, false);
        snd_pcm_close(pcm);
        pcm = NULL;
        return;
    }
    audio_started = true;
    printf("audio: %s, %u Hz stereo\n", dev, (unsigned)AUDIO_RATE);
}

void audio_shutdown(void) {
    if (!audio_started)
        return;
    atomic_store(&audio_running, false);
    pthread_join(audio_thread, NULL);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
    audio_started = false;
}
