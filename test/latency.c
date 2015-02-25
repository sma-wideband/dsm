/* do some latency measurements, using a single dsm variable, keeping
** track of statistics on a per-machine basis 
**
** $Id: latency.c,v 2.4 2013/03/01 21:01:41 ckatz Exp $
*/





#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <signal.h>

#define _DSM_INTERNAL
#include "dsm.h"


#define LATENCY_VAR       "DSM_LATENCY_S"
#define LATENCY_VAR_SIZE  2

static int wcount=0, rcount=0;
static int waccum=0, raccum=0;
static int wmin=INT_MAX, wmax=INT_MIN, rmin=INT_MAX, rmax=INT_MIN;



void siginthandler(void) {
  if(wcount!=0) {
    printf("*** After %d writes, min/avg/max (msec) = %.3f/%.3f/%.3f\n",
	   wcount, 
	   (double)wmin/1000.0, 
	   (double)waccum/(1000.0*(double)wcount),
	   (double)wmax/1000.0);
  }
  
  
  if(rcount!=0) {
    printf("*** After %d reads, min/avg/max (msec) = %.3f/%.3f/%.3f\n",
	   rcount,
	   (double)rmin/1000.0, 
	   (double)raccum/(1000.0*(double)rcount),
	   (double)rmax/1000.0);
  }

  exit(0);
}



unsigned long myrandom(unsigned long *dd) {
  return (*dd = 1664525L* *dd + 1013904223L);
}

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


#define USAGE {\
   fprintf(stderr, "Usage: %s [-v] [-d [r maxdur | delay(usec)]]\n", argv[0]); \
   exit(DSM_ERROR); \
}

