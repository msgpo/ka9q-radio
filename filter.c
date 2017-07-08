// $Id: filter.c,v 1.12 2017/07/03 23:24:18 karn Exp karn $
// General purpose filter package using fast convolution (overlap-save)
// and the FFTW3 FFT package
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
#define _GNU_SOURCE 1
#include <malloc.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include "dsp.h"
#include "filter.h"

// When decimation is used, we assume the filter response drops to negligible
// well below the decimated (lower) Nyquist rate so we can avoid the extra work of adding in
// the aliased frequency components needed to produce exactly the same result as
// decimating the time-domain output by the same ratio

// Real output filter is faster type that uses c2r IFFTs to discard imaginary component of output
// Useful for SSB and VSB
// NB: Response is always complex,
// with length N_dec = (L + M - 1)/decimate when output is complex
// and (N_dec/2+1)/decimate when output is real

// response[] must be SIMD-aligned (e.g., with fftw_alloc) and will be freed by delete_filter()
struct filter *create_filter(int const L,int const M, complex float * const response,int const decimate,enum filtertype const in_type,enum filtertype const out_type){

  int const N = L + M - 1;
  int const N_dec = N / decimate;

  // Parameter sanity check
  if((N % decimate) != 0)
    fprintf(stderr,"Warning: FFT size %'u is not divisible by decimation ratio %d\n",N,decimate);

  struct filter * const f = calloc(1,sizeof(*f));
  f->in_type = in_type;
  f->out_type = out_type;
  f->decimate = decimate;
  f->ilen = L;
  f->olen = L / decimate;
  f->impulse_length = M;

  switch(in_type){
  default:
    fprintf(stderr,"Filter input type %d, assuming complex\n",in_type); // Note fall-thru
  case COMPLEX:
    f->fdomain = fftwf_alloc_complex(N);
    f->input_buffer.c = fftwf_alloc_complex(N);
    memset(f->input_buffer.c,0,(M-1)*sizeof(*f->input_buffer.c)); // Clear earlier state
    f->input.c = f->input_buffer.c + M - 1;
    f->fwd_plan = fftwf_plan_dft_1d(N,f->input_buffer.c,f->fdomain,FFTW_FORWARD,FFTW_ESTIMATE);
    break;
  case REAL:
    f->fdomain = fftwf_alloc_complex(N/2+1); // Only N/2+1 will be filled in by the r2c FFT
    f->input_buffer.r = fftwf_alloc_real(N);
    memset(f->input_buffer.r,0,(M-1)*sizeof(*f->input_buffer.r)); // Clear earlier state
    f->input.r = f->input_buffer.r + M - 1;
    f->fwd_plan = fftwf_plan_dft_r2c_1d(N,f->input_buffer.r,f->fdomain,FFTW_ESTIMATE);
    break;
  }
  f->response = response;
  if(response != NULL){
    // *response is always complex float, but it's shortened when filter input and output are both real
    if(out_type == REAL || out_type == CROSS_CONJ){
      // Originally for complex input; Check these for real input
      // response[] has length N_dec/2+1 (for real input and output)
      // and length N_dec (for all others).
      if(in_type == REAL && out_type == REAL){
	assert(malloc_usable_size(response) >= (N_dec/2+1) * sizeof(*response));
	int n;
	for(n=0;n<=N_dec/2;n++)
	  f->response[n] *= M_SQRT1_2;
      } else {
	assert(malloc_usable_size(response) >= N_dec * sizeof(*response));
	int n;
	for(n=0;n<N_dec;n++)
	  f->response[n] *= M_SQRT1_2;
      }
    }
  }
  switch(out_type){
  default:
  case COMPLEX:
  case CROSS_CONJ:
    f->f_fdomain = fftwf_alloc_complex(N_dec);
    f->output_buffer.c = fftwf_alloc_complex(N_dec);  
    f->output.c = f->output_buffer.c + (M - 1)/decimate;
    f->rev_plan = fftwf_plan_dft_1d(N_dec,f->f_fdomain,f->output_buffer.c,FFTW_BACKWARD,FFTW_ESTIMATE);
    break;
  case REAL:
    f->f_fdomain = fftwf_alloc_complex(N_dec/2+1);
    f->output_buffer.r = fftwf_alloc_real(N_dec);
    f->output.r = f->output_buffer.r + (M - 1)/decimate;
    f->rev_plan = fftwf_plan_dft_c2r_1d(N_dec,f->f_fdomain,f->output_buffer.r,FFTW_ESTIMATE);
    break;
  }
  return f;
}

int execute_filter(struct filter * const f){
  execute_filter_nocopy(f);
  // Save for next block - non-destructive copy
  switch(f->in_type){
  default:
  case COMPLEX:
    memmove(f->input_buffer.c,f->input_buffer.c + f->ilen,(f->impulse_length - 1)*sizeof(*f->input_buffer.c));
    break;
  case REAL:
    memmove(f->input_buffer.r,f->input_buffer.r + f->ilen,(f->impulse_length - 1)*sizeof(*f->input_buffer.r));
    break;
  }
  return 0;
}


