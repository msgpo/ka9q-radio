// $Id: fm.c,v 1.69 2019/01/08 06:20:31 karn Exp karn $
// FM demodulation and squelch
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#undef I

#include "misc.h"
#include "dsp.h"
#include "filter.h"
#include "radio.h"

// FM demodulator thread
void *demod_fm(void *arg){
  pthread_setname("fm");
  assert(arg != NULL);
  struct demod * const demod = arg;  

  complex float state = 0;
  float const dsamprate = (float)demod->input.samprate / demod->filter.decimate; // Decimated (output) sample rate
  demod->sig.pdeviation = 0;
  demod->sig.foffset = 0;
  demod->output.channels = 1; // Only mono for now
  struct filter_out * const filter = demod->filter.out;
  float lastaudio = 0; // state for impulse noise removal
  int squelch_open = 0; // Number of blocks for which squelch remains open

  while(1){
    // Are we active?
    pthread_mutex_lock(&demod->demod_mutex);
    while(demod->demod_type != FM_DEMOD)
      pthread_cond_wait(&demod->demod_cond,&demod->demod_mutex);
    pthread_mutex_unlock(&demod->demod_mutex);

    // Wait for next block of frequency domain data
    execute_filter_output(filter,0);

    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    float const gain = (demod->agc.headroom *  M_1_PI * dsamprate) / fabsf(demod->filter.low - demod->filter.high);

    
    // Find average amplitude and variance for SNR estimation
    // Use two passes to avoid possible numerical problems
    float amplitudes[filter->olen];
    demod->sig.bb_power = 0;
    float avg_amp = 0;
    for(int n=0; n < filter->olen; n++){
      float t = cnrmf(filter->output.c[n]);
      demod->sig.bb_power += t;
      avg_amp += amplitudes[n] = sqrtf(t);
    }
    demod->sig.bb_power /= filter->olen;
    avg_amp /= filter->olen;

    float fm_variance = 0;
    for(int n=0; n < filter->olen; n++)
      fm_variance += (amplitudes[n] - avg_amp) * (amplitudes[n] - avg_amp);

    fm_variance /= (filter->olen - 1);

    // Magnitude has a chi-squared distribution with 2 degrees of freedom, so the mean noise amplitude is half the variance
    // This is only approximate, the proper formula is much more complicated
    demod->sig.snr = avg_amp*avg_amp/(2*fm_variance) - 1;
    demod->sig.snr = max(0.0f,demod->sig.snr); // Smoothed values can be a little inconsistent

    float samples[filter->olen];    // Demodulated FM samples
    float const thresh = 4; // +6dB?
    if(demod->sig.snr > thresh)
      squelch_open = 2; // sets length of squelch tail in blocks (2 = 40 ms)

    if(squelch_open > 0){ // Squelch is (still) open
      squelch_open--;

      // Actual FM demodulation
      float pdev_pos = 0;
      float pdev_neg = 0;
      float avg_f = 0;

      for(int n=0; n < filter->olen; n++){
	complex float p = filter->output.c[n] * conjf(state);
	state = filter->output.c[n];

	float ang = cargf(p);
	// Experimental threshold reduction per Fred Harris (if I understood him)
	float amp = cabsf(p) / fm_variance;
	if(amp > 1)
	  amp = 1;


	avg_f += ang; // Direct FM for frequency measurement
	if(n == 0)
	  pdev_pos = pdev_neg = ang;
	else if(ang > pdev_pos)
	  pdev_pos = ang;
	else if(lastaudio < pdev_neg)
	  pdev_neg = ang;

	if(demod->opt.flat){
	  samples[n] = ang * gain; // Straight FM, no threshold extension
	} else {
	  // Integrate to turn FM to PM; de-emphasis -6dB/octave, -20dB/decade.
	  // 0.135 factor is empirical; gives -15 dB audio with 400 Hz modulation and 5 kHz deviation in 16 kHz BW, 26 dB SNR
	  // 400 Hz at +-5 kHz gives -22.5 dB in FLAT mode
	  samples[n] = lastaudio += ang * .114 * gain * amp;
	  lastaudio *= 0.99376949; // 1/e at 300 Hz - who knows what the actual "standard" is??
	}
      }
      avg_f /= filter->olen;  // Average FM output is freq offset
      // Update frequency offset and peak deviation
      demod->sig.foffset = dsamprate  * avg_f * M_1_2PI;
      
      // Remove frequency offset from deviation peaks and scale
      pdev_pos -= avg_f;
      pdev_neg -= avg_f;
      demod->sig.pdeviation = dsamprate * max(pdev_pos,-pdev_neg) * M_1_2PI;
    } else {
      state = 0;
      for(int n = 0; n < filter->olen; n++){
	if(demod->opt.flat)
	  samples[n] = 0;
	else {
	  // Continue to decay audio to prevent thump when squelch closes
	  samples[n] = lastaudio;
	  lastaudio *= 0.99376949; // 1/e at 300 Hz
	}
      }
    }

    float output_level = 0;
    for(int n=0; n < filter->olen; n++)
	output_level += samples[n] * samples[n];
    demod->output.level = output_level / filter->olen;
    send_mono_output(demod,samples,filter->olen);
  }
}
