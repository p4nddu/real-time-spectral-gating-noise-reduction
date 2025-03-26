#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "noisereduce.h"

// for FFTs
#include "kiss_fft.h"
#include "kiss_fftr.h"

static void make_hann_window(float* window, int length) {
    for (int i = 0; i < length; i++) {
        window[i] = 0.5f - 0.5f * cosf(2.0f * (float)PI * i / (length - 1)); // i just used formula from matlab
    }
}

static float db_to_gain(float db) {
    return powf(10.0f, db / 20.0f);
}

SpectralGateData* spectral_gate_init(const SpectralGateConfig* config) {
    if (!config || config->frame_size <= 0 || config->hop_size <= 0 || config->hop_size > config->frame_size) {
        perror("invalid spectral gate config for init\n");
        return NULL;
    }
    SpectralGateData* spd = (SpectralGateData*)calloc(1, sizeof(SpectralGateData));
    if (!spd) {
        perror("failed to allocate spectral gate data variable\n");
        return NULL;
    }
    spd->config = *config;

    // allocate kissfft shit here
    spd->fwd_cfg = kiss_fftr_alloc(config->frame_size, 0, NULL, NULL);
    spd->inv_cfg = kiss_fftr_alloc(config->frame_size, 1, NULL, NULL);
    if (!spd->fwd_cfg || !spd->inv_cfg) {
        perror("failed to alloc fft configs in init\n");
        kiss_fftr_free(spd->fwd_cfg);
        kiss_fftr_free(spd->inv_cfg);
        free(spd);
        return NULL;
    }

    spd->window = (float*)malloc(config->frame_size * sizeof(float));
    spd->noise_est = (float*)calloc((config->frame_size / 2) + 1, sizeof(float));
    spd->overlap = (float*)calloc(config->frame_size, sizeof(float));
    if (!spd->window || !spd->noise_est || !spd->overlap) {
        perror("failed to alloc spd members in init\n");
        free(spd->window);
        free(spd->noise_est);
        free(spd->overlap);
        kiss_fftr_free(spd->fwd_cfg);
        kiss_fftr_free(spd->inv_cfg);
        free(spd);
        return NULL;
    }

    memset(spd->overlap, 0, sizeof(float) * config->frame_size);

    make_hann_window(spd->window, config->frame_size);

    for (int i = 0; i < (config->frame_size / 2) + 1; i++) {
        spd->noise_est[i] = 1e-3f;  // use as baseline
    };

    spd->initialized = 1;
    return spd;
}

void spectral_gate_free(SpectralGateData* spd) {
    if (!spd) return;
    if (spd->window) free(spd->window);
    if (spd->noise_est) free(spd->noise_est);
    if (spd->overlap) free(spd->overlap);

    if (spd->fwd_cfg) kiss_fftr_free(spd->fwd_cfg);
    if(spd->inv_cfg) kiss_fftr_free(spd->inv_cfg);

    free(spd);
}

int spectral_gate_start(SpectralGateData* spd, const float* input, float* output, long num_samples) {
    if (!spd || !spd->initialized || !input || !output) {
        perror("spectral gate data invalid\n");
        return -1;
    }

    int frame_size = spd->config.frame_size;
    int hop_size = spd->config.hop_size;
    float alpha = spd->config.alpha;
    float noise_floor_gain = db_to_gain(spd->config.noise_floor);
    float noise_decay = spd->config.noise_decay;
    float silence_threshold = spd->config.silence_threshold;

    // VAD and smoothing parameters
    const float vad_threshold_high = silence_threshold * 1.5f;  // Speech threshold
    const float vad_threshold_low = silence_threshold * 0.75f;  // Silence threshold
    const float vad_smoothing = 0.9f;  // Energy smoothing factor
    float smoothed_energy = 0.0f;
    int is_silence = 1;

    memset(output, 0, sizeof(float) * num_samples);

    // allocating FFT buffers
    kiss_fft_scalar* in_buf = (kiss_fft_scalar*)malloc(frame_size * sizeof(kiss_fft_scalar));
    kiss_fft_cpx* freq_bins = (kiss_fft_cpx*)malloc((frame_size / 2 + 1) * sizeof(kiss_fft_cpx));
    kiss_fft_cpx* out_freq_bins = (kiss_fft_cpx*)malloc((frame_size / 2 + 1) * sizeof(kiss_fft_cpx));
    float* time_buf = (float*)malloc(frame_size * sizeof(float));

    if (!in_buf || !freq_bins || !out_freq_bins || !time_buf) {
        fprintf(stderr, "spectral_gate_process: out of memory for temp buffers\n");
        free(in_buf);
        free(freq_bins);
        free(out_freq_bins);
        free(time_buf);
        return -1;
    }

    long pos = 0;
    while (pos < num_samples) {  // Handle partial frames
        int current_frame_size = frame_size;
        if (pos + frame_size > num_samples) {
            current_frame_size = num_samples - pos;
        }

        // window the audio signal and calculate frame energy for VAD
        float frame_energy = 0.0f;
        memset(in_buf, 0, frame_size * sizeof(kiss_fft_scalar));
        for (int i = 0; i < current_frame_size; i++) {
            in_buf[i] = (kiss_fft_scalar)(input[pos + i] * spd->window[i]);
            frame_energy += in_buf[i] * in_buf[i];
        }
        frame_energy /= current_frame_size;  // Normalize

        // update smoothed energy with exponential moving average
        smoothed_energy = vad_smoothing * smoothed_energy + (1 - vad_smoothing) * frame_energy;

        if (is_silence) {
            if (smoothed_energy > vad_threshold_high) {
                is_silence = 0;
            }
        } else {
            if (smoothed_energy < vad_threshold_low) {
                is_silence = 1;
            }
        }

        // forward fft (real to complex)
        kiss_fftr(spd->fwd_cfg, in_buf, freq_bins);

        // fill in frequency bins
        for (int j = 0; j < (frame_size / 2 + 1); j++) {
            float re = freq_bins[j].r;
            float im = freq_bins[j].i;
            float mag = sqrtf(re*re + im*im);
            
            // if frame is silent, update noise estimates
            if (is_silence) {
                spd->noise_est[j] = noise_decay * spd->noise_est[j] + (1.0f - noise_decay) * mag;
            }

            // noise gating
            float threshold = alpha * spd->noise_est[j];
            if (mag < threshold) {
                mag *= noise_floor_gain;
            }

            // making sure phase is preserved - takes up cycles might have to change when using the MCU
            float phase = atan2f(im, re);
            out_freq_bins[j].r = mag * cosf(phase);
            out_freq_bins[j].i = mag * sinf(phase);
        }

        // inverse fft (complex to real)
        kiss_fftri(spd->inv_cfg, out_freq_bins, time_buf);

        // overlap add for smoother transition between frames
        const float scale = 1.0f / frame_size;  // FFT scaling
        for (int i = 0; i < frame_size; i++) {
            float val = time_buf[i] * scale * spd->window[i];
            if (pos + i < num_samples) {
                output[pos + i] += val + spd->overlap[i];
            }
        }

        // update overlap buffer
        int overlap_size = frame_size - hop_size;
        for (int i = 0; i < overlap_size; i++) {
            spd->overlap[i] = time_buf[i + hop_size] * scale * spd->window[i + hop_size];
        }
        // clear remaining overlap in buffer
        for (int i = overlap_size; i < frame_size; i++) {
            spd->overlap[i] = 0.0f;
        }

        pos += hop_size;
    }

    free(in_buf);
    free(freq_bins);
    free(out_freq_bins);
    free(time_buf);

    return 0;
}