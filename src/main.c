#include <stdio.h>
#include <stdlib.h>
#include "mp3_utils.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.mp3> <output.mp3>\n", argv[0]);
        return 1;
    }
    
    const char *input_mp3  = argv[1];
    const char *output_mp3 = argv[2];

    float *pcm_data = NULL;
    int sample_rate = 0;
    int channels    = 0;
    
    // decode mp3 file
    long total_samples = mp3_to_float(input_mp3, &pcm_data, &sample_rate, &channels);
    if (total_samples <= 0) {
        fprintf(stderr, "failed to decode mp3 file\n");
        return 1;
    }

    // implement noise reduction here

    // noise reduction end

    printf("decoded %ld samples, sample rate = %d, channels = %d\n", total_samples, sample_rate, channels);

    // encode mp3 file
    if (float_to_mp3(output_mp3, pcm_data, total_samples, sample_rate, channels) != 0) {
        fprintf(stderr, "failed to encode mp3 file\n");
        free(pcm_data);
        return 1;
    }

    printf("encoding successful!\n");

    // Cleanup
    free(pcm_data);
    return 0;
}
