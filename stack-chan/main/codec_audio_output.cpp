#include "codec_audio_output.h"

#include "audio/xiaopai_audio_service.h"

#include <algorithm>

bool codec_audio_output_begin(int sample_rate, int volume_percent)
{
    (void)sample_rate;
    if (!audio_service_start()) {
        return false;
    }
    audio_service_set_volume_percent(volume_percent);
    return true;
}

bool codec_audio_output_write(const int16_t* samples, size_t sample_count)
{
    return audio_service_play_pcm_16k(samples, sample_count,
                                      AudioPlayOptions{.wait = true, .drop_oldest = false});
}

void codec_audio_output_drain()
{
    audio_service_wait_playback_idle(pdMS_TO_TICKS(1000));
}

void codec_audio_output_stop()
{
    audio_service_abort_playback();
}

void codec_audio_output_end()
{
    audio_service_wait_playback_idle(pdMS_TO_TICKS(1000));
}

bool codec_audio_output_is_active()
{
    return audio_service_is_playing();
}

void codec_audio_output_set_volume_percent(int volume_percent)
{
    audio_service_set_volume_percent(volume_percent);
}

void codec_audio_output_dump_state()
{
    audio_service_dump_state();
}

bool codec_audio_output_test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent)
{
    return audio_service_test_tone(sample_rate, tone_hz, duration_ms, volume_percent);
}