int main(int argc, char *argv[]) {

  void dotimestats(char, struct timeval, struct timeval);


  int s, i, j;
  char *p;
  size_t size;

  struct alloc_list_head *alhp = NULL, **mainalhp;
  struct class_entry *clp = NULL;
  struct class_share *csp = NULL;
  u_long nmachines, n, nclasses, nclassshares;
  u_long version;
  u_long my_address;
  u_long *my_addresses;
  int my_num_addresses;

  u_long mach;

  struct hostent *hep;
  char myhostname[DSM_NAME_LENGTH];

  unsigned usec;
  
  int random=DSM_FALSE;
  int delay=1000000;
  unsigned long randseed;

  struct timeval start,stop;
  struct timezone zone;
  time_t t;

  char allocfile[256];

  /*********/
  /* Begin */
  /*********/
 
  signal(SIGINT, (void *)siginthandler);
 
  /* with no args, print usage */
  if(argc==1) USAGE;

  /* command line args */
  for(i=1; i<argc; i++) {
    
    if(argv[i][0] != '-') USAGE;

    switch(argv[i][1]) {
    case 'v':
      verbose = DSM_TRUE;
      break;

    case 'd':
      if(++i >= argc) USAGE;
      if(argv[i][0]=='r') {
	random=DSM_TRUE;
	if(++i >= argc) USAGE;
      }

      delay = atoi(argv[i]);
      if(delay <= 0) USAGE;

      break;
      
    default:
      USAGE;
    }
  }



  printf("Verbose %s, delay = ", verbose==DSM_TRUE ? "on" : "off");
  if(random==DSM_TRUE) printf("random, maxdur = %d usec\n", delay);
  else                 printf("%d usec\n", delay);


  /********************************/
  /* now get the allocation table */
  /********************************/
  s = dsm_determine_network_info(&my_addresses, &my_num_addresses, myhostname);
  if(s != DSM_SUCCESS) {
    fprintf(stderr,
	    "Failed to determine our network information\n");
    exit(DSM_ERROR);
  }

  allocfile[0] = '\0';
  dsm_determine_alloc_file_location(allocfile);
  if(dsm_read_allocation_file(&alhp, &nmachines,
			      &clp, &nclasses,
			      &csp, &nclassshares,
			      &version,
			      my_addresses,
			      my_num_addresses,
			      &my_address,
			      allocfile)
     != DSM_SUCCESS) {
    fprintf(stderr, "Couldn't read allocation file\n");
    exit(DSM_ERROR);
  }

  if(alhp == NULL) {
    printf("No allocations for my host address 0x%lx\n",my_address);
    exit(DSM_ERROR);
  }

  printf("Read allocations for %ld machines\n", nmachines);

  if(verbose) {
    for(i=0; i<nmachines; i++) {
      printf("Host %s, %d allocs\n", alhp[i].machine_name, alhp[i].nallocs);
      for(j=0; j<alhp[i].nallocs; j++) {
	printf("  size %3d  %s\n",
	       alhp[i].allocs[j].size,
	       alhp[i].allocs[j].name);
      }
    }
  }

  /* weed out any machines which do not have the LATENCY_VAR variable
   */
  if(verbose) 
    printf("finding hosts which have allocation %s\n", LATENCY_VAR);
  mainalhp = (struct alloc_list_head **)
    malloc(nmachines * sizeof(struct alloc_list_head *)); 
  if(mainalhp == NULL){
    perror("malloc(mainalhp)");
    exit(1);
  }
		     
  n=0;
  for(i=0; i<nmachines; i++) {
    for(j=0; j<alhp[i].nallocs; j++) {
      if(!strcmp(alhp[i].allocs[j].name, LATENCY_VAR)) {
	mainalhp[n++] = &alhp[i];
	if(verbose)
	  printf("machine %s has allocation %s\n", 
		 alhp[i].machine_name,
                 LATENCY_VAR);
        break;
      }
    }
  }
  if(verbose) 
    printf("found %ld machines with allocation %s\n", n, LATENCY_VAR);

  /************************/
  /* start dsm operations */
  /************************/
  while( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    sleep(1);
  }


  randseed = (unsigned long)time(NULL);

  while(1) {
    
    /* pick a random machine */
    
    mach  = (int)((double)n * myrandom(&randseed) / UINT_MAX);

    size = LATENCY_VAR_SIZE;
    
    if(random==DSM_TRUE) {
      usec = (unsigned)((double)delay * myrandom(&randseed) / UINT_MAX);
    }
    else {
      usec = (unsigned)(2.0 * myrandom(&randseed) / UINT_MAX);
    }

    if( (p = (char *)malloc(size)) == NULL ) {
      fprintf(stderr,"Error: can't malloc(%d)\n",size);
      exit(1);
    }
    
    /* are we reading or writing? */
    if( usec % 2 == 0) {


      /* reading */
      gettimeofday(&start, &zone);
      s=dsm_read(mainalhp[mach]->machine_name,
		 LATENCY_VAR,
		 p,
		 &t);
      gettimeofday(&stop, &zone);



      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_read()");
      }
      else {
	printf("R %5d %-20s: %-30s usleep(%6u)\n",
	       *((short *)p),
	       mainalhp[mach]->machine_name,
	       LATENCY_VAR,
	       random==DSM_TRUE ? usec : delay);
	dotimestats('r', start, stop);
      }

      free(p);
    }
    else {
      /* writing */
      *((short *)p) = 10*mach;
      gettimeofday(&start, &zone);
      s=dsm_write(mainalhp[mach]->machine_name,
		  LATENCY_VAR,
		  p);
      gettimeofday(&stop, &zone);


      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_write()");
      }
      else {
	printf("W %5d %-20s: %-30s usleep(%6u)\n",
	       *((short *)p),
	       mainalhp[mach]->machine_name,
	       LATENCY_VAR,
	       random==DSM_TRUE ? usec : delay);
	dotimestats('w', start, stop);
      }

      free(p);
    }
    
    if(random==DSM_TRUE) usleep( usec );
    else usleep(delay);
    
  }
  
  
  return(0);
}


void dotimestats(char c, struct timeval start, struct timeval stop) {

  static int interval=100;

  int dur;


  
  stop.tv_usec += 1000000*(stop.tv_sec - start.tv_sec);
  dur = stop.tv_usec - start.tv_usec;
  
  if(c == 'w') {
    waccum += dur;
    wmin = dur < wmin ? dur : wmin;
    wmax = dur > wmax ? dur : wmax;
    if(++wcount % interval == 0) {
      printf("*** After %d writes, min/avg/max (msec) = %.3f/%.3f/%.3f\n",
	     wcount, 
	     (double)wmin/1000.0,
	     (double)waccum/(1000.0*(double)wcount),
	     (double)wmax/1000.0);
    }
  }

  if(c == 'r') {
    raccum += dur;
    rmin = dur < rmin ? dur : rmin;
    rmax = dur > rmax ? dur : rmax;
    if(++rcount % interval == 0) {
      printf("*** After %d reads, min/avg/max (msec) = %.3f/%.3f/%.3f\n",
	     rcount,
	     (double)rmin/1000.0,
	     (double)raccum/(1000.0*(double)rcount),
	     (double)rmax/1000.0);
    }
  }

}