int execute_filter_nocopy(struct filter * const f){
  assert(f != NULL);
  assert(f->out_type != NONE);
  assert(f->in_type != NONE);
  assert(f->response != NULL);
  assert(f->fdomain != NULL);
  assert(f->f_fdomain != NULL);  

  int const N = f->ilen + f->impulse_length - 1; // points in input buffer
  int const N_dec = N / f->decimate;                     // points in (decimated) output buffer

  fftwf_execute(f->fwd_plan);  // Forward transform

  // DC and positive frequencies up to nyquist frequency are same for all types
  assert(malloc_usable_size(f->f_fdomain) >= (N_dec/2+1) * sizeof(*f->f_fdomain));
  assert(malloc_usable_size(f->response) >= (N_dec/2+1) * sizeof(*f->response));
  assert(malloc_usable_size(f->fdomain) >= (N_dec/2+1) * sizeof(*f->fdomain));
  int p;
  for(p=0; p <= N_dec/2; p++)
    f->f_fdomain[p] = f->response[p] * f->fdomain[p];

  if(f->in_type == REAL){
    if(f->out_type != REAL){
      // For a purely real input, F[-f] = conj(F[+f])
      assert(malloc_usable_size(f->f_fdomain) >= N_dec * sizeof(*f->f_fdomain));
      int p,dn;
      for(p=1,dn=N_dec-1; dn > N_dec/2; p++,dn--)
	f->f_fdomain[dn] = f->response[dn] * conjf(f->fdomain[p]);
    } // out_type == REAL already handled
  } else { // in_type == COMPLEX
    if(f->out_type != REAL){
      // Complex output; do negative frequencies
      assert(malloc_usable_size(f->fdomain) >= N * sizeof(*f->fdomain));
      assert(malloc_usable_size(f->response) >= N_dec * sizeof(*f->response));
      assert(malloc_usable_size(f->f_fdomain) >= N_dec * sizeof(*f->f_fdomain));

      int n,dn;
      for(n=N-1,dn=N_dec-1; dn > N_dec/2;n--,dn--)
	f->f_fdomain[dn] = f->response[dn] * f->fdomain[n];
    } else {
      // Real output; fold conjugates of negative frequencies into positive to force pure real result
      assert(malloc_usable_size(f->fdomain) >= N * sizeof(*f->fdomain));
      assert(malloc_usable_size(f->response) >= N_dec * sizeof(*f->response));
      int n,p,dn;
      for(n=N-1,p=1,dn=N_dec-1; p < N_dec/2; p++,n--,dn--)
	f->f_fdomain[p] += conjf(f->response[dn] * f->fdomain[n]);
    }
  }
  if(f->out_type == CROSS_CONJ){
    // hack for ISB; forces negative frequencies onto I, positive onto Q
    assert(malloc_usable_size(f->f_fdomain) >= N_dec * sizeof(*f->f_fdomain));
    int p,dn;
    for(p=1,dn=N_dec-1; p < N_dec/2; p++,dn--){
      complex float const pos = f->f_fdomain[p];
      complex float const neg = f->f_fdomain[dn];
      
      f->f_fdomain[p]  = pos + conjf(neg);
      f->f_fdomain[dn] = neg - conjf(pos);
    }
  }
  
  fftwf_execute(f->rev_plan); // Note: c2r version destroys f_fdomain[]
  return 0;
}

int delete_filter(struct filter * const f){
  if(f != NULL){
    fftwf_destroy_plan(f->fwd_plan);
    fftwf_destroy_plan(f->rev_plan);  
    fftwf_free(f->input_buffer.c);
    fftwf_free(f->output_buffer.c);
    fftwf_free(f->response);
    fftwf_free(f->fdomain);
    fftwf_free(f->f_fdomain);
    free(f);
  }
  return 0;
}

// Window shape factor for Kaiser window
// Transition region is approx sqrt(1+Beta^2)
float Kaiser_beta = 3.0;

