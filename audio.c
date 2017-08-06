// $Id: audio.c,v 1.36 2017/08/05 18:53:42 karn Exp karn $
// Audio multicast routines for KA9Q SDR receiver
// Handles linear 16-bit PCM, mono and stereo, and the Opus lossy codec
// Copyright 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <complex.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <complex.h>
#undef I
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <opus/opus.h>

#include "rtp.h"
#include "dsp.h"
#include "audio.h"
#include "multicast.h"

#define PCM_BUFSIZE 512        // 16-bit word count; must fit in Ethernet MTU


static short const scaleclip(float x){
  if(x >= 1.0)
    return SHRT_MAX;
  else if(x <= -1.0)
    return SHRT_MIN;
  return (short)(SHRT_MAX * x);
}
  
// Send 'size' stereo samples, each in a complex float
int send_stereo_audio(struct audio *audio,complex float const *buffer,int size){
  if(audio->opus_bitrate != 0)
    write(audio->opus_stereo_write_fd,buffer,size * sizeof(*buffer));
  else
    write(audio->pcm_stereo_write_fd,buffer,size * sizeof(*buffer));    
  return 0;
}

// Send 'size' mono samples, each in a float
int send_mono_audio(struct audio *audio,float const *buffer,int size){
  if(audio->opus_bitrate != 0){
    // Send to opus encoder as stereo with duplicate channels
    complex float obuf[size];
    int i;
    for(i=0;i<size;i++)
      obuf[i] = CMPLXF(buffer[i],buffer[i]);

    write(audio->opus_stereo_write_fd,obuf,sizeof(obuf));
  } else
    write(audio->pcm_mono_write_fd,buffer,size * sizeof(*buffer));    
  return 0;
}

