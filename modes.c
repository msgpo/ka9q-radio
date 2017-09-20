// $Id: modes.c,v 1.17 2017/09/20 06:41:54 karn Exp karn $
#include <limits.h>
#include <stdio.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <string.h>
#include <errno.h>
#include "radio.h"
#include "dsp.h"


struct modetab Modes[256];
int Nmodes;

extern char Libdir[];

// Linkage table from ascii names to demodulator routines
struct demodtab {
  char name[16];
  void * (*demod)(void *);
} Demodtab[] = {
  {"fm",     demod_fm},  // NBFM and noncoherent PM
  {"am",     demod_am},  // Envelope detection of AM
  {"linear", demod_dsb}, // Coherent demodulation of AM, DSB, BPSK; calibration on WWV/WWVH/CHU carrier
};
#define NDEMOD (sizeof(Demodtab)/sizeof(struct demodtab))

int readmodes(char *file){
  char pathname[PATH_MAX];
  snprintf(pathname,sizeof(pathname),"%s/%s",Libdir,file);
  FILE * const fp = fopen(pathname,"r");
  if(fp == NULL){
    fprintf(stderr,"Can't read mode table %s:%s\n",pathname,strerror(errno));
    return -1;
  }
  char line[PATH_MAX];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    if(line[0] == '#' || line[0] == '*' || line[0] == '/')
      continue; // comment

    char name[16],demod[16],options[16];
    float low,high,shift;
    if(sscanf(line,"%16s %16s %f %f %f %16s",name,demod,&low,&high,&shift,options) < 4)
      continue; // Too short, or in wrong format

    int i;
    for(i=0;i<NDEMOD;i++)
      if(strncasecmp(demod,Demodtab[i].name,strlen(Demodtab[i].name)) == 0)
	break;
      
    if(i == NDEMOD)
      continue; // Not found
    
    strlcpy(Modes[Nmodes].name,name,sizeof(Modes[Nmodes].name));
    Modes[Nmodes].demod = Demodtab[i].demod;
    Modes[Nmodes].low = low;
    Modes[Nmodes].high = high;
    Modes[Nmodes].shift = shift;
    Modes[Nmodes].flags = 0;
    if(strcasecmp(options,"conj") == 0){
      Modes[Nmodes].flags |= CONJ;
    } else if(strcasecmp(options,"flat") == 0){
      Modes[Nmodes].flags |= FLAT;
    } else if(strcasecmp(options,"cal") == 0){
      Modes[Nmodes].flags |= CAL|COHERENT;
    } else if(strcasecmp(options,"square") == 0){
      Modes[Nmodes].flags |= SQUARE|COHERENT;
    } else if(strcasecmp(options,"coherent") == 0){
      Modes[Nmodes].flags |= COHERENT;
    }

    Nmodes++;
  }
  return 0;
}

