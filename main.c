// $Id: main.c,v 1.33 2017/06/15 00:58:02 karn Exp karn $
// Read complex float samples from stdin (e.g., from funcube.c)
// downconvert, filter and demodulate
// Take commands from UDP socket
#define _GNU_SOURCE 1
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "radio.h"
#include "filter.h"
#include "dsp.h"
#include "audio.h"
#include "command.h"
#include "rtp.h"

#define MAXPKT 1500

void closedown(int a);

void *input_loop(struct demod *);
int process_command(struct demod *,char *cmdbuf,int len);
pthread_t Display_thread;

int Nthreads = 1;
int ADC_samprate = 192000;
int DAC_samprate = 48000;

int Quiet;
int Ctl_port = 4159;

struct sockaddr_in Ctl_address;
struct sockaddr_in Input_source_address;
struct sockaddr_in Input_mcast_sockaddr;

char *OPUS_mcast_address_text = "239.1.2.6";
char *PCM_mcast_address_text = "239.1.2.5";
int OPUS_bitrate = 32000;
int OPUS_blocksize = 960;

int Input_fd;
int Ctl_fd;
int Demod_sock;
int Mcast_dest_port = 5004;     // Default for testing; recommended default RTP port

int Skips;
int Delayed;

int main(int argc,char *argv[]){
  int c,N;
  char *locale;

  enum mode mode;
  double second_IF;
  struct demod *demod = &Demod;

  locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  fprintf(stderr,"General coverage receiver for the Funcube Pro and Pro+\n");
  fprintf(stderr,"Copyright 2016 by Phil Karn, KA9Q; may be used under the terms of the GNU General Public License\n");
  fprintf(stderr,"Compiled %s on %s\n",__TIME__,__DATE__);

  // Defaults
  Quiet = 0;
  // The FFT length will be L + M - 1 because of window overlapping
  demod->L = 4096;      // Number of samples in buffer
  demod->M = 4096+1;    // Length of filter impulse response
  mode = FM;
  second_IF = ADC_samprate/4;
  char *iq_mcast_address_text = "239.1.2.3"; // Default for testing
  float opus_blockms = 20.0;

  while((c = getopt(argc,argv,"b:B:i:I:k:l:L:m:M:O:p:P:qr:t:")) != EOF){
    int i;

    switch(c){
    case 'B':
      opus_blockms = atof(optarg);
      break;
    case 'i':
      second_IF = atof(optarg);
      break;
    case 'I':
      iq_mcast_address_text = optarg;
      break;
    case 'k':
      Kaiser_beta = atof(optarg);
      break;
    case 'l':
      locale = optarg;
      break;
    case 'L':
      demod->L = atoi(optarg);
      break;
    case 'm':
      for(i = 1; i < Nmodes;i++){
	if(strcasecmp(optarg,Modes[i].name) == 0){
	  mode = Modes[i].mode;
	  break;
	}
      }
      break;
    case 'M':
      demod->M = atoi(optarg);
      break;
    case 'O':
      OPUS_mcast_address_text = optarg;
      break;
    case 'p':
      Ctl_port = atoi(optarg);
      break;
    case 'P':
      PCM_mcast_address_text = optarg;
      break;
    case 'q':
      Quiet++; // Suppress display
      break;
    case 'r':
      OPUS_bitrate = atoi(optarg);
      break;
    case 't':
      Nthreads = atoi(optarg);
      fftwf_init_threads();
      fftwf_plan_with_nthreads(Nthreads);
      fprintf(stderr,"Using %d threads for FFTs\n",Nthreads);
      break;
    default:
      fprintf(stderr,"Usage: %s [-B opus_blockms] [-I iq multicast address] [-l locale] [-L samplepoints] [-m mode] [-M impulsepoints] [-O Opus multicast address] [-P PCM multicast address] [-r opus_bitrate] [-t threads]\n",argv[0]);
      fprintf(stderr,"Default: %s -B %.1f -I %s -l %s -L %d -m %s -M %d -O %s -P %s -r %d -t %d\n",
	      argv[0],opus_blockms,iq_mcast_address_text,locale,demod->L,Modes[mode].name,demod->M,
	      OPUS_mcast_address_text,PCM_mcast_address_text,OPUS_bitrate,Nthreads);
      exit(1);
      break;
    }
  }
  if(iq_mcast_address_text == NULL){
    fprintf(stderr,"Specify -I iq_mcast_address_text_address\n");
    exit(1);
  }
  setlocale(LC_ALL,locale);

  demod->samprate = ADC_samprate * (1 + demod->calibrate);
  demod->decimate = ADC_samprate / DAC_samprate;
  // Verify decimation ratio
  if((ADC_samprate % DAC_samprate) != 0)
    fprintf(stderr,"Warning: A/D rate %'u is not integer multiple of D/A rate %'u; decimation will probably fail\n",
	    ADC_samprate,DAC_samprate);
  
  N = demod->L + demod->M - 1;
  if((N % demod->decimate) != 0)
    fprintf(stderr,"Warning: FFT size %'u is not divisible by decimation ratio %d\n",N,demod->decimate);

  if((demod->M - 1) % demod->decimate != 0)
    fprintf(stderr,"Warning: Filter length %'u - 1 is not divisible by decimation ratio %d\n",demod->M,demod->decimate);

  // Must do this before first filter is created with set_mode(), otherwise a segfault can occur
  fftwf_import_system_wisdom();

  fprintf(stderr,"UDP control port %d\n",Ctl_port);
  fprintf(stderr,"A/D sample rate %'d, D/A sample rate %'d, decimation ratio %'d\n",
	  ADC_samprate,DAC_samprate,demod->decimate);
  fprintf(stderr,"block size: %'d complex samples (%'.1f ms @ %'u S/s)\n",
	  demod->L,1000.*demod->L/ADC_samprate,ADC_samprate);
  fprintf(stderr,"Kaiser beta %'.1lf, impulse response: %'d complex samples (%'.1f ms @ %'u S/s) bin size %'.1f Hz\n",
	  Kaiser_beta,demod->M,1000.*demod->M/ADC_samprate,ADC_samprate,(float)ADC_samprate/N);

  if(!Quiet)
    pthread_create(&Display_thread,NULL,display,NULL);

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  // Set up input socket for multicast data stream from front end
  if(inet_pton(AF_INET,iq_mcast_address_text,&Input_mcast_sockaddr.sin_addr) == 1){
    if((Input_fd = socket(PF_INET,SOCK_DGRAM,0)) == -1){
      perror("can't create IPv4 input socket");
      exit(1);
    }

    int reuse = 1;
    if(setsockopt(Input_fd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse)) != 0)
      perror("ipv4 so_reuseport failed");
    if(setsockopt(Input_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) != 0)
      perror("ipv4 so_reuseaddr failed");

    Input_mcast_sockaddr.sin_family = AF_INET;
    Input_mcast_sockaddr.sin_port = htons(Mcast_dest_port);
    if(bind(Input_fd,(struct sockaddr *)&Input_mcast_sockaddr,sizeof(struct sockaddr_in)) != 0){
      perror("bind on IPv4 input socket");
      exit(1);
    }

#if 1 // old version, seems required on Apple    
    struct ip_mreq mreq;
    mreq.imr_multiaddr = Input_mcast_sockaddr.sin_addr;
    mreq.imr_interface.s_addr = INADDR_ANY;
    if(setsockopt(Input_fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0){
      perror("ipv4 multicast join");
      exit(1);
    }
#else // Linux, etc
    struct group_req group_req;
    group_req.gr_interface = 0;
    Input_mcast_sockaddr.ss_family = AF_INET;
    memcpy(&group_req.gr_group,&Input_mcast_sockaddr,sizeof(Input_mcast_sockaddr));
    if(setsockopt(Input_fd,IPPROTO_IP,MCAST_JOIN_GROUP,&group_req,sizeof(group_req)) != 0){
      perror("ipv4 multicast join");
      exit(1);
    }
#endif
  }

  // Set up control port
  Ctl_address.sin_family = AF_INET;
  Ctl_address.sin_port = htons(Ctl_port);
  Ctl_address.sin_addr.s_addr = INADDR_ANY;

  if((Ctl_fd = socket(PF_INET,SOCK_DGRAM, 0)) == -1)
    perror("can't open control socket");

  if(bind(Ctl_fd,(struct sockaddr *)&Ctl_address,sizeof(Ctl_address)) != 0)
    perror("control bind failed");

  if(fcntl(Ctl_fd,F_SETFL,O_NONBLOCK) != 0)
    perror("control fcntl noblock");

  // Set up audio output stream(s)
  Mcast_fd = socket(AF_INET,SOCK_DGRAM,0);

  // I've given up on IPv6 multicast for now. Too many bugs in too many places
  if(PCM_mcast_address_text != NULL && strlen(PCM_mcast_address_text) > 0){
    PCM_mcast_sockaddr.sin_family = AF_INET;
    PCM_mcast_sockaddr.sin_port = htons(Mcast_dest_port);
    inet_pton(AF_INET,PCM_mcast_address_text,&PCM_mcast_sockaddr.sin_addr);


    // Strictly speaking, it is not necessary to join a multicast group to which we only send.
    // But this creates a problem with brain-dead Netgear (and probably other) "smart" switches
    // that do IGMP snooping. There's a setting to handle what happens with multicast groups
    // to which no IGMP messages are seen. If set to discard them, IPv6 multicast breaks
    // because there's no IPv6 multicast querier. But set to pass them, then IPv4 multicasts
    // that aren't subscribed to by anybody are flooded everywhere! We avoid that by subscribing
    // to our own multicasts.
#if __APPLE__ // Newer, protocol-independent MCAST_JOIN_GROUP doesn't seem to work on OSX
    struct ip_mreq mreq;
    mreq.imr_multiaddr = ((struct sockaddr_in *)&PCM_mcast_sockaddr)->sin_addr;
    mreq.imr_interface.s_addr = INADDR_ANY;
    if(setsockopt(Mcast_fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0)
      perror("ipv4 multicast join");

#else // Linux, etc
    struct group_req group_req;
    group_req.gr_interface = 0;
    Input_mcast_sockaddr.sin_family = AF_INET;
    memcpy(&group_req.gr_group,&PCM_mcast_sockaddr,sizeof(Input_mcast_sockaddr));
    if(setsockopt(Mcast_fd,IPPROTO_IP,MCAST_JOIN_GROUP,&group_req,sizeof(group_req)) != 0)
      perror("ipv4 multicast join");
#endif

  }
  if(OPUS_bitrate > 0 && OPUS_mcast_address_text != NULL && strlen(OPUS_mcast_address_text) > 0){  
    OPUS_mcast_sockaddr.sin_family = AF_INET;
    OPUS_mcast_sockaddr.sin_port = htons(Mcast_dest_port);
    inet_pton(AF_INET,OPUS_mcast_address_text,&OPUS_mcast_sockaddr.sin_addr);

#if __APPLE__ // Newer, protocol-independent MCAST_JOIN_GROUP doesn't seem to work on OSX
    struct ip_mreq mreq;
    mreq.imr_multiaddr = OPUS_mcast_sockaddr.sin_addr;
    mreq.imr_interface.s_addr = INADDR_ANY;
    if(setsockopt(Mcast_fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0)
      perror("ipv4 multicast join");

#else // Linux, etc
    struct group_req group_req;
    group_req.gr_interface = 0;
    Input_mcast_sockaddr.sin_family = AF_INET;
    memcpy(&group_req.gr_group,&OPUS_mcast_sockaddr,sizeof(OPUS_mcast_sockaddr));
    if(setsockopt(Mcast_fd,IPPROTO_IP,MCAST_JOIN_GROUP,&group_req,sizeof(group_req)) != 0)
      perror("ipv4 multicast join");
#endif

  }
  // Apparently works for both IPv4 and IPv6
  int ttl = 2;
  if(setsockopt(Mcast_fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl)) != 0)
    perror("setsockopt multicast ttl failed");

  // Pipes for front end -> demod
  int sv[2];
  pipe(sv);
  Demod_sock = sv[1]; // write end
  demod->input = sv[0]; // read end
#ifdef F_SETPIPE_SZ // Linux only
  int sz;
  sz = fcntl(Demod_sock,F_SETPIPE_SZ,demod->L * sizeof(complex float));
#if 0
  fprintf(stderr,"sock size %d\n",sz);
#endif
  if(sz == -1)
    perror("F_SETPIPE_SZ");
#endif // F_SETPIPE_SZ
  if(setup_audio(opus_blockms,OPUS_bitrate) != 0){
    fprintf(stderr,"Audio setup failed\n");
    exit(1);
  }

  set_mode(demod,mode);
  set_second_LO(demod,-second_IF,1);
  input_loop(demod); // Doesn't return

  exit(0);
}

void display_cleanup(void *);

void closedown(int a){
  if(!Quiet)
    fprintf(stderr,"radio: caught signal %d: %s\n",a,strsignal(a));
  display_cleanup(NULL);

  exit(1);
}

// Read from RTP network socket, assemble blocks of samples with corrections done

void *input_loop(struct demod *demod){
  int cnt;
  short samples[MAXPKT];
  struct rtp_header rtp_header;
  struct status status;
  struct iovec iovec[3];

  iovec[0].iov_base = &rtp_header;
  iovec[0].iov_len = sizeof(rtp_header);
  iovec[1].iov_base = &status;
  iovec[1].iov_len = sizeof(status);
  iovec[2].iov_base = samples;
  iovec[2].iov_len = sizeof(samples);
  
  struct msghdr message;
  message.msg_name = &Input_source_address;
  message.msg_namelen = sizeof(Input_source_address);
  message.msg_iov = iovec;
  message.msg_iovlen = sizeof(iovec) / sizeof(struct iovec);
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;

  int eseq = -1;

  while(1){
    socklen_t addrlen;
    int rdlen;
    char pktbuf[MAXPKT];
    fd_set mask;

    FD_ZERO(&mask);
    FD_SET(Ctl_fd,&mask);
    FD_SET(Input_fd,&mask);
    
    select(max(Ctl_fd,Input_fd)+1,&mask,NULL,NULL,NULL);

    if(FD_ISSET(Ctl_fd,&mask)){
      // Got a command
      addrlen = sizeof(Ctl_address);
      rdlen = recvfrom(Ctl_fd,&pktbuf,sizeof(pktbuf),0,(struct sockaddr *)&Ctl_address,&addrlen);
      // Should probably look at the source address
      if(rdlen > 0)
	process_command(demod,pktbuf,rdlen);
    }
    if(FD_ISSET(Input_fd,&mask)){
      // Receive I/Q data from front end
      cnt = recvmsg(Input_fd,&message,0);
      if(cnt <= 0){    // ??
	perror("recvfrom");
	usleep(50000);
	continue;
      }
      if(cnt < sizeof(rtp_header) + sizeof(status))
	continue; // Too small, ignore
      
      // Host byte order
      rtp_header.ssrc = ntohl(rtp_header.ssrc);
      rtp_header.seq = ntohs(rtp_header.seq);
      rtp_header.timestamp = ntohl(rtp_header.timestamp);
      if(eseq != -1 && (int16_t)(eseq - rtp_header.seq) < 0){
	Skips++;
      } else if(eseq != -1 && (int16_t)(eseq - rtp_header.seq) > 0){
	Delayed++;
      }
      eseq = rtp_header.seq + 1;
      
      demod->first_LO = status.frequency;
      demod->lna_gain = status.lna_gain;
      demod->mixer_gain = status.mixer_gain;
      demod->if_gain = status.if_gain;    
      cnt -= sizeof(rtp_header) + sizeof(status);
      cnt /= 4; // count 4-byte stereo samples
      proc_samples(demod,samples,cnt);
    }
  }
}
int process_command(struct demod *demod,char *cmdbuf,int len){
  struct command command;

  if(len >= sizeof(command)){
    memcpy(&command,cmdbuf,sizeof(command));
    switch(command.cmd){
    case SENDSTAT:
      break; // do this later
    case SETSTATE: // first LO freq, second LO freq, second_LO freq rate, mode
#if 0
      fprintf(stderr,"setstate(%d,%.2lf,%.2lf,%.2lf,%.2lf)\n",
	      command.mode,
	      command.first_LO,
	      command.second_LO,command.second_LO_rate,command.calibrate);
#endif
      // Ignore out-of-range values
      if(command.first_LO > 0 && command.first_LO < 2e9)
	set_first_LO(demod,command.first_LO,0);

      if(command.second_LO >= -demod->samprate/2 && command.second_LO <= demod->samprate/2)
	set_second_LO(demod,command.second_LO,0);
      if(fabs(command.second_LO_rate) < 1e9)
	set_second_LO_rate(demod,command.second_LO_rate,0);
      if(command.mode > 0 && command.mode <= Nmodes)
	set_mode(demod,command.mode);
      if(fabs(command.calibrate) < 1)
	set_cal(demod,command.calibrate);
      break;
    default:
      break; // Ignore
    } // switch
  }
  return 0;
}
 
