/*
** get a value from dsm using read_wait()
**
** $Id: read_wait.c,v 2.1 2006/02/09 14:47:25 ckatz Exp $
*/



#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "dsm.h"

#define DSM_FALSE 0
#define DSM_TRUE  1

int verbose=DSM_FALSE;
int dprintf(const char *format, ...) {
  extern int verbose;
  va_list ap;
  int s=0;

  if(verbose) {
    va_start(ap, format);
    s = vfprintf(stderr, format, ap);
    va_end(ap);
    fflush(stderr);
  }

  return(s);
}


int main(int argc, char *argv[]) {

  int s,i;
  char p[1000],name[DSM_NAME_LENGTH],host[DSM_NAME_LENGTH];
  int n_struct = 100;
  dsm_structure ds[n_struct];

  if(argc<2 || argc % 2 != 1) {
    fprintf(stderr, "Usage: %s <host alloc> [host alloc]...\n", argv[0]);
    exit(1);
  }

  for(i=0; i<n_struct; i++) ds[i].name[0] = '\0';

  if( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    exit(1);
  }
  dprintf("dsm_open() succeeded\n");

  for(i=1; i<argc; i+=2) {

    if(argv[i+1][strlen(argv[i+1])-1] == 'X') {
      s=dsm_structure_init(&ds[i], argv[i+1]);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_structure_init");
      }
      
      s=dsm_monitor(argv[i], argv[i+1], &ds[i]);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_monitor(struct)");
	exit(1);
      }
    }
    else {
      s=dsm_monitor(argv[i], argv[i+1]);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_monitor(nonstruct)");
	exit(1);
      }
    }
    
    dprintf("dsm_monitor(%s:%s) succeeded\n", argv[i], argv[i+1]);
  }

  dprintf("calling dsm_read_wait()\n");
  s=dsm_read_wait(host, name, p);

  if(s!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_read()");
    exit(1);
  }

  if(name[strlen(name)-1]=='X') {
    dsm_structure *sp = *((dsm_structure **)p);
    /* it's a structure */
    printf("received %s from %s\n", name, host);
  
    for(i=0; i<sp->n_elements; i++) {
      printf("  %30s: val 0x%08x\n", 
	     sp->elements[i].name,
	     *(unsigned *)(sp->elements[i].datap));
    }
  }
  else {
    printf("received %s from %s, value 0x%08x\n",
	   name, host, *((unsigned *)p));
  }

  return(0);
}
