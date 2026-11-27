#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

#include <Arduino.h>

class AudioFilter {
private:
    float dc_offset;
    float noise_floor;
    float gain;
public:
    AudioFilter(float g = 4.0) {
        dc_offset = 0.0;
        noise_floor = 80.0;
        gain = g;
    }
    
    int16_t process(int32_t pcm16) {
        // Lọc thông cao (IIR Filter) để triệt tiêu hoàn toàn DC Offset (điện áp nền)
        dc_offset = 0.995 * dc_offset + 0.005 * (float)pcm16;
        float clean_float = (float)pcm16 - dc_offset;
        float amplitude = clean_float >= 0.0f ? clean_float : -clean_float;

        if (amplitude < noise_floor * 2.0f) {
            noise_floor = 0.999f * noise_floor + 0.001f * amplitude;
        }

        float gate_threshold = noise_floor * 2.5f;
        if (gate_threshold < 120.0f) gate_threshold = 120.0f;
        if (amplitude < gate_threshold) {
            clean_float *= 0.25f;
        } else if (amplitude < gate_threshold * 2.0f) {
            float speech_ratio = (amplitude - gate_threshold) / gate_threshold;
            clean_float *= 0.25f + 0.75f * speech_ratio;
        }

        // Giảm nhiễu nền mềm để không cắt các âm tiết nhỏ.
        clean_float *= gain;
        
        // Ép ngưỡng (Clipping) để chống nổ tiếng
        if (clean_float > 32767.0f) clean_float = 32767.0f;
        if (clean_float < -32768.0f) clean_float = -32768.0f;
        
        return (int16_t)clean_float;
    }
};

#endif