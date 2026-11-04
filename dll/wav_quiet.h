// A sound of the game at a lower volume. PlaySound has no volume of its own, and the wave device's volume is the whole
// game's: so the sound is decoded (IMA ADPCM, as the game's chat sound is) and played as plain samples, scaled.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *wav_quiet(const unsigned char *f, size_t n, float gain) {
    static const int STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
        1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int MOVE[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    if (n < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4)) return NULL;
    const unsigned char *fmt = NULL, *data = NULL;
    size_t data_n = 0;
    uint32_t total = 0;
    for (size_t at = 12; at + 8 <= n; ) {
        uint32_t size;
        memcpy(&size, f + at + 4, 4);
        if (size > n - at - 8) break;
        if (!memcmp(f + at, "fmt ", 4) && size >= 20) fmt = f + at + 8;
        if (!memcmp(f + at, "data", 4)) { data = f + at + 8; data_n = size; }
        if (!memcmp(f + at, "fact", 4) && size >= 4) memcpy(&total, f + at + 8, 4);
        at += 8 + (size_t)size + (size & 1);
    }
    if (!fmt || !data) return NULL;
    uint16_t format, channels, block, per_block;
    uint32_t rate;
    memcpy(&format, fmt, 2); memcpy(&channels, fmt + 2, 2); memcpy(&rate, fmt + 4, 4); memcpy(&block, fmt + 12, 2); memcpy(&per_block, fmt + 18, 2);
    if (format != 0x11 || channels < 1 || channels > 2 || block < 4u * channels || per_block < 2) return NULL;
    size_t blocks = (data_n + block - 1) / block, frames = blocks * per_block;
    if (total && total < frames) frames = total;
    size_t bytes = frames * channels * 2;
    unsigned char *out = (unsigned char *)calloc(1, 44 + bytes);
    if (!out) return NULL;
    int16_t *pcm = (int16_t *)(out + 44);
    for (size_t b = 0; b < blocks; b++) {
        const unsigned char *p = data + b * block;
        size_t have = data_n - b * block < block ? data_n - b * block : block;
        if (have < 4u * channels) break;
        for (int c = 0; c < channels; c++) {
            const unsigned char *h = p + 4 * c;
            int value = (int16_t)(h[0] | h[1] << 8), index = h[2] > 88 ? 88 : h[2];
            size_t frame = b * per_block, got = 0;
            if (frame < frames) pcm[frame * channels + c] = (int16_t)(value * gain);
            frame++;
            // eight samples of a channel to four bytes, the channels in turn
            for (size_t at = 4u * channels + 4u * c; at + 4 <= have && got + 1 < per_block; at += 4u * channels)
                for (int k = 0; k < 8 && got + 1 < per_block; k++, got++, frame++) {
                    int code = p[at + k / 2] >> (k & 1) * 4 & 15, step = STEP[index], diff = step >> 3;
                    if (code & 4) diff += step;
                    if (code & 2) diff += step >> 1;
                    if (code & 1) diff += step >> 2;
                    value += code & 8 ? -diff : diff;
                    value = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
                    index += MOVE[code & 7];
                    index = index < 0 ? 0 : index > 88 ? 88 : index;
                    if (frame < frames) pcm[frame * channels + c] = (int16_t)(value * gain);
                }
        }
    }
    uint32_t riff = (uint32_t)(36 + bytes), size16 = 16, byte_rate = rate * channels * 2, data_size = (uint32_t)bytes;
    uint16_t plain = 1, align = (uint16_t)(channels * 2), bits = 16;
    memcpy(out, "RIFF", 4); memcpy(out + 4, &riff, 4); memcpy(out + 8, "WAVEfmt ", 8); memcpy(out + 16, &size16, 4);
    memcpy(out + 20, &plain, 2); memcpy(out + 22, &channels, 2); memcpy(out + 24, &rate, 4); memcpy(out + 28, &byte_rate, 4);
    memcpy(out + 32, &align, 2); memcpy(out + 34, &bits, 2); memcpy(out + 36, "data", 4); memcpy(out + 40, &data_size, 4);
    return out;
}
