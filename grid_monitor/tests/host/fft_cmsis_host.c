/* Host build of the REAL CMSIS-DSP real FFT, used to cross-check the stand-in FFT. */
#include "arm_math.h"
#include "dsp.h"
static arm_rfft_fast_instance_f32 inst;
void fft_port_init(void) { arm_rfft_fast_init_f32(&inst, FFT_N); }
void fft_port_rfft(float *in, float *out) { arm_rfft_fast_f32(&inst, in, out, 0); }
