/*
** dsm_main.c
**
** Contains main() for dsm (Distributed Shared Memory).  This program
** allows the sharing of memory among arbitrary combinations of TCP/IP
** connected computers.  The ability for wait for a particular datum to
** change is provided.
** 
** The sharing of memory is defined in the allocation file
** dsm_allocation.
**
** Written for the Submillimeter Array, Smithsonian Astrophysical
** Observatory, Cambridge, MA.  
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_main.c,v 2.6 2013/03/01 21:01:40 ckatz Exp $
*/


#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <limits.h>
#include <string.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <semaphore.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>

#ifdef __linux__
#  define QUIT_RTN   0
#  define SYSERR_RTN 1
#endif

#ifdef __Lynx__
#  include <socket.h>
#  include "smadaemon.h"
#  include "commonLib.h"
#endif



#define _DSM_INTERNAL
#include "dsm.h"
#undef  _DSM_INTERNAL

static char rcsid[] = "$Id: dsm_main.c,v 2.6 2013/03/01 21:01:40 ckatz Exp $";

/********************/
/* Global Variables */
/********************/

struct alloc_list_head  *alloclist       = (struct alloc_list_head * )NULL;
struct alloc_list_head **alloclistbyname = (struct alloc_list_head **)NULL;
struct class_entry      *classlist       = (struct class_entry *)NULL;
struct class_share      *sharelist       = (struct class_share *)NULL;

u_long my_alloc_version, nmachines, nclasses, nclassshares;
u_long my_address;
char myhostname[DSM_NAME_LENGTH];

time_t starttime;


/* request queue stuff (not strictly a queue) */
struct decoded_request_args *request_queue[DSM_REQ_QUEUE_LENGTH];
struct decoded_request_args *inprogress_list[DSM_REQ_QUEUE_LENGTH];
int request_queue_entries, inprogress_list_entries;

sem_t request_queue_semaphore;
pthread_mutex_t request_queue_mutex;


/* global stuff for the notification table */
pthread_mutex_t                 notification_table_mutex;
void                            *shared_memory;
int                             *dsm_nproc_notify_max_p;
size_t                          *max_buffer_size_p;
struct notification_table_entry *notification_table;
char                            *data_buffers;

pthread_mutex_t                 notification_ring_mutex;
/*********************************************************************/
void Usage(char *progname) {
  fprintf(stderr,"Usage: %s [-v] [-f allocation file path]\n", progname);
  exit(QUIT_RTN);
}

/*********************************************************************/

/* debugging helper; SIGUSR1 toggles debugging file output on/off */
void sigusr1handler(void) {
  extern int verbose;
  extern FILE *errfile;
  time_t now=time(NULL);
  char filename[256];

  if(verbose==DSM_TRUE) {
    verbose=DSM_FALSE;
    if(errfile!=(FILE *)NULL && errfile!=stderr) fclose(errfile);
    fprintf(stderr, "**** dsm: SIGUSR1 caught: debugging off ****\n");
  }
  else {
    verbose=DSM_TRUE;
    sprintf(filename, "dsm_log.%d", (int)now);
    errfile = fopen(filename, "w");
    if(errfile==(FILE *)NULL) {
      perror("sigusr1handler: open(error file)");
      verbose=DSM_FALSE;
    }
    else {
      fprintf(stderr,
	      "**** dsm: SIGUSR1 caught: debugging to %s ****\n",
	      filename);
    }
  }
}

