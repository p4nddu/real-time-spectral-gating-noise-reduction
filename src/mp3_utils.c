#include "mp3_utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// for decoding
#include <mad.h>

// for encoding
#include <lame/lame.h>

static inline float mad_fixed_to_float(mad_fixed_t fixed) {
  const double scale = 1.0 / (double)(1 << MAD_F_FRACBITS);
  return (float)(fixed * scale);
}

long mp3_to_float(const char* filename, float** output, int* sample_rate,
                  int* channels) {
  if (!filename || !output || !sample_rate || !channels) {
    perror("invalid args to mp3_to_float");
    return -1;
  }

  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    perror("failed to open file");
    return -1;
  }

  // read file into a buffer
  fseek(fp, 0, SEEK_END);
  long filesize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  unsigned char* mp3_buffer = (unsigned char*)malloc(filesize);
  if (!mp3_buffer) {
    perror("unable to allocate memory for mp3 buffer");
    fclose(fp);
    return -1;
  }
  fread(mp3_buffer, 1, filesize, fp);
  fclose(fp);

  // libmad stuff
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;

  mad_stream_init(&stream);
  mad_frame_init(&frame);
  mad_synth_init(&synth);

  // something here
  mad_stream_buffer(&stream, mp3_buffer, filesize);
  int sr = 0;  // sample_rate
  int ch = 0;  // channel

  float* decoded_data = NULL;
  long decoded_size = 0;

  // libmad runs with a constant decoding loop
  while (1) {
    if (mad_frame_decode(&frame, &stream) == -1) {
      if (stream.error == MAD_ERROR_BUFLEN) {
        // reached end of file
        break;
      } else if (stream.error == MAD_ERROR_LOSTSYNC) {
        // skip when lost sync
        continue;
      } else if (stream.error) {
        fprintf(stderr, "libmad error: %s\n", mad_stream_errorstr(&stream));
        break;
      }
    }

    if (sr == 0) {  // set the sample rate and channel from frame header
      sr = frame.header.samplerate;
      ch = MAD_NCHANNELS(&frame.header);
    }

    mad_synth_frame(&synth, &frame);

    unsigned int no_samples = synth.pcm.length;
    unsigned int fch = synth.pcm.channels;

    // allocate output buffer
    float* temp = (float*)realloc(
        decoded_data, (decoded_size + (no_samples * fch)) * sizeof(float));

    if (!temp) {
      free(decoded_data);
      decoded_data = NULL;
      break;
    }
    decoded_data = temp;

    // convert from fixed to float
    for (int i = 0; i < no_samples; i++) {
      for (int j = 0; j < fch; j++) {
        decoded_data[decoded_size++] =
            mad_fixed_to_float(synth.pcm.samples[j][i]);
      }
    }
  }
  free(mp3_buffer);

  mad_stream_finish(&stream);
  mad_frame_finish(&frame);
  mad_synth_finish(&synth);

  if (!decoded_data || decoded_size == 0) {
    if (decoded_data) {
      free(decoded_data);
    }
    return -1;
  }

  // set arguments to values read
  *output = decoded_data;
  *sample_rate = sr;
  *channels = ch;

  return decoded_size;
}

int float_to_mp3(const char* filename, const float* input, long num_samples,
                 int sample_rate, int channels) {
  if (!filename || !input || num_samples <= 0 || sample_rate <= 0 ||
      channels <= 0) {
    perror("invalid args to float_to_mp3");
    return -1;
  }

  // initialize lame stuff
  lame_t lame = lame_init();
  if (!lame) {
    perror("failed to initialize lame");
    return -1;
  }

  lame_set_num_channels(lame, channels);
  lame_set_in_samplerate(lame, sample_rate);

  lame_set_brate(lame, 128);
  lame_set_quality(lame, 5);  // 0 is best (slow), 9 is worst (very fast)
  

  // open a new file to save the mp3
  FILE* fp = fopen(filename, "wb");
  if (!fp) {
    perror("error opening output file\n");
    lame_close(lame);
    return -1;
  }

  if (lame_init_params(lame) < 0) {
    fprintf(stderr, "Error: lame_init_params() failed.\n");
    fclose(fp);
    lame_close(lame);
    return -1;
}

  // initialize output buffer. recommended size formula from LAME docs: 1.25 *
  // num_samples + 7200
  int mp3buf_size = (int)(1.25f * num_samples + 7200);
  unsigned char* mp3buf = (unsigned char*)malloc(mp3buf_size);
  if (!mp3buf) {
    perror("error allocating mp3 output buffer\n");
    fclose(fp);
    lame_close(lame);
    return -1;
  }

  // encoding to mp3
  int write_size = 0;
  if (channels == 1) {
    // means we are dealing with MONO audio
    write_size =
        lame_encode_buffer_ieee_float(lame,
                                      input,  // left
                                      NULL,   // right not used
                                      num_samples, mp3buf, mp3buf_size);
  } else if (channels == 2) {
    // dealing with STEREO audio
    float* left = (float*)malloc(num_samples * sizeof(float));
    float* right = (float*)malloc(num_samples * sizeof(float));
    if (!left || !right) {
      fprintf(stderr, "float_to_mp3: channel split alloc failed.\n");
      free(left);
      free(right);
      free(mp3buf);
      fclose(fp);
      lame_close(lame);
      return -1;
    }

    // input is apparently interleaved via libmad so looks like [L, R, L, R...]
    // have to split intoseparate arrays for L and R
    for (int i = 0; i < num_samples; i++) {
      left[i] = input[2 * i];       // L
      right[i] = input[2 * i + 1];  // R
    }

    write_size = lame_encode_buffer_ieee_float(lame, left, right, num_samples,
                                               mp3buf, mp3buf_size);

    free(left);
    free(right);
  } else {
    // just return if not MONO or STEREO
    fprintf(stderr, "float_to_mp3: only supports mono or stereo.\n");
    free(mp3buf);
    fclose(fp);
    lame_close(lame);
    return -1;
  }

  if (write_size < 0) {
    fprintf(stderr, "float_to_mp3: error encoding (%d)\n", write_size);
    free(mp3buf);
    fclose(fp);
    lame_close(lame);
    return -1;
  }

  // write the encoded mp3 frames to mp3buf
  fwrite(mp3buf, 1, write_size, fp);

  // flush the rest of the frames
  write_size = lame_encode_flush(lame, mp3buf, mp3buf_size);
  if (write_size < 0) {
    fprintf(stderr, "float_to_mp3: flush error\n");
    free(mp3buf);
    fclose(fp);
    lame_close(lame);
    return -1;
  }
  fwrite(mp3buf, 1, write_size, fp);

  // cleanup stuff
  free(mp3buf);
  fclose(fp);
  lame_close(lame);

  return 0;  // success
}