// Modified Bessel function of the 0th kind, used by the Kaiser window
static const float i0(float const x){
  const float t = 0.25 * x * x;
  float sum = 1 + t;
  float term = t;
  int k;
  for(k=2;k<40;k++){
    term *= t/(k*k);
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return sum;
}


#if 0 // Available if you ever want them

// Hamming window
const static float hamming(int const n,int const M){
  const float alpha = 25./46;
  const float beta = (1-alpha);

  return alpha - beta * cos(2*M_PI*n/(M-1));
}

// Hann / "Hanning" window
const static float hann(int const n,int const M){
    return 0.5 - 0.5 * cos(2*M_PI*n/(M-1));
}

// Exact Blackman window
const static float blackman(int const n,int const M){
  float const a0 = 7938./18608;
  float const a1 = 9240./18608;
  float const a2 = 1430./18608;
  return a0 - a1*cos(2*M_PI*n/(M-1)) + a2*cos(4*M_PI*n/(M-1));
}

// Jim Kaiser was in my Bellcore department in the 1980s. Wonder whatever happened to him.
// Superseded by make_kaiser() routine that more efficiently computes entire window at once
const static float kaiser(const int n,const int M, const float beta){
  static float old_beta = NAN;
  static float old_inv_denom;

  // Cache old value of beta, since it rarely changes
  if(beta != old_beta){
    old_beta = beta;
    old_inv_denom = 1. / i0(M_PI*beta);
  }
  const float p = 2.0*n/(M-1) - 1;
  return i0(M_PI*beta*sqrtf(1-p*p)) * old_inv_denom;
}
#endif

// Compute an entire Kaiser window
// More efficient than repeatedly calling kaiser(n,M,beta)
int make_kaiser(float *window,const int M,const float beta){
  // Precompute unchanging partial values
  float const numc = M_PI * beta;
  float const inv_denom = 1. / i0(numc); // Inverse of denominator
  float const pc = 2.0 / (M-1);

  // The window is symmetrical, so compute only half of it and mirror
  // this won't compute the middle value in an odd-length sequence
  int n;
  for(n = 0; n < M/2; n++){
    float const p = pc * n  - 1;
    window[M-1-n] = window[n] = i0(numc * sqrtf(1-p*p)) * inv_denom;
  }
  // If sequence length is odd, middle value is unity
  if(M & 1)
    window[(M-1)/2] = 1; // The -1 is actually unnecessary

  return 0;
}


// Apply Kaiser window to filter frequency response
// "response" is SIMD-aligned array of N complex floats
// Impulse response will be limited to first M samples in the time domain
// Phase is adjusted so "time zero" (center of impulse response) is at M/2
// L and M refer to the decimated output
int window_filter(int const L,int const M,complex float * const response,float const beta){
  int const N = L + M - 1;
  assert(malloc_usable_size(response) >= N*sizeof(*response));
  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  complex float * const buffer = fftwf_alloc_complex(N);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD,FFTW_ESTIMATE);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD,FFTW_ESTIMATE);

  // Convert to time domain
  memcpy(buffer,response,N*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
  
  // Shift to beginning of buffer, apply window and scale (N*N)
  float const scale = 1./((float)N*N);  // Integer * integer will overflow if too large
  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);
  int n;
  for(n = M - 1; n >= 0; n--)
    buffer[n] = buffer[(n-M/2+N)%N] * kaiser_window[n] * scale;

  // Pad with zeroes on right side
  memset(buffer+M,0,(N-M)*sizeof(*buffer));

#if 0
  fprintf(stderr,"Filter impulse response, shifted, windowed and zero padded\n");
  for(n=0;n< N;n++)
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);

#if 0
  fprintf(stderr,"Filter response amplitude\n");
  for(n=0;n<N;n++){
    float f = n*192000./N;
    fprintf(stderr,"%.1f %.1f\n",f,power2dB(cnrmf(buffer[n])));
  }
  fprintf(stderr,"\n");
#endif
  memcpy(response,buffer,N*sizeof(*response));
  fftwf_free(buffer);
  return 0;
}
// Real-only counterpart to window_filter()
// response[] is only N/2+1 elements containing DC and positive frequencies only
// Negative frequencies are inplicitly the conjugate of the positive frequencies
// L and M refer to the decimated output
int window_rfilter(const int L,const int M,complex float * const response,const float beta){
  int const N = L + M - 1;
  assert(malloc_usable_size(response) >= (N/2+1)*sizeof(*response));
  complex float * const buffer = fftwf_alloc_complex(N/2 + 1); // plan destroys its input
  float * const timebuf = fftwf_alloc_real(N);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_c2r_1d(N,buffer,timebuf,FFTW_ESTIMATE);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_r2c_1d(N,timebuf,buffer,FFTW_ESTIMATE);
  
  // Convert to time domain
  memcpy(buffer,response,(N/2+1)*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);

  // Shift to beginning of buffer, apply window and scale (N*N)
  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);
  float const scale = 1./((float)N*N); // Integer will overflow if too large
  int n;
  for(n = M - 1; n >= 0; n--)
    timebuf[n] = timebuf[(n-M/2+N)%N] * kaiser_window[n] * scale;
  
  // Pad with zeroes on right side
  memset(timebuf+M,0,(N-M)*sizeof(*timebuf));
#if 0
  printf("Filter impulse response, shifted, windowed and zero padded\n");
  for(n=0;n< M;n++)
    printf("%d %lg\n",n,timebuf[n]);
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fftwf_free(timebuf);
#if 0
  printf("Filter frequency response\n");
  for(n=0; n < N/2 + 1; n++)
    printf("%d %g %g (%.1f dB)\n",n,crealf(buffer[n]),cimagf(buffer[n]),
	   power2dB(cnrmf(buffer[n])));
#endif
  memcpy(response,buffer,(N/2+1)*sizeof(*response));
  fftwf_free(buffer);
  return 0;
}
