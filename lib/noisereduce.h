#ifndef NOISEREDUCE_H
#define NOISEREDUCE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PI 3.14159265359
#include "kiss_fft.h"
#include "kiss_fftr.h"

typedef struct {
    int frame_size;
    int hop_size;
    float alpha; // gating threshold
    float noise_floor; // minimal gain floor
    float noise_decay; // smoothing factor 0-1; (eg. 0.9 = 90% old and 10% new)
    float silence_threshold; //e enrgy threshold to consider frame as silence, if negative, auto calibration used
}SpectralGateConfig;

typedef struct {
    SpectralGateConfig config;
    
    kiss_fftr_cfg fwd_cfg; // real to complex
    kiss_fftr_cfg inv_cfg; // complex to real

    //buffers
    float* window; // hanning window of length frame size
    float* noise_est; // estimated noise floor for each window
    float* overlap; // overlap buffer

    int initialized;
} SpectralGateData;

// noise reduce functions
// spectralgateconfig stores the user parameters for the spectral gating (FFT size, smoothing factor, etc)
// spectralgatecontext is the variable that stores the processed audio (internal buffers, fft results, etc)

static void make_hann_window(float* window, int length);
static float db_to_gain(float db); // convert dB to linear gain for noise floor, etc.
SpectralGateData* spectral_gate_init(const SpectralGateConfig* config); // initialize a spectral gate data variable
void spectral_gate_free(SpectralGateData* spd); // free spectral gate data variable
int spectral_gate_start(SpectralGateData* spd, const float* input, float* output, long num_samples);


#ifdef __cplusplus
}
#endif
#endif