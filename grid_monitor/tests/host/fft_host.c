/* Host-side FFT with the same packed output format as CMSIS arm_rfft_fast_f32. */
#include <math.h>
#include <stdlib.h>
#include "dsp.h"

void fft_port_init(void) {}

void fft_port_rfft(float *in, float *out)
{
	const int N = FFT_N;
	double *re = malloc(sizeof(double) * N), *im = malloc(sizeof(double) * N);

	for (int k = 0; k < N; k++) {
		re[k] = in[k];
		im[k] = 0.0;
	}
	/* bit reversal */
	for (int i = 1, j = 0; i < N; i++) {
		int bit = N >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			double t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= N; len <<= 1) {
		double ang = -2.0 * M_PI / len;

		for (int i = 0; i < N; i += len) {
			for (int k = 0; k < len / 2; k++) {
				double wr = cos(ang * k), wi = sin(ang * k);
				int a = i + k, b = i + k + len / 2;
				double xr = re[b] * wr - im[b] * wi;
				double xi = re[b] * wi + im[b] * wr;

				re[b] = re[a] - xr; im[b] = im[a] - xi;
				re[a] += xr;        im[a] += xi;
			}
		}
	}
	out[0] = (float)re[0];
	out[1] = (float)re[N / 2];
	for (int k = 1; k < N / 2; k++) {
		out[2 * k] = (float)re[k];
		out[2 * k + 1] = (float)im[k];
	}
	free(re);
	free(im);
}
