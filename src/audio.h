#ifndef ORANGE386_AUDIO_H
#define ORANGE386_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <pico.h>   /* the RP2350 audio.h brings in the SDK platform macros */

/*
 * Audio output: the Linux counterpart of murm386 drivers/audio/audio.h.
 *
 * The mixer (audio_linux.c, ported from audio.c timer_callback) produces
 * 44.1 kHz stereo S16 frames on an ALSA playback thread, which takes the
 * role of the RP2350 core1 audio timer.  The PCM device is $AUDIODEV, or
 * "default" when unset.
 */

#define AUDIO_OUTPUT_AUTO 0
#define AUDIO_OUTPUT_PWM  1
#define AUDIO_OUTPUT_I2S  2
#define AUDIO_OUTPUT_HWAY 3
#define AUDIO_OUTPUT_HWAY8930 4

void audio_set_boot_output(int output);
void audio_set_enabled(bool);
void audio_set_volume(uint8_t);
uint8_t audio_get_volume(void);
bool audio_is_hway(void);
bool audio_is_hway8930(void);
void audio_init(void);
void audio_play_tone(unsigned hz, unsigned ms);

/* Linux host: stop the playback thread and close the PCM device. */
void audio_shutdown(void);

#endif /* ORANGE386_AUDIO_H */