// Thread for compressing and multicasting audio with Opus codec
void *stereo_opus_audio(void *arg){
  pthread_setname("opus");
  assert(arg != NULL);
  struct audio *audio = arg;
  uint32_t timestamp = 0;
  uint16_t seq = 0;
  time_t tt = time(NULL);
  uint32_t const ssrc = tt & 0xffffffff;

  // Must correspond to 2.5, 5, 10, 20, 40, 60 ms
  // i.e., 120, 240, 480, 960, 1920, 2880 samples @ 48 kHz
  // opus 1.2 also supports 80, 100 and 120 ms
  if(audio->opus_blocktime != 2.5 && audio->opus_blocktime != 5
     && audio->opus_blocktime != 10 && audio->opus_blocktime != 20
     && audio->opus_blocktime != 40 && audio->opus_blocktime != 60
     && audio->opus_blocktime != 80 && audio->opus_blocktime != 100
     && audio->opus_blocktime != 120){
    fprintf(stderr,"opus block time must be 2.5/5/10/20/40/60/80/100/120 ms\n");
    fprintf(stderr,"80/100/120 supported only on opus 1.2 and later\n");
    return NULL;
  }
  int const opus_blocksize = round(audio->opus_blocktime * audio->samprate / 1000.);
  int error;
  audio->opus = opus_encoder_create(audio->samprate,2,OPUS_APPLICATION_AUDIO,&error);
  if(audio->opus == NULL){
    fprintf(stderr,"opus_encoder_create failed, error %d\n",error);
    return NULL;
  }
  opus_encoder_ctl(audio->opus,OPUS_SET_BITRATE(audio->opus_bitrate));
  opus_encoder_ctl(audio->opus,OPUS_SET_DTX(audio->opus_dtx));
  opus_encoder_ctl(audio->opus,OPUS_SET_LSB_DEPTH(16));

  struct rtp_header rtp;
  rtp.vpxcc = (RTP_VERS << 6); // Version 2, padding = 0, extension = 0, csrc count = 0
  rtp.ssrc = htonl(ssrc);
  rtp.mpt = 20;         // arbitrary choice

  struct iovec iovec[2];
  unsigned char data[2048]; // Larger than Ethernet MTU
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = data;
  
  struct msghdr message;
  message.msg_name = NULL; // Set by connect() call in setup_output()
  message.msg_namelen = 0;
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;

  float blocktime = (float)opus_blocksize / audio->samprate;
  float decay = exp(-blocktime);

  complex float opusbuf[sizeof(complex float) * opus_blocksize];
  while(pipefill(audio->opus_stereo_read_fd,opusbuf,sizeof(*opusbuf) * opus_blocksize) > 0){
    // Encoder accepts stereo, which we represent as complex, but it wants an array of floats
    int const dlen = opus_encode_float(audio->opus,(float *)opusbuf,opus_blocksize,data,sizeof(data));
    if(dlen < 0){
      fprintf(stderr,"opus encode error %d\n",dlen);
      continue;
    }
    // Don't transmit if discontinuous mode is selected and frame is <= 2 bytes
    if(!audio->opus_dtx || dlen > 2){
      rtp.seq = htons(seq++);
      rtp.timestamp = htonl(timestamp);
      iovec[1].iov_len = dlen; // Length varies
      int r = sendmsg(audio->audio_mcast_fd,&message,0);
      if(r < 0){
	perror("opus: sendmsg");
	break;
      }
      audio->audio_packets++;
      assert(!isnan(audio->bitrate));

      assert(!isnan(decay));
      assert(!isnan(audio->bitrate));
      audio->bitrate = decay * audio->bitrate + 8 * dlen;
      assert(!isnan(audio->bitrate));
    }
    // always update timestamp so decoder will know how much was dropped
    timestamp += opus_blocksize;
  }
  close(audio->opus_stereo_read_fd);
  audio->opus_stereo_read_fd = -1;
  opus_encoder_destroy(audio->opus);
  audio->opus = NULL;
  return NULL;
}
void *stereo_pcm_audio(void *arg){
  pthread_setname("stereo-pcm");
  assert(arg != NULL);
  struct audio *audio = arg;

  uint32_t timestamp = 0;
  uint16_t seq = 0;
  time_t tt = time(NULL);
  uint32_t const ssrc = tt & 0xffffffff;

  struct rtp_header rtp;
  rtp.vpxcc = (RTP_VERS << 6); // Version 2, padding = 0, extension = 0, csrc count = 0
  rtp.ssrc = htonl(ssrc);
  rtp.mpt = 10;         // 16 bit linear, big endian, stereo

  int16_t PCM_buf[PCM_BUFSIZE];

  struct iovec iovec[2];      
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = PCM_buf;
  iovec[1].iov_len = sizeof(PCM_buf); // byte count - fixed
  
  struct msghdr message;      
  message.msg_name = NULL; // Set by connect() call in setup_output()
  message.msg_namelen = 0;
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;
  
  float decay = expf((-PCM_BUFSIZE/2.)/ audio->samprate);

  complex float buffer[PCM_BUFSIZE/2];
  while(pipefill(audio->pcm_stereo_read_fd,buffer,sizeof(buffer)) > 0){
    int i;
    for(i=0;i<PCM_BUFSIZE/2;i++){
      PCM_buf[2*i] = htons(scaleclip(crealf(buffer[i])));
      PCM_buf[2*i+1] = htons(scaleclip(cimagf(buffer[i])));
    }
    rtp.seq = htons(seq++);
    rtp.timestamp = htonl(timestamp);
    timestamp += PCM_BUFSIZE/2; // Increase by stereo sample count
    int r = sendmsg(audio->audio_mcast_fd,&message,0);
    if(r < 0){
      perror("stereo_pcm: sendmsg");
      break;
    }
    assert(!isnan(decay));
    assert(!isnan(audio->bitrate));
    audio->bitrate = decay * audio->bitrate + 8 * sizeof(PCM_buf);
    assert(!isnan(audio->bitrate));
    audio->audio_packets++;
  }
  close(audio->pcm_stereo_read_fd);
  audio->pcm_stereo_read_fd = -1;
  return NULL;
}

