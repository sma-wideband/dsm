/*
** pound dsm with lots of activity; poundrec will be the recipient of
** the activity
**
** $Id: poundsend.c,v 2.4 2013/03/01 21:01:42 ckatz Exp $
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
#include <string.h>
#include <pthread.h>

#define _DSM_INTERNAL
#include "dsm.h"


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

  int pid=(int)getpid();

  int s, i, j;
  long writecount=0;
  char *p, *buf;
  size_t size;

  struct alloc_list_head *alhp = NULL;
  struct class_entry     *clp  = NULL;
  struct class_share     *csp  = NULL;
  u_long nmachines;
  u_long nclasses;
  u_long nclassshares;
  u_long version;
  u_long my_address;
  u_long *my_addresses;
  int my_num_addresses;

  u_long mach, alloc, element;

  struct hostent *hep;
  char myhostname[DSM_NAME_LENGTH];
  char *targetname, *allocname, *elementname;

  struct alloc_list_head *thisalhp;
  struct alloc_entry *thisaep;

  unsigned usec;
  
  int random=DSM_FALSE;
  int delay=1000000;
  unsigned long randseed;

  char *progname;
  char label[DSM_FULL_NAME_LENGTH];

  dsm_structure ds;
  int is_structure;

  char allocfile[256];

  /*********/
  /* Begin */
  /*********/

  progname = strrchr(argv[0], '/');

  if(progname == (char *)NULL)
    progname = argv[0];
  else
    progname += 1;
 
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


  /*************************************************/
  /* now get the allocation table and my host info */
  /*************************************************/
  s = dsm_determine_network_info(&my_addresses, &my_num_addresses, myhostname);
  if(s != DSM_SUCCESS) {
    fprintf(stderr,
	    "Failed to determine our network information\n");
    exit(DSM_ERROR);
  }

  allocfile[0] = '\0';
  dsm_determine_alloc_file_location(allocfile);

  if(dsm_read_allocation_file(&alhp, &nmachines,
			      &clp,  &nclasses,
			      &csp,  &nclassshares,
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

  /************************/
  /* start dsm operations */
  /************************/
  while( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    sleep(1);
  }

  randseed = (unsigned long)time(NULL)/* + (unsigned long)pid*/;

  while(1) {
    
    /* pick a random allocation for a random machine */
    mach  = (int)((double)(nmachines+nclassshares)
		  * myrandom(&randseed) / UINT_MAX);
    
    if(mach >= nmachines) {
      /* do a class-wide write */
      int sh = mach-nmachines;
      targetname = csp[sh].class->class_name;

      alloc = (int)((double)csp[sh].nallocs
		    * myrandom(&randseed) / UINT_MAX);

      allocname = csp[sh].csaep[alloc].allocname;

      /* find the alloc entry */
      thisalhp = NULL;
      for(i=0; i<nmachines; i++) {
	if(!strcmp(alhp[i].machine_name,
		   csp[sh].class->machinelist[0].machine_name)) {
	  thisalhp = &alhp[i];
	  break;
	}
      }
      if(thisalhp==NULL) {
	fprintf(stderr, "didn't find alhp\n");
	exit(-1);
      }
      
      thisaep=NULL;
      for(i=0; i<thisalhp->nallocs; i++) {
	if(!strcmp(thisalhp->allocs[i].name, allocname)) {
	  thisaep = &thisalhp->allocs[i];
	  break;
	}
      }
      if(thisaep==NULL) {
	fprintf(stderr, "didn't find alloc\n");
	exit(-1);
      }

      element = (int)((double)thisaep->n_elements 
		    * myrandom(&randseed) / UINT_MAX);
      size = thisaep->elements[element].size;
      elementname = thisaep->elements[element].name;
    }
    else {
      /* single-host write */
      targetname = alhp[mach].machine_name;
      alloc = (int)((double)alhp[mach].nallocs 
		    * myrandom(&randseed) / UINT_MAX);
      allocname = alhp[mach].allocs[alloc].name;
      element = (int)((double)alhp[mach].allocs[alloc].n_elements 
		      * myrandom(&randseed) / UINT_MAX);
      size = alhp[mach].allocs[alloc].elements[element].size;
      thisalhp = &alhp[mach];
      thisaep = &alhp[mach].allocs[alloc];
      elementname = thisaep->elements[element].name;
    }

    if(random==DSM_TRUE) {
      usec = (unsigned)((double)delay * myrandom(&randseed) / UINT_MAX);
    }
    else {
      usec = (unsigned)(2.0 * myrandom(&randseed) / UINT_MAX);
    }

    if( (buf = (char *)malloc(size)) == NULL ) {
      fprintf(stderr,"Error: can't malloc(%d)\n",size);
      exit(1);
    }
    
    p = buf;

    /* if it's a structure, initialize */
    is_structure = thisaep->is_structure;
    if(is_structure == DSM_TRUE) {
      s = dsm_structure_init(&ds, allocname);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_structure_init");
	free(p);
	sleep(2);
	continue;
      }
      p = (char *)&ds;
    }

    if(size==1)       *((char  *)buf) = (char)  writecount;
    else if(size==2)  *((short *)buf) = (short) writecount;
    else              *((long  *)buf) = (long)  writecount;

    if(is_structure == DSM_TRUE) {
      s = dsm_structure_set_element(&ds, elementname, buf);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_structure_set_element");
	exit(1);
      }
    }

    if(usec % 2) {
      s=dsm_write(targetname, allocname, p);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_write()");
      }
    }
    else {
      s=dsm_write_notify(targetname, allocname, p);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_write_notify()");
      }
    }

    strcpy(label, allocname);
    if(is_structure == DSM_TRUE) {
      strcat(label, ":");
      strcat(label, elementname);
    }
    
    printf("%s[%2d]: val%6ld, sz %3d, (wt%7d) %s:%s\n",
	   (usec % 2) ? "wrt" : "not",
	   pid, writecount,
	   size, random==DSM_TRUE ? usec : delay,
	   targetname, label);

    writecount++;

    if(is_structure == DSM_TRUE) dsm_structure_destroy(&ds);
 
    free(buf);
    
    if(random==DSM_TRUE) usleep( usec );
    else usleep(delay);
  }
    
  return(0);
}
