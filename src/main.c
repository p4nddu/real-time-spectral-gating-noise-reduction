#include <stdio.h>
#include <stdlib.h>

#include "mp3_utils.h"
#include "noisereduce.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <input.mp3> <output.mp3>\n", argv[0]);
    return 1;
  }

  const char *input_mp3 = argv[1];
  const char *output_mp3 = argv[2];

  float *pcm_data = NULL;
  int sample_rate = 0;
  int channels = 0;

  // decode mp3 file
  long total_samples =
      mp3_to_float(input_mp3, &pcm_data, &sample_rate, &channels);
  if (total_samples <= 0) {
    fprintf(stderr, "failed to decode mp3 file\n");
    return 1;
  }
  printf("decoded %ld samples, sample rate = %d, channels = %d\n",
         total_samples, sample_rate, channels);

  // noise reduce

  SpectralGateConfig config;
  config.frame_size = 1024;  // must be a power of 2
  config.hop_size = 256;     // 50% overlap for proper COLA with a Hann window
  config.alpha = 1.5f;       // threshold scaling factor
  config.noise_floor = -30.0f;  // noise floor in dB
  config.noise_decay = 0.98f;    // noise estimation decay factor
  config.silence_threshold = 0.01f;

  // Initialize the spectral gate data structure for noise reduction.
  SpectralGateData *spd = spectral_gate_init(&config);
  if (!spd) {
    fprintf(stderr, "failed to initialize spectral gate\n");
    free(pcm_data);
    return 1;
  }

  // Allocate a buffer for the processed (noise reduced) audio.
  float *processed_data = (float *)calloc(total_samples, sizeof(float));
  if (!processed_data) {
    fprintf(stderr, "failed to allocate processed data buffer\n");
    spectral_gate_free(spd);
    free(pcm_data);
    return 1;
  }

  // If audio is mono, process directly. If multi-channel, process each channel
  // separately.
  if (channels == 1) {
    if (spectral_gate_start(spd, pcm_data, processed_data, total_samples) !=
        0) {
      fprintf(stderr, "noise reduction processing failed\n");
      free(processed_data);
      spectral_gate_free(spd);
      free(pcm_data);
      return 1;
    }
  } else {
    // Calculate samples per channel (assume pcm_data is interleaved).
    int samples_per_channel = total_samples / channels;
    for (int ch = 0; ch < channels; ch++) {
      // Allocate temporary buffers for the individual channel.
      float *channel_in = (float *)malloc(samples_per_channel * sizeof(float));
      float *channel_out = (float *)calloc(samples_per_channel, sizeof(float));
      if (!channel_in || !channel_out) {
        fprintf(stderr, "failed to allocate buffers for channel %d\n", ch);
        free(channel_in);
        free(channel_out);
        spectral_gate_free(spd);
        free(processed_data);
        free(pcm_data);
        return 1;
      }
      // Deinterleave: extract the channel data.
      for (int i = 0; i < samples_per_channel; i++) {
        channel_in[i] = pcm_data[i * channels + ch];
      }
      // Process noise reduction for this channel.
      if (spectral_gate_start(spd, channel_in, channel_out,
                              samples_per_channel) != 0) {
        fprintf(stderr, "noise reduction processing failed on channel %d\n",
                ch);
        free(channel_in);
        free(channel_out);
        spectral_gate_free(spd);
        free(processed_data);
        free(pcm_data);
        return 1;
      }
      // Reinterleave: write the processed data back into the output buffer.
      for (int i = 0; i < samples_per_channel; i++) {
        processed_data[i * channels + ch] = channel_out[i];
      }
      free(channel_in);
      free(channel_out);
    }
  }
  printf("noise reduced!\n");

  // Cleanup noise reduction data structure.
  spectral_gate_free(spd);

  // encode mp3 file
  if (float_to_mp3(output_mp3, processed_data, total_samples, sample_rate,
                   channels) != 0) {
    fprintf(stderr, "failed to encode mp3 file\n");
    free(pcm_data);
    return 1;
  }

  printf("encoding successful!\n");

  // Cleanup
  free(pcm_data);
  return 0;
}