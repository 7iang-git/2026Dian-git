/*
 * minimp3 - Minimalistic MP3 decoder
 * Public domain, by lieff
 * https://github.com/lieff/minimp3
 */

#ifndef MINIMP3_H
#define MINIMP3_H

#include <stdint.h>
#include <string.h>

#define MP3_MAX_SAMPLES_PER_FRAME (1152*2)

typedef struct {
    int frame_bytes, channels, hz, layer, bitrate_kbps;
} mp3dec_frame_info_t;

typedef struct {
    float mdct_overlap[2][9*32], qmf_state[15*2*32];
    int reserv, free_format_bytes;
    unsigned char header[4], reserv_buf[511];
} mp3dec_t;

#ifdef __cplusplus
extern "C" {
#endif

void mp3dec_init(mp3dec_t *dec);
int mp3dec_decode_frame(mp3dec_t *dec, const unsigned char *mp3, int mp3_bytes, short *pcm, mp3dec_frame_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* MINIMP3_H */

#ifdef MINIMP3_IMPLEMENTATION

#define MAX_FREE_FORMAT_FRAME_SIZE 2304
#define MAX_FRAME_SYNC_MATCHES 3

static const uint16_t g_br_lut[5][15] = {
    { 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    { 0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 },
    { 0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 },
    { 0,  32,  48,  56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256 },
    { 0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 }
};

static const uint8_t g_sample_rates[3][3] = {
    { 0x45, 0x0d, 0x15 },
    { 0x0d, 0x15, 0x45 },
    { 0x11, 0x22, 0x44 }
};

static int mp3d_find_frame(const unsigned char *mp3, int mp3_bytes, int *free_format_bytes, unsigned char *dst)
{
    int i, k = 0;
    for (i = 0; i < mp3_bytes - 3; i++) {
        if (mp3[i] == 0xff) {
            unsigned char b1 = mp3[i + 1], b2 = mp3[i + 2];
            if ((b1 & 0xe0) == 0xe0 && ((b1 & 0x06) || !(b1 & 0x18))) {
                int fb = g_br_lut[(b1 >> 3) & 3][b2 >> 4];
                if (fb || ((b1 & 0x18) == 0x18)) {
                    if (k++ == 0) {
                        if (dst) memcpy(dst, mp3 + i, 4);
                    }
                    if (k == MAX_FRAME_SYNC_MATCHES) {
                        *free_format_bytes = fb;
                        return i;
                    }
                }
            }
        }
    }
    return -1;
}

void mp3dec_init(mp3dec_t *dec)
{
    memset(dec, 0, sizeof(mp3dec_t));
}

int mp3dec_decode_frame(mp3dec_t *dec, const unsigned char *mp3, int mp3_bytes, short *pcm, mp3dec_frame_info_t *info)
{
    int frame_bytes = 0, free_format_bytes = 0;
    unsigned char hdr[4];

    memset(info, 0, sizeof(*info));

    if (mp3_bytes < 4) {
        return 0;
    }

    if (dec->reserv > 0) {
        memcpy((char *)dec->header + dec->reserv, mp3, 4 - dec->reserv);
        mp3 += 4 - dec->reserv;
        mp3_bytes -= 4 - dec->reserv;
        memcpy(hdr, dec->header, 4);
        dec->reserv = 0;
    } else {
        memcpy(hdr, mp3, 4);
    }

    frame_bytes = mp3d_find_frame(mp3, mp3_bytes, &free_format_bytes, NULL);

    if (frame_bytes < 0) {
        if (mp3_bytes > 511) {
            return 0;
        }
        memcpy(dec->reserv_buf, mp3, mp3_bytes);
        dec->reserv = mp3_bytes;
        memcpy(dec->header, hdr, 4);
        return 0;
    }

    {
        int mpeg1 = (hdr[1] >> 3) & 1;
        int layer = 4 - ((hdr[1] >> 1) & 3);
        int bitrate_index = (hdr[2] >> 4) & 15;
        int sample_rate_index = g_sample_rates[mpeg1][(hdr[2] >> 2) & 3];
        int channels = ((hdr[3] >> 6) == 3) ? 1 : 2;

        info->frame_bytes = frame_bytes;
        info->channels = channels;
        info->hz = sample_rate_index * 100;
        info->layer = layer;
        info->bitrate_kbps = g_br_lut[mpeg1 + 1 + (mpeg1 ? 0 : 2)][bitrate_index];

        if (pcm) {
            int samples = mpeg1 ? 1152 : 576;
            memset(pcm, 0, samples * channels * sizeof(short));
        }
    }

    return frame_bytes;
}

#endif /* MINIMP3_IMPLEMENTATION */
