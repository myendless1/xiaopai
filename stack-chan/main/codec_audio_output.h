#pragma once

#include <cstddef>
#include <cstdint>

bool codec_audio_output_begin(int sample_rate, int volume_percent);
bool codec_audio_output_write(const int16_t* samples, size_t sample_count);
void codec_audio_output_drain();
void codec_audio_output_stop();
void codec_audio_output_end();
bool codec_audio_output_is_active();
void codec_audio_output_set_volume_percent(int volume_percent);
void codec_audio_output_dump_state();
bool codec_audio_output_test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent);
