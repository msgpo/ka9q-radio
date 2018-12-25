// $Id: linear.c,v 1.41 2018/12/20 05:25:42 karn Exp karn $

// General purpose linear demodulator
// Handles USB/IQ/CW/etc, basically all modes but FM and envelope-detected AM
// Copyright Sept 20 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <pthread.h>
#include <string.h>

#include "misc.h"
#include "dsp.h"
#include "filter.h"
#include "radio.h"


void *demod_linear(void *arg){
  pthread_setname("linear");
  assert(arg != NULL);
  struct demod * const demod = arg;

  demod->opt.loop_bw = 1; // eventually to be set from mode table

  // Set derived (and other) constants
  float const samptime = (float)demod->filter.decimate / (float)demod->input.samprate;  // Time between (decimated) samples
  float const blocktime = samptime * demod->filter.L; // Update rate of fine PLL (once/block)

  // AGC
  int hangcount = 0;

  // Coherent mode parameters
  float const snrthreshdb = 3;     // Loop lock threshold at +3 dB SNR
  int   const fftsize = 1 << 16;   // search FFT bin size = 64K = 1.37 sec @ 48 kHz
  float const damping = M_SQRT1_2; // PLL loop damping factor; 1/sqrt(2) is "critical" damping
  float const lock_time = 1;       // hysteresis parameter: 2*locktime seconds of good signal -> lock, 2*locktime sec of bad signal -> unlock
  int   const fft_enable = 1;

  // FFT search params
  float const snrthresh = powf(10,snrthreshdb/10);          // SNR threshold for lock
  int   const lock_limit = round(lock_time / samptime);     // Stop sweeping after locked for this amount of time
  float const binsize = 1. / (fftsize * samptime);          // FFT bin size, Hz
  // FFT bin indices for search limits. Squaring doubles frequency, so double the search range
  float const searchhigh = 300;    // FFT search limits, in Hz
  float const searchlow =  -300;
  int   const lowlimit =  round(searchlow / binsize);
  int   const highlimit = round(searchhigh / binsize);

  // Second-order PLL loop filter (see Gardner)
  float const vcogain = 2*M_PI;                            // 1 Hz = 2pi radians/sec per "volt"
  float const pdgain = 1;                                  // phase detector gain "volts" per radian (unity from atan2)
  float const natfreq = demod->opt.loop_bw * 2*M_PI;       // loop natural frequency in rad/sec
  float const tau1 = vcogain * pdgain / (natfreq*natfreq); // 1 / 2pi
  float const integrator_gain = 1 / tau1;                  // 2pi
  float const tau2 = 2 * damping / natfreq;                // sqrt(2) / 2pi = 1/ (pi*sqrt(2))
  float const prop_gain = tau2 / tau1;                     // sqrt(2)/2
  //  float const ramprate = demod->opt.loop_bw * blocktime / integrator_gain;   // sweep at one loop bw/sec
  float const ramprate = 0; // temp disable


#if 0
  // DC removal from envelope-detected AM and coherent AM
  complex float DC_filter = 0;
  float const DC_filter_coeff = .0001;
#endif

  demod->sig.snr = 0;

  // Detection filter
  struct filter_out * const filter = demod->filter.out;

  // Carrier search FFT
  complex float * fftinbuf = NULL;
  complex float *fftoutbuf = NULL;
  fftwf_plan fft_plan = NULL;
  int fft_ptr = 0;  

  if(fft_enable){
    fftinbuf = fftwf_alloc_complex(fftsize);
    fftoutbuf = fftwf_alloc_complex(fftsize);  
    fft_plan = fftwf_plan_dft_1d(fftsize,fftinbuf,fftoutbuf,FFTW_FORWARD,FFTW_ESTIMATE);
  }

  // PLL oscillator is in two parts, coarse and fine, so that small angle approximations
  // can be used to rapidly tweak the frequency by small amounts
  struct osc fine;
  memset(&fine,0,sizeof(fine));
  fine.phasor = 1;
  set_osc(&fine, 0.0, 0.0);

  struct osc coarse;                    // FFT-controlled offset LO
  memset(&coarse,0,sizeof(coarse));
  coarse.phasor = 1;
  set_osc(&coarse,0.0, 0.0);            // 0 Hz to start
  
  float integrator = 0;                 // 2nd order loop integrator
  float delta_f = 0;                    // FFT-derived offset
  float ramp = 0;                       // Frequency sweep (do we still need this?)
  int lock_count = 0;

  int fft_samples = 0;                  // FFT input samples since last transform

  while(1){
    // Are we active?
    pthread_mutex_lock(&demod->demod_mutex);
    while(demod->demod_type != LINEAR_DEMOD)
      pthread_cond_wait(&demod->demod_cond,&demod->demod_mutex);
    pthread_mutex_unlock(&demod->demod_mutex);

    // Wait for new samples
    execute_filter_output(filter);    

    // Carrier (or regenerated carrier) tracking in coherent mode
    if(demod->opt.pll){
      // Copy into circular input buffer for FFT in case we need it for acquisition
      if(fft_enable){
	fft_samples += filter->olen;
	if(fft_samples > fftsize)
	  fft_samples = fftsize; // no need to let it go higher
	if(demod->opt.square){
	  // Squaring loop is enabled; square samples to strip BPSK or DSB modulation
	  // and form a carrier component at 2x its actual frequency
	  // This is of course suboptimal for BPSK since there's no matched filter,
	  // but it may be useful in a pinch
	  for(int i=0;i<filter->olen;i++){
	    fftinbuf[fft_ptr++] = filter->output.c[i] * filter->output.c[i];
	    if(fft_ptr >= fftsize)
	      fft_ptr -= fftsize;
	  }
	} else {
	  // No squaring, just analyze the samples directly for a carrier
	  for(int i=0;i<filter->olen;i++){
	    fftinbuf[fft_ptr++] = filter->output.c[i];
	    if(fft_ptr >= fftsize)
	      fft_ptr -= fftsize;
	  }
	}
      }
      // Loop lock detector with hysteresis
      // If the loop is locked, the SNR must fall below the threshold for a while
      // before we declare it unlocked, and vice versa
      if(demod->sig.snr < snrthresh){
	lock_count -= filter->olen;
      } else {
	lock_count += filter->olen;
      }
      if(lock_count >= lock_limit){
	lock_count = lock_limit;
	demod->sig.pll_lock = 1;
      }
      if(lock_count <= -lock_limit){
	lock_count = -lock_limit;
	demod->sig.pll_lock = 0;
      }
      demod->sig.lock_timer = lock_count;

      // If loop is out of lock, reacquire
      if(!demod->sig.pll_lock){
	if(fft_enable && fft_samples > fftsize/2){ // Don't run FFT more often than every half block; it's slow
	  fft_samples = 0;
	  // Run FFT, look for peak bin
	  // Do this every time??
	  fftwf_execute(fft_plan);
	  
	  // Search limited range of FFT buffer for peak energy
	  int maxbin = 0;
	  float maxenergy = 0;
	  int sqterm = demod->opt.square ? 2 : 1;
	  for(int n = sqterm * lowlimit; n <= sqterm * highlimit; n++){
	    float const e = cnrmf(fftoutbuf[n < 0 ? n + fftsize : n]);
	    if(e > maxenergy){
	      maxenergy = e;
	      maxbin = n;
	    }
	  }
	  if(maxenergy > 0){ // Make sure there's signal
	    double new_delta_f = binsize * maxbin;
	    if(demod->opt.square)
	      new_delta_f /= 2; // Squaring loop provides 2xf component, so we must divide by 2
	    
	    if(new_delta_f != delta_f){
	      delta_f = new_delta_f;
	      integrator = 0; // reset integrator
	      set_osc(&coarse, -samptime * delta_f, 0.0);
	    }
	  }
	}
	if(ramp == 0) // not already sweeping
	  ramp = ramprate;
      } else { // !pll_lock
	ramp = 0;
      }
      // Apply coarse and fine offsets, estimate SNR, gather DC phase information
      float signal = 0;
      float noise = 0;
      complex float accum = 0;
      assert(coarse.phasor == coarse.phasor && fine.phasor == fine.phasor);
      for(int n=0;n<filter->olen;n++){
	complex float s;
	s = filter->output.c[n] *= step_osc(&coarse) * step_osc(&fine);
	
	float rp = crealf(s) * crealf(s);
	float ip = cimagf(s) * cimagf(s);
	signal += rp;
	noise += ip;
	if(demod->opt.square)
	  accum += s*s;
	else
	  accum += s;
      }
      demod->sig.cphase = cargf(accum);
      if(isnan(demod->sig.cphase))
	demod->sig.cphase = 0;
      if(demod->opt.square)
	demod->sig.cphase /= 2; // Squaring doubles the phase


      // fine PLL on block basis
      // Includes ramp generator for frequency sweeping during acquisition
      float carrier_phase = demod->sig.cphase;

      // Lag-lead (integral plus proportional) 
      integrator += carrier_phase * blocktime + ramp;
      float const feedback = integrator_gain * integrator + prop_gain * carrier_phase; // units of Hz
      assert(!isnan(feedback));
      set_osc(&fine,-feedback * samptime, 0.0);
      
      // Acquisition frequency sweep
      if((feedback >= binsize) && (ramp > 0))
	ramp = -ramprate; // reached upward sweep limit, sweep down
      else if((feedback <= binsize) && (ramp < 0))
	ramp = ramprate;  // Reached downward sweep limit, sweep up
      
      if(isnan(demod->sig.foffset))
	demod->sig.foffset = feedback + delta_f;
      else
	demod->sig.foffset += 0.01 * (feedback + delta_f - demod->sig.foffset);
      if(noise != 0){
	demod->sig.snr = (signal / noise) - 1; // S/N as power ratio; meaningful only in coherent modes
	if(demod->sig.snr < 0)
	  demod->sig.snr = 0; // Clamp to 0 so it'll show as -Inf dB
      } else
	demod->sig.snr = NAN;
    }
    // Demodulation
    float samples[filter->olen]; // for mono output
    float energy = 0;
    float output_level = 0;
    for(int n=0; n<filter->olen; n++){
      complex float s = filter->output.c[n] * step_osc(&demod->shift);
      float norm = cnrmf(s);
      energy += norm;
      float amplitude = sqrtf(norm);
      
      // AGC
      // Lots of people seem to have strong opinions how AGCs should work
      // so there's probably a lot of work to do here
      // The attack_factor feature doesn't seem to work well; if it's at all
      // slow you get an annoying "pumping" effect.
      // But if it's too fast, brief spikes can deafen you for some time
      // What to do?
      if(demod->opt.agc){
	if(isnan(demod->agc.gain) || amplitude * demod->agc.gain > demod->agc.headroom){
	  demod->agc.gain = demod->agc.headroom / amplitude; // Startup
	  hangcount = demod->agc.hangtime;
	} else if(hangcount > 0){
	  hangcount--;
	} else {
	  demod->agc.gain *= demod->agc.recovery_rate;
	}
      }
      if(demod->opt.env){
	// AM envelope detection -- should re-add DC removal
	samples[n] = amplitude * demod->agc.gain;
	output_level += samples[n] * samples[n];
      } else if(demod->output.channels == 1) {
	samples[n] = crealf(s) * demod->agc.gain;
	output_level += samples[n] * samples[n];
      } else {
	filter->output.c[n] = s * demod->agc.gain;
	output_level += cnrmf(filter->output.c[n]);
      }
    }
    demod->output.level = output_level / (filter->olen * demod->output.channels);
    if(demod->opt.env || demod->output.channels == 1)
      send_mono_output(demod,samples,filter->olen);
    else      // I on left, Q on right
      send_stereo_output(demod,(float *)filter->output.c,filter->olen);

    // Total baseband power (I+Q), scaled to each sample
    demod->sig.bb_power = energy / filter->olen;
  }
}
