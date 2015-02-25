/*
** Run the dsm_get_allocation_list() function
**
** $Id: get_alloc_list.c,v 2.1 2006/02/09 14:42:46 ckatz Exp $
*/


#include <stdarg.h>
#include <stdio.h>


#define _DSM_INTERNAL
#include "dsm.h"
#undef _DSM_INTERNAL

int verbose=DSM_FALSE;
int dprintf(char *format, ...) {
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

  int i,j,s,nhosts;
  struct dsm_allocation_list *dal;
  
  if(argc!=1 && argc!=2) {
    fprintf(stderr,
	    "Usage: %s [-v]\n",argv[0]);
    exit(1);
  }

  if(argc==2 && argv[1][0]=='-' && argv[1][1]=='v') verbose = DSM_TRUE;


  if( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    exit(1);
  }

  s = dsm_get_allocation_list(&nhosts, &dal);
  if(s!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_get_allocation_list()");
    exit(1);
  }

  printf("We share with %d hosts\n", nhosts);
  for(i=0; i<nhosts; i++) {
    
    printf("\nSharing %d entries with host %s:\n",
	   dal[i].n_entries, dal[i].host_name);

    for(j=0; j<dal[i].n_entries; j++)
      printf("  %s\n", dal[i].alloc_list[j]);
  }

  printf("\nDestroying alloc list\n");
  dsm_destroy_allocation_list(&dal);

  return(0);
}
