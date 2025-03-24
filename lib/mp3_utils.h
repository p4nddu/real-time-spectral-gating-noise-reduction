#ifndef MP3_UTILS_H
#define MP3_UTILS_H
#endif

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <lame/lame.h>
#include <mad.h>

// decodes mp3 files to a float buffer
// returns number of samples decoded on success and -1 on error
// channels: 1 (mono) or 2 (stereo)
static inline float mad_fixed_to_float(mad_fixed_t fixed);

long mp3_to_float(const char* mp3_filename, float** output, int* sample_rate,
                  int* channels);

// encodes a float buffer into an mp3 then write to mp3 file
// returns 0 on success and -1 on error
int float_to_mp3(const char* filename, const float* input, long num_samples,
                 int sample_rate, int channels);

#ifdef __cplusplus
}
#endif