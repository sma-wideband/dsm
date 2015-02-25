/*
** receive the pounding sent by the poundsend program
**
** $Id: poundrec.c,v 2.4 2013/03/01 21:01:42 ckatz Exp $
*/


#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <limits.h>
#include <netdb.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#define _DSM_INTERNAL
#include "dsm.h"

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

unsigned long myrandom(unsigned long *dd) {
  return (*dd = 1664525L* *dd + 1013904223L);
}

int main(int argc, char *argv[]) {
  int pid=(int)getpid();

  int i,j,s;
  unsigned long randseed;
  int howmany, totalallocs=0;
  size_t size;
  u_long mach,alloc;

  char myhostname[DSM_NAME_LENGTH];
  u_long *my_addresses;
  u_long my_address;
  int my_num_addresses;

  char *p, host[DSM_NAME_LENGTH], allocname[DSM_NAME_LENGTH];
  char *progname;
  
  struct alloc_list_head *alhp=NULL;
  struct class_entry     *clp=NULL;
  struct class_share     *csp=NULL;
  u_long nmachines,nclasses,nclassshares,version;

  int readwait;

  time_t t;

  int is_structure;

#define NSTRUCT 100
  dsm_structure ds[NSTRUCT];

  struct timeval now;
  struct timezone dummy;

  char allocfile[256];

  /*********/
  /* Begin */
  /*********/

  if(argc>1 && argv[1][0]=='-' && argv[1][1]=='v') verbose=DSM_TRUE;

  progname = strrchr(argv[0], '/');

  if(progname == (char *)NULL) progname  = argv[0];
  else                         progname += 1;

  for(i=0; i<NSTRUCT; i++) ds[i].name[0] = '\0';

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
  
  totalallocs=0;
  for(i=0; i<nmachines; i++) {
    if(verbose)
      printf("Host #%d %s, %d allocs\n", 
	     i, alhp[i].machine_name, alhp[i].nallocs);
    
    for(j=0; j<alhp[i].nallocs; j++) {
      if(verbose) printf("  alloc #%d size %3d  %s\n",
			 j,
			 alhp[i].allocs[j].size,
			 alhp[i].allocs[j].name);
      totalallocs++;
    }
  }



  /************************/
  /* start dsm operations */
  /************************/
  if( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    exit(DSM_ERROR);
  }

  randseed = (unsigned long)time(NULL)/* + (unsigned long)pid*/;

  while(1) {
    if(myrandom(&randseed) & 0x2000UL) readwait = DSM_TRUE;
    else                               readwait = DSM_FALSE;

    /* how many allocations do we wait on? */
    if(readwait) {
      howmany = 1 +
	(int)
	((double)totalallocs * ((double)myrandom(&randseed) / (double)UINT_MAX));

      if(howmany>NSTRUCT) howmany = NSTRUCT;
      printf("%s[%d] will monitor %d allocations\n", progname, pid, howmany);
    }
    else {
      howmany = 1;
    }


    size = 0;
    for(i=0; i<howmany; i++) {
      dprintf("%s[%d]: looping to select howmany=%d allocs; doing i=%d\n",
	      progname, pid, howmany, i);

      /* pick one at random */
      mach  = (int)((double)nmachines * myrandom(&randseed) / UINT_MAX);
      alloc = (int)((double)alhp[mach].nallocs * myrandom(&randseed)
		    / UINT_MAX);

      is_structure = alhp[mach].allocs[alloc].is_structure;

      if(is_structure == DSM_TRUE) {
	/* it's a structure; initialize a structure object */
	dprintf("about to call dsm_struct_init(&ds[%d], %s)\n",
		i, alhp[mach].allocs[alloc].name);
	s = dsm_structure_init(&ds[i], alhp[mach].allocs[alloc].name);
	if(s!=DSM_SUCCESS) {
	  dsm_error_message(s, "dsm_structure_init");
	  fprintf(stderr,
		  "can't initialize dsm_struct for %s; skipping\n",
		  alhp[mach].allocs[alloc].name);
	  continue;
	}
	
	size = sizeof(dsm_structure *) > size ? sizeof(dsm_structure *) : size;
      }
      else {
	size =
	  alhp[mach].allocs[alloc].size > size 
	  ?  alhp[mach].allocs[alloc].size
	  :  size;
      }

      if(readwait) {
	dprintf("%s[%d]: mach = %d/%d alloc=%d/%d size=%d\n",
		progname, pid,
		mach, nmachines,
		alloc, alhp[mach].nallocs,
		size);

	dprintf("%s[%d]: calling dsm_monitor for \"%s\":\"%s\"\n",
		progname,pid,
		alhp[mach].machine_name,alhp[mach].allocs[alloc].name);

	if(is_structure==DSM_TRUE) {
	  s = dsm_monitor(alhp[mach].machine_name,
			  alhp[mach].allocs[alloc].name,
			  &ds[i]);
	}
	else {
	  s = dsm_monitor(alhp[mach].machine_name,
			  alhp[mach].allocs[alloc].name);
	}
	if(s!=DSM_SUCCESS) {
	  if(s==DSM_IN_MON_LIST) {
	    if(is_structure==DSM_TRUE) {
	      dsm_structure_destroy(&ds[i]);
	      ds[i].name[0]='\0';
	    }
	    --i;
	    continue;
	  }
	  else {
	    fprintf(stderr, "error: alloc %s/%s\n",
		    alhp[mach].machine_name,alhp[mach].allocs[alloc].name);

	    dsm_error_message(s, "dsm_monitor()");
	    if(s==DSM_RPC_ERROR || s==DSM_MON_LIST_EMPTY) continue;
	    else                                          abort();
	  }
	}
	
	printf("%s[%d]: monitor #%d (sz %d) %s:%s\n",
	       progname,pid,i+1,alhp[mach].allocs[alloc].size,
	       alhp[mach].machine_name,alhp[mach].allocs[alloc].name);
      }
    } /* for loop over howmany */

    dprintf("%s[%d]: allocating %d for return buffer\n",
	    progname,pid,size);
    p = (char *)malloc(size);
    if(p==(char *)NULL) {
      perror("malloc");
      abort();
    }

    if(readwait) {
      gettimeofday(&now, &dummy);
      printf("%s[%d]: calling dsm_read_wait() at %ld.%06ld\n",
	     progname,pid, now.tv_sec, now.tv_usec);
      s = dsm_read_wait(host, allocname, p);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_read_wait()");
	if(s==DSM_RPC_ERROR) {
	  free(p);
	  
	  s = dsm_clear_monitor();
	  if(s != DSM_SUCCESS) {
	    dsm_error_message(s,"dsm_clear_monitor()");
	    if(s!=DSM_RPC_ERROR) abort();
	  }
	  sleep(2);
	  continue;
	}
	else if(s==DSM_MON_LIST_EMPTY) {
	  free(p);
	  continue;
	}
	else 
	  abort();
      }
      
      if(allocname[strlen(allocname)-1]=='X') {
	dsm_structure *sp = *((dsm_structure **)p);
	printf("%s[%d] **** received %s:%s\n",
	       progname, pid, host, allocname);
	for(i=0; i<sp->n_elements; i++) {
	  printf("                       %s: lval=%d at %d\n",
		 sp->elements[i].name,
		 *(unsigned *)(sp->elements[i].datap),
		 (int)time(NULL));
	}
      }
      else {
	gettimeofday(&now, &dummy);
	printf("%s[%d] **** received %s:%s, lval=%ld at %ld.%06ld\n",
	       progname, pid, host, allocname, *((long *)p),
	       now.tv_sec, now.tv_usec);
      }
      
      dprintf("%s[%d] calling dsm_clear_monitor()\n",
	      progname, pid);
      s = dsm_clear_monitor();
      if(s != DSM_SUCCESS) {
	dsm_error_message(s,"dsm_clear_monitor()");
	if(s==DSM_RPC_ERROR) {
	  free(p);
	  continue;
	}
	else
	  abort();
      }
      
      for(i=0; i<NSTRUCT; i++) {
	if(ds[i].name[0] != '\0') {
	  dsm_structure_destroy(&ds[i]);
	  ds[i].name[0] = '\0';
	}
      }
      
      free(p);
    }
    else {
      dprintf("%s[%d]: calling dsm_read() at %d\n",
	      progname,pid, (int)time(NULL));
      
      printf("%s[%d] **** reading %s:%s\n",
	     progname, pid,
	     alhp[mach].machine_name,
	     alhp[mach].allocs[alloc].name);
      
      if(is_structure) {
	free(p);
	p = (char *)&ds[0];
      }

      s = dsm_read(alhp[mach].machine_name,
		   alhp[mach].allocs[alloc].name,
		   p,
		   &t);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_read()");
	if(s==DSM_RPC_ERROR) {
	  if(!is_structure) free(p);
	  continue;
	}
	else                 abort();
      }
      
      if(is_structure) {
	for(i=0; i<ds[0].n_elements; i++) {
	  printf("  %30s: lval %ld\n", 
		 ds[0].elements[i].name,
		 *(long *)(ds[0].elements[i].datap));
	}
	dsm_structure_destroy(&ds[0]);
	ds[0].name[0]='\0';
      }
      else {
	printf("%s[%d]           lval=%ld at %d\n",
	       progname, pid, *((long *)p), (int)time(NULL));
	free(p);
      }
    } /* if not read_wait */

    printf("\n\n");
  } /* while 1 */

  return(0);
}