void *mono_pcm_audio(void *arg){
  pthread_setname("mono-pcm");
  assert(arg != NULL);
  struct audio *audio = arg;
  uint32_t timestamp = 0;
  uint16_t seq = 0;
  time_t tt = time(NULL);
  uint32_t const ssrc = tt & 0xffffffff;

  struct rtp_header rtp;
  rtp.vpxcc = (RTP_VERS << 6); // Version 2, padding = 0, extension = 0, csrc count = 0
  rtp.ssrc = htonl(ssrc);
  rtp.mpt = 11;         // 16 bit linear, big endian, mono

  int16_t PCM_buf[PCM_BUFSIZE];

  struct iovec iovec[2];      
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = PCM_buf;
  iovec[1].iov_len = sizeof(PCM_buf); // byte count - fixed
  
  struct msghdr message;      
  message.msg_name = NULL; // Set by connect() call in setup_output()
  message.msg_namelen = 0;
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;
    
  float decay = expf(-1.0 * PCM_BUFSIZE / audio->samprate);

  float buffer[PCM_BUFSIZE];
  while(pipefill(audio->pcm_mono_read_fd,buffer,sizeof(buffer)) > 0){
    int i;
    for(i=0;i<PCM_BUFSIZE;i++)
      PCM_buf[i] = htons(scaleclip(buffer[i]));

    rtp.seq = htons(seq++);
    rtp.timestamp = htonl(timestamp);
    timestamp += PCM_BUFSIZE; // Increase by stereo sample count
    int r = sendmsg(audio->audio_mcast_fd,&message,0);
    if(r < 0){
      perror("stereo_pcm: sendmsg");
      break;
    }
    assert(!isnan(decay));
    assert(!isnan(audio->bitrate));
    audio->bitrate = decay * audio->bitrate + 8 * sizeof(PCM_buf);
    assert(!isnan(audio->bitrate));
    audio->audio_packets++;
  }
  close(audio->pcm_mono_read_fd);
  audio->pcm_mono_read_fd = -1;
  return NULL;
}

// Set up pipes to encoding/sending tasks and start them up
int setup_audio(struct audio *audio){
  
  if(Verbose)
    fprintf(stderr,"%s\n",opus_get_version_string());
  
  assert(audio != NULL);

  audio->bitrate = 0;
  
  // Set up audio output stream(s)
  if(audio->audio_mcast_fd > 0)
    close(audio->audio_mcast_fd);
  audio->audio_mcast_fd = setup_mcast(audio->audio_mcast_address_text,Mcast_dest_port,1);
  if(audio->audio_mcast_fd == -1){
    fprintf(stderr,"Can't set up multicast audio output\n");
    return -1;
  }

  int pipefd[2];

  if(audio->opus_stereo_write_fd > 0){
    close(audio->opus_stereo_write_fd);
    pthread_join(audio->opus_stereo_thread,NULL);
  }
  pipe(pipefd);
  audio->opus_stereo_read_fd = pipefd[0];
  audio->opus_stereo_write_fd = pipefd[1];
  pthread_create(&audio->opus_stereo_thread,NULL,stereo_opus_audio,audio);

  if(audio->pcm_stereo_write_fd > 0){
    close(audio->pcm_stereo_write_fd);
    pthread_join(audio->pcm_stereo_thread,NULL);
  }
  pipe(pipefd);
  audio->pcm_stereo_read_fd = pipefd[0];
  audio->pcm_stereo_write_fd = pipefd[1];
  pthread_create(&audio->pcm_stereo_thread,NULL,stereo_pcm_audio,audio);

  if(audio->pcm_mono_write_fd > 0){
    close(audio->pcm_mono_write_fd);
    pthread_join(audio->pcm_mono_thread,NULL);
  }
  pipe(pipefd);
  audio->pcm_mono_read_fd = pipefd[0];
  audio->pcm_mono_write_fd = pipefd[1];
  pthread_create(&audio->pcm_mono_thread,NULL,mono_pcm_audio,audio);

  return 0;
}