int main(int argc, char *argv[]) {

  void remote_write_giver(struct decoded_request_args *);

  extern FILE *errfile;
  
  char *progname = argv[0];
  char alloc_file_name[256];
  extern int verbose;
  
  SVCXPRT *transp;

  pthread_t thread;

  int i, j, k, s;

  u_long *my_addresses;
  int my_num_addresses;

  size_t largest_largest;

  /*********/
  /* Begin */
  /*********/

#ifdef __Lynx__
  /* for some unknown reason we get these signals from time to time;
     ignoring them doesn't seem to hurt anything */
  signal(SIGUDEF29, SIG_IGN);
  signal(SIGEMT,    SIG_IGN);
#endif

  /* no need to die just because an RPC didn't complete; we handle that
     case ourselves */
  signal(SIGPIPE, SIG_IGN);

  /* to help debugging */
  signal(SIGUSR1,   (void *)sigusr1handler);

  
#ifdef __Lynx__
  DAEMONSET;
#endif

  alloc_file_name[0] = '\0';

  fclose(stdin);
  fclose(stdout);

  starttime = time(NULL);
  fprintf(stderr, "dsm: start at time %d\n", (int)starttime);

  errfile = (FILE *)NULL;

  /* parse command line arguments */
  while( --argc > 0 ) {
    if((*++argv)[0] != '-') Usage(progname);

    switch( (*argv)[1] ) {
    case 'v':
      verbose = DSM_TRUE;
      errfile = stderr;
      break;

    case 'f':
      if(--argc <= 0) Usage(progname);
      argv++;
      strcpy(alloc_file_name, *argv);
      break;

    default:
      Usage(progname);
    }
  }

  /* get network information */
  s = dsm_determine_network_info(&my_addresses, &my_num_addresses, myhostname);
  if(s != DSM_SUCCESS) {
    fprintf(stderr,
	    "Failed to determine our network information\n");
    exit(QUIT_RTN);
  }

  /*****************************************/
  /* only one dsm server may be running at */
  /* once; make sure that's the case       */
  /***************************************/
#ifdef __Lynx__
  if( processPresent(DSM_EXCLUSIVE_NAME) ) {
    fprintf(stderr,
	    "processPresent(%s) returned true; dsm already running?\n",
	    DSM_EXCLUSIVE_NAME);
    exit(QUIT_RTN);
  }
#else
#  warning  processPresent() not called
#endif


  /*****************************/
  /* read the allocation table */
  dsm_determine_alloc_file_location(alloc_file_name);

  s = dsm_read_allocation_file(&alloclist, &nmachines,
			       &classlist, &nclasses,
			       &sharelist, &nclassshares,
			       &my_alloc_version,
			       my_addresses,
			       my_num_addresses,
			       &my_address,
			       alloc_file_name);
  if(s != DSM_SUCCESS ) {
    fprintf(stderr, "dsm_main(): dsm_read_allocation_file() failed\n");
    exit(QUIT_RTN);
  }

  /* did we actually get any allocations? */
  if(alloclist == (struct alloc_list_head *)NULL) {
    fprintf(stderr, "\nNo allocations found involving local host; exiting\n");
    exit(QUIT_RTN);
  }
 
  /************************************************************/
  /* now we have the final form of the allocation tables, 
  ** so we can initialize the data in them
  */

  dprintf("Allocation tables constructed; initializing them\n");

  /* sort the allocation list heads by host address so we can find
     entries using bsearch() */
  qsort(alloclist,
	nmachines,
	sizeof(struct alloc_list_head),
	compare_alloc_list_head_addr);

  /* now make an index table for the allocation list, but */
  /* sorted by name  */
  alloclistbyname = (struct alloc_list_head **)
    malloc(nmachines*sizeof(struct alloc_list_head *));

  for(i=0; i<nmachines; i++) alloclistbyname[i] = &alloclist[i];
 
  qsort(alloclistbyname,
	nmachines,
	sizeof(struct alloc_list_head *),
	compare_alloc_list_head_name);


  for(i=0; i<nmachines; i++) {
    
    /* we start out with no connection to the other end */
    alloclist[i].cl = (CLIENT *)NULL;

    /* initialize the mutices which protect client operations */
    bzero((char *)&alloclist[i].connect_mutex, sizeof(pthread_mutex_t));
    s = pthread_mutex_init(&alloclist[i].connect_mutex, NULL);
    if(s!=0) {
      fprintf(stderr,	"pthread_mutex_init(connect_mutex): %s\n",
	      strerror(s));
      /* we can't run with things like this, but we want to try again,
	 so let smainit restart us */
      exit(SYSERR_RTN);
    }
 
    bzero((char *)&alloclist[i].client_call_mutex, sizeof(pthread_mutex_t));
    s = pthread_mutex_init(&alloclist[i].client_call_mutex, NULL);
    if(s!=0) {
      fprintf(stderr,	"pthread_mutex_init(client_call_mutex): %s\n",
	      strerror(s));
      /* we can't run with things like this, but we want to try again,
	 so let smainit restart us */
      exit(SYSERR_RTN);
    }

    /* no connection is in progress at first */
    alloclist[i].connect_in_progress = DSM_FALSE;
    

    /* and we don't know its version */
    alloclist[i].version = DSM_VERSION_UNKNOWN;

    /* initialize the mutex which protects the version status */
    bzero((char *)&alloclist[i].version_mutex, sizeof(pthread_mutex_t));
    s = pthread_mutex_init(&alloclist[i].version_mutex, NULL);
    if(s!=0) {
      fprintf(stderr,	"pthread_mutex_init(version_mutex): %s\n",
	      strerror(s));
      /* we can't run with things like this, but we want to try again,
	 so let smainit restart us */
      exit(SYSERR_RTN);
    }

    /* sort the allocation list by allocation name so we can find
       entries using bsearch() */
    dprintf("Sorting allocation list for host %s (0x%x)\n",
	    alloclist[i].machine_name,
	    alloclist[i].machine_addr);
      
    qsort(alloclist[i].allocs,
	  alloclist[i].nallocs,
	  sizeof(struct alloc_entry),
	  compare_alloc_entries);

    
    for(j=0; j<alloclist[i].nallocs; j++) {
      /* back pointer to alloc_list_head that owns us; we can't set
         these up any earlier because the sorting changes lots of
	 addresses */
      alloclist[i].allocs[j].owner = &alloclist[i];

      /* make space for the data */
      for(k=0; k<alloclist[i].allocs[j].n_elements; k++) {
	alloclist[i].allocs[j].elements[k].datap =
	  malloc(alloclist[i].allocs[j].elements[k].size);
	if(alloclist[i].allocs[j].elements[k].datap == NULL) {
	  fprintf(stderr, "Can't malloc(%d) %s:datap: %s\n",
		  alloclist[i].allocs[j].elements[k].size,
		  alloclist[i].allocs[j].elements[k].name,
		  strerror(errno));
	  /* we can't run with things like this, but we want to try again,
	     so let smainit restart us */
	  exit(SYSERR_RTN);
	}
      }

      /* at the beginning, there's been no update */
      alloclist[i].allocs[j].timestamp = (time_t)0;

      /* we start out with no one waiting */
      alloclist[i].allocs[j].notification_list =
	(struct notification_table_reference *)NULL;
      
      /* and no one accessing this allocation */
      bzero((char *)&alloclist[i].allocs[j].mutex, sizeof(pthread_mutex_t));
      s = pthread_mutex_init(&alloclist[i].allocs[j].mutex, NULL);
      if(s!=0) {
	fprintf(stderr,	"pthread_mutex_init(alloc_entry): %s\n",
		strerror(s));
	/* we can't run with things like this, but we want to try again,
	   so let smainit restart us */
	exit(SYSERR_RTN);
      }

    }
  }

  /*****************************/
  /* finish setup of sharelist */
  
  /* sort top level */
  dprintf("Sorting sharelist\n");
  qsort(sharelist,
	nclassshares,
	sizeof(struct class_share),
	compare_class_share_entries);

  dprintf("Finish constructing sharelist\n");
  for(i=0; i<nclassshares; i++) {
    /* sort alloc names */
    qsort(sharelist[i].csaep, 
	  sharelist[i].nallocs,
	  sizeof(struct class_share_alloc_entry),
	  compare_class_share_allocnames);
    
    /* hook up the alhp pointers for each of the hosts in each class */
    for(j=0; j<sharelist[i].class->nmembers; j++) {
      sharelist[i].alhpp[j] = 
	lookup_machine_name(sharelist[i].class->machinelist[j].machine_name);
      if(sharelist[i].alhpp[j] == (struct alloc_list_head *)NULL) {
	fprintf(stderr, "error: can't find host %s (class %s)\n",
		sharelist[i].class->machinelist[j].machine_name,
		sharelist[i].class->class_name);
	exit(SYSERR_RTN);
      }
    }

    /* sort the alhp pointers */
    qsort(sharelist[i].alhpp,
	  sharelist[i].class->nmembers,
	  sizeof(struct alloc_list_head *),
	  compare_alloc_list_head_name);

    /* finally, find every alloc_entry for each host in class */
    for(j=0; j<sharelist[i].nallocs; j++) {
      sharelist[i].csaep[j].aepp = (struct alloc_entry **)
	malloc(sharelist[i].class->nmembers * 
	       sizeof(struct alloc_entry *));
      if(sharelist[i].csaep[j].aepp == (struct alloc_entry **)NULL) {
	perror("malloc(csaep).aepp");
	exit(SYSERR_RTN);
      }

      for(k=0; k<sharelist[i].class->nmembers; k++) {
	sharelist[i].csaep[j].aepp[k] = 
	  lookup_allocation(sharelist[i].alhpp[k], 
			    sharelist[i].csaep[j].allocname);
	if(sharelist[i].csaep[j].aepp[k] == (struct alloc_entry *)NULL) {
	  fprintf(stderr,
		  "Error: can't find %s for host %s (class_share %s)\n",
		  sharelist[i].csaep[j].allocname,
		  sharelist[i].alhpp[k]->machine_name,
		  sharelist[i].class->class_name);
	} /* did lookup_allocation give NULL? */
      } /* loop over class members (k) */
    } /* loop over class_share allocs (j) */
  } /* loop over class_shares (i) */

  /***********************************************************************/
  /* make sure our hostname matches what's listed in the allocation file */
  /* (necessary because the gethostname() call gives different results   */
  /* on different platforms)                                             */
  k=0;
  for(i=0; i<nclasses; i++) {
    for(j=0; j<classlist[i].nmembers; j++) {
      if(classlist[i].machinelist[j].machine_addr == my_address) {
	if(strcmp(myhostname, classlist[i].machinelist[j].machine_name)) {
	  dprintf("Adjusting hostname from %s to %s\n",
		  myhostname, classlist[i].machinelist[j].machine_name);
	  strcpy(myhostname, classlist[i].machinelist[j].machine_name);
	  k = 1;
	  break;
	}
      }
    }
    if(k==1) break;
  }

  /****************/
  /* done; report */
  if(verbose) {
    fprintf(stderr, "\nAllocation summary by host:\n\n");

    for(i=0; i<nmachines; i++) {
      fprintf(stderr,
	      "Sharing %d allocation%s (max size %d) with machine %s:\n",
	     alloclist[i].nallocs,
	     alloclist[i].nallocs==1 ? "" : "s",
	     alloclist[i].largest,
	     alloclist[i].machine_name);
      for(j=0; j<alloclist[i].nallocs; j++)
	fprintf(stderr,
		"  n_elem=%-3d size=%-3d  name=%s\n",
		alloclist[i].allocs[j].n_elements,
		alloclist[i].allocs[j].size,
		alloclist[i].allocs[j].name);
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "\nAllocation summary by class:\n\n");
    for(i=0; i<nclassshares; i++) {
      fprintf(stderr, "Sharing %d allocation%s with class %s (%d host%s):\n",
	      sharelist[i].nallocs, 
	      sharelist[i].nallocs==1 ? "" : "s",
	      sharelist[i].class->class_name,
	      sharelist[i].class->nmembers,
	      sharelist[i].class->nmembers==1 ? "" : "s");
      for(j=0; j<sharelist[i].nallocs; j++) {
	fprintf(stderr, "  %s\n", sharelist[i].csaep[j].allocname);
      }
    }
    fprintf(stderr, "\n");
  }

  /*********************************/
  /* get set up to do notification */
  largest_largest = 0;
  for(i=0; i<nmachines; i++)
    largest_largest = 
      alloclist[i].largest > largest_largest 
      ? alloclist[i].largest
      : largest_largest;
  
  s = dsm_initialize_notification_table(largest_largest);
  if(s != DSM_SUCCESS) exit(QUIT_RTN);

  bzero((char *)&notification_ring_mutex, sizeof(pthread_mutex_t));
  s = pthread_mutex_init(&notification_ring_mutex, NULL);
  if(s!=0) {
    fprintf(stderr, "pthread_mutex_init(notification_ring) returns %d\n", s);
    exit(QUIT_RTN);
  }


  /*********************************************************************/
  /* set up the RPC client request queue, the in-progress queue, and the
     thread pool which will handle the request in the queue */
  for(i=0; i<DSM_REQ_QUEUE_LENGTH; i++) {
    request_queue[i]   = (struct decoded_request_args *)NULL;
    inprogress_list[i] = (struct decoded_request_args *)NULL;
  }

  request_queue_entries   = 0;
  inprogress_list_entries = 0;

  bzero((char *)&request_queue_mutex, sizeof(pthread_mutex_t));
  s = pthread_mutex_init(&request_queue_mutex, NULL);
  if(s!=0) {
    fprintf(stderr,
	    "pthread_mutex_init(request_queue_mutex): %s\n",
	    strerror(s));
    exit(QUIT_RTN);
  }

  s = sem_init(&request_queue_semaphore, 0, 0);
  if(s!=0) {
    perror("sem_init(request_queue_semaphore)");
    exit(QUIT_RTN);
  }


  /* set up the thread pool */
  for(i=0; i<DSM_RPC_THREAD_POOL_COUNT; i++) {
    s = pthread_create(&thread,
		       NULL,
		       (void *)remote_write_giver,
		       (void *)i);
    if(s!=0)
      fprintf(stderr,
	      "dsm_main(): pthread_create(threadpool %d) returned %d\n",
	      i, s);
    else {
      s = pthread_detach(thread);
      if(s!=0)
	fprintf(stderr,
		"dsm_main(): pthread_detach(threadpool %d) returned %d\n",
		i, s);
    }
  }



  /*****************/
  /* start serving */
  /*****************/

  
  /* just to be safe */
  pmap_unset(DSM, DSM_VERS);

  /* create TCP service transport handle with default buffers */
  transp = svctcp_create(RPC_ANYSOCK, 0, 0);
  if(transp == (SVCXPRT *)NULL) {
    fprintf(stderr, "svctcp_create() failed\n");
    exit(QUIT_RTN);
  }
  dprintf("Created TCP service transport handle\n");


  /* register the dispatcher */
  if( svc_register(transp, DSM, DSM_VERS, dsm_dispatcher, IPPROTO_TCP)
      == FALSE) {
    fprintf(stderr, "svc_register() failed\n");
    exit(QUIT_RTN);
  }
  dprintf("Registered dsm service\n");


  /* let 'er rip */
  dprintf("Starting server\n\n");
  svc_run();

  
  /* we shouldn't get here */
  fprintf(stderr, "svc_run() returned\n");
  exit(QUIT_RTN);

}
