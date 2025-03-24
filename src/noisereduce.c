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
    // float noise_decay = spd->config.noise_decay;
    float silence_threshold = spd->config.silence_threshold;

    memset(output, 0, sizeof(float) * num_samples);

    // initialize FFT buffers here
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

    // short-time fourier transform loop
    long pos = 0;
    while (pos + frame_size <= num_samples) {
        // compute average energy for the frame for VAD, issilent = 1 when frame is below silent threshold
        float frame_energy = 0.0f;
        int is_silence = 0;

        for (int i = 0; i < frame_size; i++) {
            frame_energy += input[pos + i] * input[pos + i];
        }
        frame_energy /= frame_size;

        float gamma = 0.2f; // silence threshold updating smoothing factor
        if (frame_energy < spd->config.silence_threshold) {
            spd->config.silence_threshold = (1.0f - gamma) * spd->config.silence_threshold + gamma * frame_energy;
        }

        if(frame_energy < spd->config.silence_threshold) is_silence = 1;

        // copy the hann windows into in_buf
        for (int i = 0; i < frame_size; i++) {
            in_buf[i] = (kiss_fft_scalar)(input[pos + i] * spd->window[i]);
        }

        // forward fft (real to complex)
        kiss_fftr(spd->fwd_cfg, in_buf, freq_bins);

        // weiner filter soft thresholding and minima tracking
        for (int j = 0; j < (frame_size / 2 + 1); j++) {
            float re = freq_bins[j].r;
            float im = freq_bins[j].i;
            float mag2 = re*re + im*im; // square rooting is taxing on cycles

            if (is_silence) {
                // good to update noise estimate 
                float noise_est_squared = spd->noise_est[j] * spd->noise_est[j];
        
                if (mag2 < noise_est_squared) {
                    spd->noise_est[j] = sqrtf(mag2);  // update noise estimate (linear magnitude)
                }
            }

            // weiner gain calcualtion. take into account SNR for smoother noise reduction
            float noise2 = spd->noise_est[j] * spd->noise_est[j];
            float SNR = (mag2 - noise2) / (noise2 + 1e-10f);
            if (SNR < 0) SNR = 0;

            float gain = SNR / (1.0f + SNR);
            gain = fmaxf(gain, noise_floor_gain);

            out_freq_bins[j].r = freq_bins[j].r * gain;
            out_freq_bins[j].i = freq_bins[j].i * gain;

        }

        // inverse fft (complex to real)
        kiss_fftri(spd->inv_cfg, out_freq_bins, time_buf);

        // re window the time_buf for synthesis and overlap
        for (int i = 0; i < frame_size; i++) {
            float val = (time_buf[i] / frame_size) * spd->window[i];
            float outval = val + spd->overlap[i];

            output[pos + i] += outval;

            spd->overlap[i] = 0.0f; // just do overlap of zero right neow
        }

        int shift = frame_size - hop_size;
        for (int i = 0; i < shift; i++) {
            spd->overlap[i] = (time_buf[i + hop_size] / frame_size) * spd->window[i + hop_size];
        }
        for (int i = shift; i < frame_size; i++) {
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