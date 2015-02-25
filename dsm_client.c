/*
** dsm_client.c
**
** Contains functions which clients use to get services from the SMA
** distributed shared memory (dsm) server.  These are used both
** internally by the server to make client calls to other servers, and
** are provided to users through a static link library.
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_client.c,v 2.8 2009/06/12 16:09:58 ckatz Exp $
*/

#define __LYNXOS
#include <sys/file.h>
#undef __LYNXOS

#include <rpc/rpc.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>

#ifdef __Lynx__
#  include <socket.h>
#  include <net/tcp.h>
#  include <proc.h>
#  include <info.h>
#endif

#ifdef __linux__
#  include <netinet/tcp.h>
#endif

#define _DSM_INTERNAL
#include "dsm.h"
#undef  _DSM_INTERNAL

/* It appears that the Nagle-algorithm-disabling-code is not necessary
   in the client; disabling the algorithm in the server seems to remove
   the 200ms delay problem; however, since I wrote the code somewhat
   painstakingly, I'll leave it in here in case I ever need it, and turn
   it off by removing the DSM_NONAGLE macro */
#undef DSM_NONAGLE

static char rcsid[] = "$Id: dsm_client.c,v 2.8 2009/06/12 16:09:58 ckatz Exp $";

extern int verbose;
extern int dprintf(char *, ...);


/* timeout for client calls */
static struct timeval timeout = DSM_CLIENT_CALL_TIMEOUT;

/* client handle to the local server, through which all requests go */
/* (NULL value means not connected) */
static CLIENT *cl = (CLIENT *)NULL;
static int useropen = DSM_FALSE;

/* the monitor lists go here */
static char *hostname_list  = (char *)NULL;
static char *allocname_list = (char *)NULL;
static dsm_structure **struct_buffer_list = (dsm_structure **)NULL;
static u_int nmonitor=0; /* number of allocations in monitor list */
static pthread_mutex_t monlist_mutex;

/* everyone uses the shared memory */
static void *shared_memory=NULL;
static int shm_descr;
static int shm_size;


static struct notification_table_entry *notification_table;
static char   *data_buffers;
static int    *dsm_nproc_notify_max_p;
static size_t *max_buffer_size_p;

/* this is a list of just the alloc names, like that returned by
   dsm_get_allocation_list(); we keep an internal copy */
struct dsm_allocation_list *allocnamelist = (struct dsm_allocation_list *)NULL;

int nmachines=0; /* number of machines with which we share; used in
		    dsm_get_allocation_list(); global because it's also
		    used in the xdr filter (ugh!) */

/***********************************************************/

/* Since all these functions share a pool of global variables, they're
** not threadsafe without protection.  This is the clunkiest attempt at 
** threadsafety I can think of.  Only one function at a time can run.
** Ugh.  If this degrades performance badly, it will have to be
** improved.
*/
pthread_mutex_t function_call_mutex = PTHREAD_MUTEX_INITIALIZER;

/* this provides extra safety, but is not foolproof; functions which are
   called internally require that the function_call_mutex be locked
   already; this checks and aborts if it's not locked
*/
#define ABORT_IF_MUTEX_UNLOCKED(mutex_addr, function_name) {      \
  int fgh744;                                                     \
  dprintf("%s(): trylock " #mutex_addr "\n", function_name);      \
  fgh744 = pthread_mutex_trylock(mutex_addr);                     \
  if(fgh744==0) {                                                 \
    dprintf("%s(): " #mutex_addr " not locked\n", function_name); \
    MUTEX_UNLOCK(mutex_addr, function_name, "");                  \
    return(DSM_INTERNAL_ERROR);                                   \
  }                                                               \
  else if(fgh744!=EBUSY) {                                        \
    dprintf("%s(): pthread_mutex_trylock() returned %d\n",        \
	    function_name,fgh744);                                \
    return(DSM_INTERNAL_ERROR);                                   \
  }                                                               \
}

 


/******************************************/
/* we maintain a list of allocation info; */
/* this keeps us from always asking the   */
/* server the info; we reinitialize the   */
/* list on any dsm_open(), and destroy it */
/* in dsm_close()                         */
struct dsm_local_structure_info *alloc_info_list;
static int                       alloc_info_list_length;

/* a function for sorting and finding things in alloc_info_list; sorts
 first by machine name, then by allocation name */
static int compare_alloc_info_entries(const void *p1, const void *p2) {
  return( strcmp( 
		 ((struct dsm_local_structure_info *)p1)->name,
		 ((struct dsm_local_structure_info *)p2)->name
		)
	);
}

#ifdef DSM_NONAGLE
/* In order to avoid the 200ms problem, we want to disable the Nagle
** algorithm on the socket used by RPC.  Normal RPC implementations
** provide clnt_control(cl, CLGET_FD, &sd) to get this, but LynxOS does
** not. Thus we need to fudge it in this rather convoluted way.
**
** This function will write into list all the file descriptors which
** point to sockets (BSOK)
** list must be an array of info(SYS_NFDS) ints
*/
static int get_socket_list(int *list) {
  __pentry procentry;
  struct pssentry pss;
  struct file f;
  long proctabaddr,proctabsize;
  pid_t pid;
  int i,j,fd,s,foundpid=0;

  proctabaddr = info(_I_PROCTAB);
  proctabsize = info(_I_NPROCTAB);
  pid         = getpid();

  fd = open("/dev/mem", O_RDONLY);
  if(fd==-1) {
    perror("get_socket_list: open()");
    return(DSM_ERROR);
  }

  /* loop through process table to find our own entry */
  for(i=0; i<proctabsize; i++) {
    s = lseek(fd, proctabaddr + i*sizeof(__pentry), 0);
    if(s == -1) {
      perror("get_socket_list: lseek(process table)");
      close(fd);
      return(DSM_ERROR);
    }
    
    s = read(fd, &procentry, sizeof(__pentry));
    if(s!=sizeof(__pentry)) {
      perror("get_socket_list: read(proctab entry)");
      close(fd);
      return(DSM_ERROR);
    }

    if(pid != procentry.pid) {
      continue;
    }
    foundpid = 1;

    s = lseek(fd, (int)(procentry.pss), 0);
    if(s == -1) {
      perror("get_socket_list: lseek(pss)");
      close(fd);
      return(DSM_ERROR);
    }

    s = read(fd, &pss, sizeof(struct pssentry));
    if(s!=sizeof(struct pssentry)) {
      perror("get_socket_list: read(pss)");
      close(fd);
      return(DSM_ERROR);
    }

    /* loop over the file descriptors looking for sockets */
    for(j=0; j<SYS_NFDS; j++) {
      s = lseek(fd, (int)(pss.fds[j].f), 0);
      if(s == -1) {
	perror("get_socket_list: lseek(fds)");
	close(fd);
	return(DSM_ERROR);
      }

      s = read(fd, &f, sizeof(struct file));
      if(s != sizeof(struct file)) {
	perror("get_socket_list: read(struct file)");
	close(fd);
	return(DSM_ERROR);
      }

      if(f.dev_type == DT_BSDSOCKET) list[j] = 1; /* we found a socket! */
      else                           list[j] = 0;

    } /* loop over file descriptors */
  } /* loop over process table */

  if(foundpid == 0) {
    fprintf(stderr,
	    "get_socket_list: pid %d not found in proctab\n",
	    (int)pid);
    close(fd);
    return(DSM_ERROR);
  }

  close(fd);
  return(DSM_SUCCESS);
}
#endif

/****************************************/
/* dummy functions to placate the flock */
/****************************************/

int dsm_open(void) {
  int dsm_init(void);

  int s;

  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_open", "");

  if(useropen == DSM_FALSE) {
    /* okay, if they're calling legitimately,  we might as well go ahead
       and try to open it */

    s = dsm_init();
    if(s==DSM_SUCCESS) useropen = DSM_TRUE;

    MUTEX_UNLOCK(&function_call_mutex, "dsm_open", "");
    return(s);
  }
  else {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_open", "");
    return(DSM_ALREADY_OPEN);
  }
}


int dsm_close(void) {
  int dsm_shutdown(void);
  int s;

  MUTEX_LOCK(&function_call_mutex, "dsm_close", "");

  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_close", "");
    return(DSM_NO_INIT);
  }
  else {
    useropen = DSM_FALSE;
    s = dsm_shutdown();
    MUTEX_UNLOCK(&function_call_mutex, "dsm_close", "");
    return(s);
  }
}


/*****************************/
/* initialize dsm operations */
/*****************************/

/* YOU MUST ALREADY HOLD function_call_mutex before calling this */
int dsm_init(void) {
#ifdef DSM_NONAGLE
  int sockets_before[SYS_NFDS],sockets_after[SYS_NFDS],gotsocklist=0;
  int i,sock;
#endif
  int s;
  size_t max_buffer_size;

  /*********/
  /* Begin */
  /*********/

  /* make sure function_call_mutex is held; note that this is not
     foolproof */
  ABORT_IF_MUTEX_UNLOCKED(&function_call_mutex, "dsm_init");

  /* sometimes RPC calls fail internally with a SIGPIPE; we're supposed
     to be detecting broken connections, so we don't want to die from
     SIGPIPE 
  */
  signal(SIGPIPE, SIG_IGN);

  /* first we have to open the shm to get the max_buffer_size value so
     we can then determine the correct size */
  if(shared_memory == NULL) {
    dprintf("dsm_init(): opening shared memory for max_buffer_size\n");
    shm_descr = shm_open(DSM_NOTIFY_TABLE_SHM_NAME, O_RDWR
#ifdef __linux__
			 ,0
#endif
			 );

    if(shm_descr == -1) {
      dprintf("dsm_init(): shm_open() returns errno=%d\n", errno);
      return(DSM_NO_RESOURCE);
    }

    /* map it */
    dprintf("dsm_init(): mapping shm for max_buffer_size\n");
    shared_memory = mmap(0,
			 sizeof(*dsm_nproc_notify_max_p)
			 + sizeof(*max_buffer_size_p),
			 PROT_READ | PROT_WRITE,
			 MAP_SHARED, shm_descr, (off_t)0);
    if(shared_memory == MAP_FAILED) {
      close(shm_descr);
      shared_memory = NULL;
      return(DSM_NO_RESOURCE);
    }

    /* now we can get the value of max_buffer_size */
    max_buffer_size = *( (size_t *)(shared_memory 
				    + sizeof(*dsm_nproc_notify_max_p)));
    dprintf("dsm_init(): max_buffer_size = %d\n", max_buffer_size);

    /* now we gotta close up the shm so we can reopen it at the correct
       size */
    munmap(shared_memory, 
	   sizeof(*dsm_nproc_notify_max_p) + sizeof(*max_buffer_size_p));
    close(shm_descr);
    

  
    /* reopen the shm */
    dprintf("dsm_init(): opening shared memory\n");
    shm_descr = shm_open(DSM_NOTIFY_TABLE_SHM_NAME, O_RDWR
#ifdef __linux
			 ,0
#endif
			 );

    if(shm_descr == -1) return(DSM_NO_RESOURCE);
    
    shm_size =
      sizeof(*dsm_nproc_notify_max_p)
      + sizeof(*max_buffer_size_p)
      + DSM_NPROC_NOTIFY_MAX * sizeof(struct notification_table_entry)
      + DSM_NPROC_NOTIFY_MAX * max_buffer_size;


    /* set shm size */
    dprintf("dsm_init(): setting shared memory size to %d\n", shm_size);
    s = ftruncate(shm_descr, (off_t)shm_size);
    if(s!=0) {
      s = close(shm_descr);
      return(DSM_NO_RESOURCE);
    }

    /* map it */
    shared_memory = mmap(0, shm_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, shm_descr, (off_t)0);
    if(shared_memory == MAP_FAILED) {
      close(shm_descr);
      shared_memory = NULL;
      return(DSM_NO_RESOURCE);
    }
    
    /* set up the pointers into the shared memory */
    dsm_nproc_notify_max_p = (int *)shared_memory;

    max_buffer_size_p = (size_t *)(shared_memory
				   +sizeof(*dsm_nproc_notify_max_p));
    
    notification_table =
      (struct notification_table_entry *)(shared_memory 
					  + sizeof(int)
					  + sizeof(size_t));

    data_buffers = (char *)notification_table
      + *dsm_nproc_notify_max_p * sizeof(*notification_table);

  } /* if shared memory is not open */

  /* make sure the number values are consistent */
  if(*dsm_nproc_notify_max_p != DSM_NPROC_NOTIFY_MAX
     ||
     *max_buffer_size_p != max_buffer_size) {

    dprintf("dsm_init(): internal inconsistency detected\n");
    dprintf("dsm_init(): *dsm_nproc_notify_max_p = %d\n",
	    *dsm_nproc_notify_max_p);
    dprintf("dsm_init(): DSM_NPROC_NOTIFY_MAX = %d\n",
	    DSM_NPROC_NOTIFY_MAX);
    dprintf("dsm_init(): *max_buffer_size_p = %d\n",
	    *max_buffer_size_p);
    dprintf("dsm_init(): max_buffer_size = %d\n",
	    max_buffer_size);
    close(shm_descr);
    shared_memory = NULL;
    return(DSM_INTERNAL_ERROR);
  }
    

  /* shared memory is set up; now connect to the server */
  
  /* if we already have a client handle, don't make a new one */
  if(cl != (CLIENT *)NULL) return(DSM_ALREADY_OPEN);
  
  /* make a connection; we want to find out the socket descriptor used,
     so before making the connection, get the list of sockets */
#ifdef DSM_NONAGLE
  s = get_socket_list(sockets_before);
  if(s==DSM_SUCCESS) gotsocklist=1;
#endif
    
  cl = clnt_create("localhost", DSM, DSM_VERS, "tcp");

  /* did it fail? */
  if(cl == (CLIENT *)NULL) {
    munmap(shared_memory, shm_size);
    close(shm_descr);
    shared_memory = NULL;
    clnt_pcreateerror("dsm_init(): clnt_create()");
    return(DSM_UNAVAILABLE);
  }

#ifdef DSM_NONAGLE
  if(gotsocklist) s = get_socket_list(sockets_after);
  if(s!=DSM_SUCCESS) gotsocklist = 0;
#endif
  
  /* adjust the timeout */
  if( clnt_control(cl, CLSET_TIMEOUT, (char *)&timeout) != TRUE)
    fprintf(stderr, "dsm_init(): clnt_control(CLSET_TIMEOUT) failed\n");

#ifdef DSM_NONAGLE
  /* disable the Nagle algorithm on the socket */
  if(gotsocklist == 1) {
    sock = -1;
    for(i=0; i<SYS_NFDS; i++) {
      if(sockets_before[i]!=1 && sockets_before[i]!=sockets_after[i]) {
	if(sock==-1) sock = i;
	else {
	  fprintf(stderr, "dsm_init(): multiple sockets created\n");
	  gotsocklist=0;
	}
      }
    }
    
    if(sock != -1) {
      if(gotsocklist==1) {
	i = 1;
	s = setsockopt(sock, 
		       IPPROTO_TCP, TCP_NODELAY, (char *)&i, sizeof(int));
      if(s < 0) perror("dsm_init(): setsockopt");
      }
    }
    else {
      fprintf(stderr, "dsm_init(): client handle socket not found\n");
    }
  }
#endif
  
  alloc_info_list = (struct dsm_local_structure_info *)NULL;
  alloc_info_list_length = 0;

  s = pthread_mutex_init(&monlist_mutex, NULL);
  if(s!=0 && s!=EBUSY)
    fprintf(stderr, "dsm_init(): pthread_mutex_init() returns %d\n", s);

  /* retrieve the allocation list */
  s = dsm_get_allocation_list_unlocked(&nmachines, &allocnamelist);
  if(s!=DSM_SUCCESS) {
    fprintf(stderr, "dsm_init(): dgaeu() returned %d\n", s);
  }


  /* and the light shone down */
  return(DSM_SUCCESS);
}

/****************************/
/* shut down dsm operations */
/****************************/

/* YOU MUST ALREADY HOLD function_call_mutex before calling this */
int dsm_shutdown(void) {
  int i,j;

  /*********/
  /* Begin */
  /*********/

  /* make sure function_call_mutex is held; note that this is not
     foolproof */
  ABORT_IF_MUTEX_UNLOCKED(&function_call_mutex, "dsm_shutdown");
 
  /* is it initialized? */
  if(cl == (CLIENT *)NULL) return(DSM_NO_INIT);

  /* yep.  undo it. */
  clnt_destroy(cl);
  cl = (CLIENT *)NULL;

  /* throw out the allocation name list */
  dsm_destroy_allocation_list_unlocked(&allocnamelist);

  /* throw out the allocation info list */
  for(i=0; i<alloc_info_list_length; i++) {
    for(j=0; j<alloc_info_list[i].n_elements; j++) {
      free(alloc_info_list[i].elements[j].datap);
    }
  }
  free(alloc_info_list);
  alloc_info_list_length = 0;

  /* close up the shared memory space */
  munmap(shared_memory, shm_size);
  close(shm_descr);
  shared_memory = NULL;

  return(DSM_SUCCESS);
}

/************************************************/
/* make the client call; if it fails, retry     */
/* the connection and try the client call       */
/* again; this is just like clnt_call(), but it */
/* does some dsm-specific stuff and returns a   */
/* dsm status code                              */
static int dsm_client_call_with_retry(u_long procnum,
				      xdrproc_t inproc,
				      char *in,
				      xdrproc_t outproc,
				      char *out,
				      struct timeval timeout) {
  int dsm_init(void);
  int dsm_shutdown(void);

  enum clnt_stat rpcstat;

  /*********/
  /* Begin */
  /*********/

  /* make sure function_call_mutex is held; note that this is not
     foolproof */
  ABORT_IF_MUTEX_UNLOCKED(&function_call_mutex, "dsm_client_call_with_retry");
  
  /* if the connection was good last time we used it, make the call */
  if(cl != (CLIENT *)NULL) {
    rpcstat = clnt_call(cl, procnum, inproc, in, outproc, out, timeout);
    if(rpcstat != RPC_SUCCESS) {
      /* destroy the connection */
      dprintf("dsm_client_call_with_retry(): destroying connection\n");
      dsm_shutdown();

      /* give the server a chance to get set up */
      sleep(1);
    }
  }
  else {
    /* if we already know the connection is bad, just try 
       to reconnect */
    dprintf("dsm_client_call_with_retry(): attempting to connect\n");
    rpcstat = RPC_FAILED;
  }

  if(rpcstat != RPC_SUCCESS) {

    /* if the server connection is bad, we must try to connect */
    if( dsm_init() != DSM_SUCCESS) {
      /* don't make other client calls fail; just make them try to
	 reconnect */
      dprintf("dsm_client_call_with_retry(): couldn't connect\n");

      return(DSM_NO_LOCAL_SVC);
    }
    else {
      dprintf("dsm_client_call_with_retry(): "
	      "successfully connected; trying request\n");
      
      /* try it again with the new connection */
      rpcstat = clnt_call(cl, procnum, inproc, in, outproc, out, timeout);
  
      
      if(rpcstat != RPC_SUCCESS) {
	/* still didn't work; give up */
	clnt_perror(cl, "dsm_client_call_with_retry()");

        return(DSM_NO_LOCAL_SVC);
      }
    }
  }

  return(DSM_SUCCESS);
}


/*************************************************/
/* find out the details of an allocation without */
/* actually getting the values; details are      */
/* returned in a dsm_local_structure_info object */
/*************************************************/

/* YOU MUST ALREADY HOLD function_call_mutex before calling this */

/* for internal use only */
int dsm_get_info(char                            *targetname,
		 char                            *allocname,
		 struct dsm_local_structure_info **dlsipp       ) {
  struct dsm_transaction           req, reply;
  struct dsm_local_structure_info *lsip,lsi;
  int                              i,s;

  /*********/
  /* Begin */
  /*********/

  /* make sure function_call_mutex is held; note that this is not
     foolproof */
  ABORT_IF_MUTEX_UNLOCKED(&function_call_mutex, "dsm_get_info");

  /* is it initialized? */
  if(useropen == DSM_FALSE) return(DSM_NO_INIT);

  /* first see whether the info is already in our internal table */
  dprintf("dsm_get_info(): check list for %s:%s\n", targetname, allocname);
  strcpy(lsi.name, allocname);
  lsip = (struct dsm_local_structure_info *)
    bsearch(&lsi,
	    alloc_info_list,
	    alloc_info_list_length,
	    sizeof(struct dsm_local_structure_info),
	    compare_alloc_info_entries);
  if(lsip != (struct dsm_local_structure_info *)NULL) {
    /* it was already in the list */
    dprintf("dsm_get_info(): found alloc info in internal list\n");
    *dlsipp = lsip;
    return(DSM_SUCCESS);
  }

  /* it wasn't in the list; ask the server */
  dprintf("dsm_get_info(): info not found in internal list; asking server\n");

  /* note that this ordering is okay; even if the value we have in the
     list is stale, when we make the client call using the stale value,
     we will find out about the spoilage.  Then the info list will get
     destroyed and the new info read from the server. */
  req.targetname  = targetname;
  req.allocname   = allocname;

  /* for a query, there are no data */
  req.data_size    = 0;
  req.n_elements   = 0;
  req.is_structure = 0;
  req.elements     = (struct alloc_element *)NULL;
  req.timestamp    = 0;
  req.notify       = DSM_FALSE;
  req.status       = DSM_SUCCESS;

  /* make filter allocate space */
  reply.targetname  = (char *)NULL;
  reply.allocname   = (char *)NULL;
  reply.elements    = (struct alloc_element *)NULL;
  
  dprintf("dsm_get_info(): making DSM_ALLOC_INFO_QUERY client call\n");
  s = dsm_client_call_with_retry(DSM_ALLOC_INFO_QUERY,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&req,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&reply,
				 timeout);
  if(s!=DSM_SUCCESS) {
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    return(DSM_RPC_ERROR);
  }

  /* even if the RPC works, there can be a dsm error */
  if(reply.status != DSM_SUCCESS) {
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    return(reply.status);
  }

  /* it worked! copy the info from the reply into the local storage list */
  dprintf("dsm_get_info(): got info from server; reallocing list\n");
  alloc_info_list = (struct dsm_local_structure_info *)
    realloc(alloc_info_list,
	   (alloc_info_list_length+1)*sizeof(struct dsm_local_structure_info));
  if(alloc_info_list == (struct dsm_local_structure_info *)NULL) {
    perror("dsm_get_info(): realloc()");
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    return(DSM_INTERNAL_ERROR);
  }
  lsip = &alloc_info_list[alloc_info_list_length];

  strcpy(lsip->name, allocname);
  lsip->size         = (size_t)reply.data_size;
  lsip->n_elements   = (int)reply.n_elements;
  lsip->is_structure = (int)reply.is_structure;

  lsip->elements = malloc(lsip->n_elements * sizeof(*lsip->elements));
  if(lsip->elements == NULL) {
    perror("dsm_get_info(): malloc(lsip->elements)");
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    return(DSM_INTERNAL_ERROR);
  }

  for(i=0; i<lsip->n_elements; i++) {
    strcpy(lsip->elements[i].name, reply.elements[i].name);
    lsip->elements[i].size  = reply.elements[i].size;
    lsip->elements[i].datap = NULL; /* we don't keep actual data here */
  }
  alloc_info_list_length++;

  /* it's on the bottom of the list; now re-sort it */
  qsort(alloc_info_list,
	alloc_info_list_length,
	sizeof(struct dsm_local_structure_info),
	compare_alloc_info_entries);

  /* find it again so we can send it back to the caller */
  lsip = (struct dsm_local_structure_info *)
    bsearch(&lsi,
	    alloc_info_list,
	    alloc_info_list_length,
	    sizeof(struct dsm_local_structure_info),
	    compare_alloc_info_entries);
  if(lsip == (struct dsm_local_structure_info *)NULL) {
    /* how could we not find it when we just put it in? */
    fprintf(stderr, "dsm_get_info(): entry not found after adding\n");
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    return(DSM_INTERNAL_ERROR);
  }

  /* got it; give it to the caller */
  *dlsipp = lsip;

  if(verbose==DSM_TRUE) {
    fprintf(stderr, "dsm_get_info(): new alloc info list:\n");
    for(i=0; i<alloc_info_list_length; i++)
      fprintf(stderr, "  %d: %s %d elements\n",
	     i, 
	     alloc_info_list[i].name,
	     alloc_info_list[i].n_elements);
  }

  /* free the memory allocated by the filter */
  xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);

  dprintf("dsm_get_info(): finished\n");
  return(DSM_SUCCESS);
}
  
/*************************/
/* read a value from dsm */
/*************************/
int dsm_read(char   *hostname,
	     char   *allocname,
	     void   *datump,
	     time_t *timestamp) {
  
  struct dsm_transaction           req, reply;
  struct dsm_local_structure_info *lsip;
  struct timeval                   now, limit=DSM_CLIENT_READ_RETRY_TIMEOUT;
  struct timezone                  dummy_tz;
  double                           timestart, timenow, timelimit;
  int                              i, s;

  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_read", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read", "");
    return(DSM_NO_INIT);
  }

  /* fill in the request data */
  req.targetname  = hostname;
  req.allocname   = allocname;
  
  /* for a read, there are no data */
  req.data_size    = 0;
  req.n_elements   = 0;
  req.is_structure = 0;
  req.timestamp    = 0;            /* dummy */
  req.notify       = DSM_FALSE;    /* dummy */
  req.status       = DSM_SUCCESS;  /* dummy */

  /* make the filter allocate space */
  reply.targetname  = (char *)NULL;
  reply.allocname   = (char *)NULL;
  reply.elements    = (struct alloc_element *)NULL;

  s = dsm_client_call_with_retry(DSM_LOCAL_READ,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&req,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&reply,
				 timeout);
  if(s!=DSM_SUCCESS) {
    /* free the memory allocated by the filter */
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read", "");
    return(DSM_RPC_ERROR);
  }

  /* even if the RPC works, there can be a dsm error */
  if(reply.status == DSM_NO_CONNECTION) {
    gettimeofday(&now, &dummy_tz);
    timestart = (double)now.tv_sec     + (double)now.tv_usec/1000000.0;
    timelimit = (double)limit.tv_sec + (double)limit.tv_usec/1000000.0;

    while(reply.status == DSM_NO_CONNECTION) {
      /* we had no connection to the remote machine, so we might not have
	 the most up to date data; if the server detects this condition,
	 it makes the connection and gets the data, so we should ask again
	 to get the new value */
      
      /* give the new connection a chance to get established; we'll keep
	 trying until we stop getting DSM_NO_CONNECTION or until we've
	 waited half of DSM_CLIENT_CALL_TIMEOUT, whichever comes first */
      usleep(100000);
      
      s = dsm_client_call_with_retry(DSM_LOCAL_READ,
				     (xdrproc_t)xdr_dsm_transaction,
				     (char *)&req,
				     (xdrproc_t)xdr_dsm_transaction,
				     (char *)&reply,
				     timeout);
      if(s!=DSM_SUCCESS) {
	/* free the memory allocated by the filter */
	xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
	MUTEX_UNLOCK(&function_call_mutex, "dsm_read", "");
	return(DSM_RPC_ERROR);
      }

      gettimeofday(&now, &dummy_tz);
      timenow = (double)now.tv_sec + (double)now.tv_usec/1000000.0;
      if(timenow - timestart > timelimit) break;
    }
  }

  if(reply.status!=DSM_SUCCESS && reply.status!=DSM_NO_CONNECTION) {
    /* it didn't work, and not because there was no connection */

    /* free the memory allocated by the filter */
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read", "");
    return(reply.status);
  }
  
  /* it worked! (or timed out waiting for a connection, so we get the
     local value, possibly not up to date) */

  /* copy the data into the user's buffer; we have to copy differently
     depending on whether it's a structure */
  if(reply.is_structure == DSM_TRUE) {
    /* it's a structure; copy the data one element at a time */
    lsip = (struct dsm_local_structure_info *)datump;
    for(i=0; i<reply.n_elements; i++) {
      memcpy(lsip->elements[i].datap,
	     reply.elements[i].datap,
	     reply.elements[i].size);
    }
  }
  else {
    /* it's not a structure; copy right into the provided buffer */
    memcpy(datump, reply.elements[0].datap, reply.elements[0].size);
  }

  /* if requested, copy the timestamp into the user's buffer */
  if(timestamp != NULL) *timestamp = (time_t)(reply.timestamp);

  /* free the memory allocated by the filter */
  xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);

  MUTEX_UNLOCK(&function_call_mutex, "dsm_read", "");
  return(DSM_SUCCESS);
}

/*************************************/
/* wait for a value in dsm to change */
/*************************************/
int dsm_read_wait(char *hostname,
		  char *allocname,
		  void *datump) {
  struct dsm_transaction reply;
  struct dsm_read_wait_req rwr;
  int s, sem_count, accum_size, i, j;

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  struct sembuf sbuf;
  union semun dummy_semarg;
#endif

  struct dsm_local_structure_info *lsip;

  int notification_table_index;

  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_read_wait", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(DSM_NO_INIT);
  }

  /* is there anything on the monitor list? */
  if(nmonitor==0) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(DSM_MON_LIST_EMPTY);
  }

  /* fill in the request data */
  rwr.hostnames   = hostname_list;
  rwr.allocnames  = allocname_list;
  rwr.string_size = DSM_NAME_LENGTH;
  rwr.num_alloc   = nmonitor;
  rwr.pid         = (u_int)getpid();

#ifdef __Lynx__
  rwr.tid         = (u_int)pthread_self();
#elif defined __linux__
  rwr.tid         = (u_int)pthread_self();
#else
#  error "Don't know pthread_t type on this platform"
#endif

  /* make the filter allocate memory for the reply */
  reply.targetname  = (char *)NULL;
  reply.allocname   = (char *)NULL;
  reply.elements    = (struct alloc_element *)NULL;

  s = dsm_client_call_with_retry(DSM_LOCAL_READ_WAIT,
				 (xdrproc_t)xdr_dsm_read_wait_req,
				 (char *)&rwr,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&reply,
				 timeout);
  if(s!=DSM_SUCCESS) {
    /* free the memory allocated by the filter */
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(DSM_RPC_ERROR);
  }
 
  /* even if the RPC works, there can be a dsm error */
  if(reply.status != DSM_SUCCESS) {
    /* free the memory allocated by the filter */
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(reply.status);
  }
  
  /* it worked! the server returned to us an index into the notification
   table */
  notification_table_index = *((int *)reply.elements[0].datap);

  /* free the memory allocated by the filter */
  xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);

  /* now, having the index, we can wait on the right semaphore */
  dprintf("dsm_read_wait(): waiting on sem %d\n",
	  notification_table_index);

  if(verbose==DSM_TRUE) {
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
    s = semctl(notification_table[notification_table_index].semid,
	       notification_table_index,
	       GETNCNT,
	       &dummy_semarg);
    if(s==-1) perror("dsm_read_wait(): semctl(GETNCNT)");
    else
      sem_count = s;
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
    s = sem_getvalue(&notification_table[notification_table_index].sem,
		     &sem_count);
#endif
  }

  /* WARNING WARNING WARNING  this could be non-threadsafe; what things
  ** can change while we're waiting for the semaphore?
  */
  MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  sbuf.sem_num = notification_table_index;
  sbuf.sem_op  = -1;
  sbuf.sem_flg =  0;
  s = semop(notification_table[notification_table_index].semid, &sbuf, 1);

  MUTEX_LOCK(&function_call_mutex, "dsm_read_wait", "");

  if(s!=0) {
    dprintf("dsm_read_wait(): semop() returned error\n");
    if(errno!=EINTR) {
      MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
      return(DSM_INTERNAL_ERROR);
    }
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(DSM_INTERRUPTED);
  }
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
  s = sem_wait(&notification_table[notification_table_index].sem);
  MUTEX_LOCK(&function_call_mutex, "dsm_read_wait", "");

  if(s!=0) {
    dprintf("dsm_read_wait(): sem_wait() returned error %d\n", errno);
    if(errno!=EINTR) {
      MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
      return(DSM_INTERNAL_ERROR);
    }

    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(DSM_INTERRUPTED);
  }
#endif

  /* the semaphore was posted; check whether the first byte of the
     hostname is zero, indicating an abnormal return */

  if(notification_table[notification_table_index].hostname[0] == '\0') {

    dprintf("dsm_read_wait(): abnormal return from sem_wait()\n");

    /* abnormal return; what kind? allocname, taken as an int, tells us */
    if( *((int *)(notification_table[notification_table_index].allocname))
	== DSM_INTERNAL_CHANGE) {
      /* there's an internal inconsistency caused by somebody
	 recompiling; we have to shut down our shm operations and start
	 again; however, we first wait a little while so that the server
	 has time to redo the shared memory */
      dprintf("dsm_read_wait(): internal change reported; restarting\n");
      dsm_shutdown();
      
      sleep(1);
      dsm_init();
    }

    /* now we must resubmit the monitor list and wait again */
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return( dsm_read_wait(hostname, allocname, datump) );
  }

  /* the semaphore was posted normally; we can get the value from the
     buffer */ 
  dprintf("dsm_read_wait(): normal return from sem wait; copy hostname to client buffer\n");
  strncpy(hostname, 
	  notification_table[notification_table_index].hostname,
	  DSM_NAME_LENGTH);

  dprintf("dsm_read_wait(): copying allocname to client buffer\n");
  strncpy(allocname, 
	  notification_table[notification_table_index].allocname,
	  DSM_NAME_LENGTH);

  dprintf("dsm_read_wait(): checking info for %s:%s\n", hostname, allocname);
  s = dsm_get_info(hostname, allocname, &lsip);
  if(s!=DSM_SUCCESS) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
    return(s);
  }

  if(lsip->is_structure == DSM_TRUE) {
    /* find the buffer that the caller provided */
    for(i=0; i<nmonitor; i++) {
      if(!strncmp(hostname,
		  hostname_list+i*DSM_NAME_LENGTH, 
		  DSM_NAME_LENGTH)
	 &&
	 !strncmp(allocname, 
		  allocname_list+i*DSM_NAME_LENGTH,
		  DSM_NAME_LENGTH)
	 ) {
	/* found it! */
	dprintf("dsm_read_wait(): found struct buffer at index %d\n", i);
	break;
      }
    }

    if(i>=nmonitor) {
      /* we didn't find it! */
      MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
      fprintf(stderr, "dsm_read_wait: alloc name not in monlist\n");
      return(DSM_INTERNAL_ERROR);
    }
    
    if(struct_buffer_list[i] == (dsm_structure *)NULL) {
      MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
      fprintf(stderr, "dsm_read_wait: NULL struct buffer detected\n");
      return(DSM_INTERNAL_ERROR);
    }
    
    if(strcmp(struct_buffer_list[i]->name, allocname)) {	
      MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
      fprintf(stderr, "dsm_read_wait: struct buffer mismatch %s - %s\n",
	      struct_buffer_list[i]->name, allocname);
      return(DSM_INTERNAL_ERROR);
    }
    
    /* copy the data into the caller's buffer, one element at a time */
    dprintf("dsm_read_wait(): copying struct data to client buffer\n");
    accum_size = 0;
    for(j=0; j<struct_buffer_list[i]->n_elements; j++) {
      memcpy(struct_buffer_list[i]->elements[j].datap,
	     data_buffers + notification_table_index * *max_buffer_size_p
	     + accum_size,
	     struct_buffer_list[i]->elements[j].size);
      accum_size += struct_buffer_list[i]->elements[j].size;
    }
    /* and tell them where the structure is */
    memcpy(datump, &struct_buffer_list[i], sizeof(struct_buffer_list[i]));
  }
  else {
    /* not a structure; just copy all bytes */
    dprintf("dsm_read_wait(): copying non-struct data to client buffer\n");
    memcpy(datump,
	   data_buffers + notification_table_index * *max_buffer_size_p,
	   notification_table[notification_table_index].size);
  }

  MUTEX_UNLOCK(&function_call_mutex, "dsm_read_wait", "");
  return(DSM_SUCCESS);
}


/*****************************************************/
/* add a hostname/allocname pair to the monitor list */
/*****************************************************/

/* if allocname refers to a structure, the caller must provide a third
** argument which is a pointer to a dsm_structure initialized for the
** structure 
*/
int dsm_monitor(char *hostname, char *allocname, ...) {
  va_list ap;
  struct dsm_local_structure_info *lsip;
  dsm_structure *dsp;
  int i, s;

  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_monitor", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
    return(DSM_NO_INIT);
  }

  /* make sure the request is valid (we use dsm_get_info(), but we could
     just as well use anything else; at least with dsm_get_info(), the
     info is cached locally so next time we run it for this alloc we
     won't have to ask the server) */
  dprintf("dsm_monitor(): checking info for %s:%s\n", hostname, allocname);
  s = dsm_get_info(hostname, allocname, &lsip);
  if(s!=DSM_SUCCESS) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
    return(s);
  }

  /* if it's a structure, make sure the provided argument is okay */
  if(lsip->is_structure == DSM_TRUE) {
    va_start(ap, allocname);
    dsp = va_arg(ap, dsm_structure *);
    va_end(ap);

    if(strcmp(allocname, dsp->name)) {
      MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
      return(DSM_STRUCT_INVALID);
    }
  }
  else {
    dsp = (dsm_structure *)NULL;
  }

  /* get the mutex for the monitor list */
  dprintf("dsm_monitor(): locking monlist_mutex\n");
  s = pthread_mutex_lock(&monlist_mutex);
  if(s!=0) {
    fprintf(stderr, "dsm_monitor(): pthread_mutex_lock() returns %d\n",s);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
    return(DSM_INTERNAL_ERROR);
  }

  /* check to see whether the values are already in the list */
  dprintf("dsm_monitor(): checking whether alloc already in list\n");
  for(i=0; i<nmonitor; i++) {
    if(strncmp(hostname,
	       hostname_list + i*DSM_NAME_LENGTH,
	       DSM_NAME_LENGTH)
       == 0
       &&
       strncmp(allocname, 
	       allocname_list + i*DSM_NAME_LENGTH,
	       DSM_NAME_LENGTH) 
       == 0) {

      dprintf("dsm_monitor(): alloc already in list\n");
      dprintf("dsm_monitor(): unlocking monlist mutex\n");
      s = pthread_mutex_unlock(&monlist_mutex);
      if(s!=0) 
	fprintf(stderr, "dsm_monitor(): pthread_mutex_lock() returns %d\n",s);
      
      MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
      return(DSM_IN_MON_LIST);
    }
  }


  /* make new space in the monitor list */
  dprintf("dsm_monitor(): alloc not in list; reallocing list\n");
  hostname_list = (char *)realloc(hostname_list, 
				  (nmonitor+1)*DSM_NAME_LENGTH);
  if(hostname_list == (char *)NULL) {
    fprintf(stderr, "dsm_monitor(): realloc: %s\n", strerror(errno));
    dprintf("dsm_monitor(): unlocking monlist mutex\n");
    s = pthread_mutex_unlock(&monlist_mutex);
    if(s!=0) 
      fprintf(stderr, "dsm_monitor(): pthread_mutex_lock() returns %d\n",s);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
    return(DSM_INTERNAL_ERROR);
  }

  allocname_list = (char *)realloc(allocname_list, 
				   (nmonitor+1)*DSM_NAME_LENGTH);
  if(allocname_list == (char *)NULL) {
    dprintf("dsm_monitor(): unlocking monlist mutex\n");
    s = pthread_mutex_unlock(&monlist_mutex);
    if(s!=0) 
      fprintf(stderr, "dsm_monitor(): pthread_mutex_lock() returns %d\n",s);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
    return(DSM_INTERNAL_ERROR);
  }

  struct_buffer_list = 
    (dsm_structure **)realloc(struct_buffer_list,
			      (nmonitor+1)*sizeof(dsm_structure *));
  if(struct_buffer_list == (dsm_structure **)NULL) {
    dprintf("dsm_monitor(): unlocking monlist mutex\n");
    s = pthread_mutex_unlock(&monlist_mutex);
    if(s!=0) 
      fprintf(stderr, "dsm_monitor(): pthread_mutex_lock() returns %d\n",s);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
    return(DSM_INTERNAL_ERROR);
  }

  /* we got the memory; put in the values and update the count */
  strncpy(hostname_list + nmonitor*DSM_NAME_LENGTH,
	  hostname,
	  DSM_NAME_LENGTH);

  strncpy(allocname_list + nmonitor*DSM_NAME_LENGTH,
	  allocname,
	  DSM_NAME_LENGTH);

  struct_buffer_list[nmonitor] = dsp;


  nmonitor++;
  dprintf("dsm_monitor(): incremented nmonitor to %d\n", nmonitor);

  dprintf("dsm_monitor(): unlocking monlist mutex\n");
  s = pthread_mutex_unlock(&monlist_mutex);
  if(s!=0)
    fprintf(stderr, "dsm_monitor(): pthread_mutex_unlock() returns %d\n",s);

  dprintf("dsm_monitor(): finished\n");
  MUTEX_UNLOCK(&function_call_mutex, "dsm_monitor", "");
  return(DSM_SUCCESS);
}


/**************************************************/
/* remove a host/alloc pair from the monitor list */
/**************************************************/
int dsm_no_monitor(char *hostname, char *allocname) {
  int index;
  int i, s;

  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_no_monitor", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_no_monitor", "");
    return(DSM_NO_INIT);
  }

  s = pthread_mutex_lock(&monlist_mutex);
  if(s!=0) {
    fprintf(stderr, "dsm_no_monitor(): pthread_mutex_lock() returns %d\n", s);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_no_monitor", "");
    return(DSM_INTERNAL_ERROR);
  }

  /* look through the list to see whether the host/alloc pair is in
     there */
  index = -1;
  for(i=0; i<nmonitor; i++) {
    if(strncmp(hostname,
	       hostname_list + i*DSM_NAME_LENGTH,
	       DSM_NAME_LENGTH)
       == 0
       &&
       strncmp(allocname,
	       allocname_list + i*DSM_NAME_LENGTH,
	       DSM_NAME_LENGTH)
       == 0) {
      index = i;
      break;
    }
  }

  /* did we find it? */
  if(index == -1) {
    /* nope */
    s = pthread_mutex_unlock(&monlist_mutex);
    if(s!=0)
      fprintf(stderr, 
	      "dsm_no_monitor(): pthread_mutex_unlock() returns %d\n", s);

    MUTEX_UNLOCK(&function_call_mutex, "dsm_no_monitor", "");
    return(DSM_NOT_IN_LIST);
  }

  /* they're in the list; get'em out */
  if(nmonitor==1) {
    free(allocname_list);
    free(hostname_list);
    free(struct_buffer_list);
    allocname_list     = (char *)NULL;
    hostname_list      = (char *)NULL;
    struct_buffer_list = (dsm_structure **)NULL;
    nmonitor = 0;
  }
  else {
    /* move all the ones above down one space */
    for(i=index; i<nmonitor-1; i++) {
      strncpy(hostname_list + i*DSM_NAME_LENGTH,
	      hostname_list + (i+1)*DSM_NAME_LENGTH,
	      DSM_NAME_LENGTH);

      strncpy(allocname_list + i*DSM_NAME_LENGTH,
	      allocname_list + (i+1)*DSM_NAME_LENGTH,
	      DSM_NAME_LENGTH);

      struct_buffer_list[i] = struct_buffer_list[i+1];
    }
    
    /* now resize the arrays to eliminate the end */
    hostname_list = (char *)realloc(hostname_list,
				    (nmonitor-1)*DSM_NAME_LENGTH);

    allocname_list = (char *)realloc(allocname_list,
				     (nmonitor-1)*DSM_NAME_LENGTH);
    struct_buffer_list = 
      (dsm_structure **)realloc(struct_buffer_list,
				(nmonitor-1)*sizeof(dsm_structure *));
    
    nmonitor--;
  }

  s = pthread_mutex_unlock(&monlist_mutex);
  if(s!=0)
    fprintf(stderr, 
	    "dsm_no_monitor(): pthread_mutex_unlock() returns %d\n", s);
  
  MUTEX_UNLOCK(&function_call_mutex, "dsm_no_monitor", "");
  return(DSM_SUCCESS);
}


/*******************************************/
/* remove everything from the monitor list */  
/*******************************************/
int dsm_clear_monitor(void) {
  int s;
  
  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_clear_monitor", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_clear_monitor", "");
    return(DSM_NO_INIT);
  }

  s = pthread_mutex_lock(&monlist_mutex);
  if(s!=0) {
    fprintf(stderr, 
	    "dsm_clear_monitor(): pthread_mutex_lock() returns %d\n",
	    s);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_clear_monitor", "");
    return(DSM_INTERNAL_ERROR);
  }

  if(nmonitor==0) {
    s = pthread_mutex_unlock(&monlist_mutex);
    if(s!=0) 
      fprintf(stderr, 
	      "dsm_clear_monitor(): pthread_mutex_unlock() returns %d\n",
 	      s); 
    MUTEX_UNLOCK(&function_call_mutex, "dsm_clear_monitor", "");
    return(DSM_MON_LIST_EMPTY);
  }

  dprintf("dsm_clear_monitor(): freeing hostname_list=0x%p\n",hostname_list);
  free(hostname_list);
  dprintf("dsm_clear_monitor(): freeing allocname_list=0x%p\n",allocname_list);
  free(allocname_list);
  dprintf("dsm_clear_monitor(): freeing struct_buffer_list=0x%p\n",
	  struct_buffer_list);
  free(struct_buffer_list);

  hostname_list      = (char *)NULL;
  allocname_list     = (char *)NULL;
  struct_buffer_list = (dsm_structure **)NULL;
  nmonitor = 0;

  s = pthread_mutex_unlock(&monlist_mutex);
  if(s!=0) 
    fprintf(stderr, 
	    "dsm_clear_monitor(): pthread_mutex_unlock() returns %d\n", s);

  MUTEX_UNLOCK(&function_call_mutex, "dsm_clear_monitor", "");
  return(DSM_SUCCESS);
}



/************************/
/* write a value to dsm */
/************************/
static int dsm_write_internal(char *targetname,
			      char *allocname,
			      void *datump,
			      int notify) {
  struct dsm_transaction req,reply;
  struct dsm_local_structure_info *lsip;
  int    i,s;

  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_write_internal", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
    return(DSM_NO_INIT);
  }

  /* get the info */
  s = dsm_get_info(targetname, allocname, &lsip);
  if(s != DSM_SUCCESS) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
    return(s);
  }

  /* fill in the request data; not all of this gets used for a write,
     but fill it in for completeness */
  req.targetname     = targetname;
  req.allocname      = allocname;
  req.data_size      = lsip->size;
  req.n_elements     = lsip->n_elements;
  req.is_structure   = lsip->is_structure;
  req.elements       =
    (struct alloc_element *)malloc(lsip->n_elements
				   *
				   sizeof(struct alloc_element));
  if(req.elements == (struct alloc_element *)NULL) { 
    perror("dsm_write_internal: malloc(req.elements)");
    MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
    return(DSM_INTERNAL_ERROR);
  }

  for(i=0; i<lsip->n_elements; i++) {
    strcpy(req.elements[i].name, lsip->elements[i].name);
    req.elements[i].size       = lsip->elements[i].size;

    /* since this is a local request, we don't need to know the details
       of what's in the buffer; we can just treat it as a single string
       of bytes */
    req.elements[i].n_sub_elements = 1;
    req.elements[i].xdr_filter_index = XDR_FILTER_OPAQUE;

    req.elements[i].datap     = malloc(req.elements[i].size);
    if(req.elements[i].datap == NULL) {
      perror("dsm_write_internal: malloc(req.elements[].datap)");
      free(req.elements);
      MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
      return(DSM_INTERNAL_ERROR);
    }
      
    if(lsip->is_structure == DSM_TRUE) {
      memcpy(req.elements[i].datap,
	     ((struct dsm_local_structure_info *)datump)->elements[i].datap,
	     req.elements[i].size);
    }
    else {
      memcpy(req.elements[i].datap, datump, req.elements[i].size);
    }      
  }
  req.timestamp = 0; /* dummy */
  req.notify    = notify;
  req.status    = DSM_SUCCESS; /* dummy */
  
  /* make the filters allocate space for the reply */
  reply.targetname  = (char *)NULL;
  reply.allocname   = (char *)NULL;
  reply.elements    = (struct alloc_element *)NULL;

  /* and make the call */
  s = dsm_client_call_with_retry(DSM_LOCAL_WRITE,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&req,
				 (xdrproc_t)xdr_dsm_transaction,
				 (char *)&reply,
				 timeout);
  for(i=0; i<req.n_elements; i++) free(req.elements[i].datap);
  free(req.elements);

  if(s!=DSM_SUCCESS) {
    /* free the memory allocated by the filter */
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
    return(DSM_RPC_ERROR);
  } 

  /* even if the rpc worked, there can be a dsm error */
  if(reply.status != DSM_SUCCESS) {
    /* free the memory allocated by the filter */
    xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
    MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
    return(reply.status);
  }
 
  /* free the memory allocated by the filter */
  xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
  
  MUTEX_UNLOCK(&function_call_mutex, "dsm_write_internal", "");
  return(DSM_SUCCESS);
}


int dsm_write(char *targetname, char *allocname, void *datump) {
  return(dsm_write_internal(targetname, allocname, datump, DSM_FALSE) );
}

int dsm_write_notify(char *targetname, char *allocname, void *datump) {
  return(dsm_write_internal(targetname, allocname, datump, DSM_TRUE) );
}


/****************************/
/* dsm structure management */
/*
** dsp *must* point to an actual structure
**
** WRONG:
** dsm_structure *d;
** dsm_structure_init(d, alloc);
**
** RIGHT:
** dsm_structure d;
** dsm_structure_init(&d, alloc);
**
*/
int dsm_structure_init(dsm_structure *dsp, char *allocname) {
  dsm_structure *ldsp;
  char hostname[DSM_NAME_LENGTH];
  int i,j,s;
  
  /*********/
  /* Begin */
  /*********/

  MUTEX_LOCK(&function_call_mutex, "dsm_structure_init", "");

  /* is it initialized? */
  if(useropen == DSM_FALSE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
    return(DSM_NO_INIT);
  }

  /* we must have the allocation list to init a structure; if dsm_open()
     was called, there should be an allocname list, but check just to be
     safe */
  if(allocnamelist == (struct dsm_allocation_list *)NULL) {
    /* we don't have the alllocation list; go get it */
    dprintf("dsm_structure_init: no allocnamelist; getting it\n");
    
    s = dsm_get_allocation_list_unlocked(&nmachines, &allocnamelist);
    if(s!=DSM_SUCCESS) {
      MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
      return(s);
    }
  }

  /* find the allocname in the allocnamelist so we can figure out which
     hosts use this; this is kind of stupid, but it's necessary because
     to ask the server for info we have to provide a hostname */
  dprintf("dsm_structure_init: finding host for %s\n", allocname);
  hostname[0] = '\0';
  for(i=0; i<nmachines; i++) {
    for(j=0; j<allocnamelist[i].n_entries; j++) {
      if(!strncmp(allocname,
		  allocnamelist[i].alloc_list[j],
		  strlen(allocname))) {
	/* make sure it's really part of a structure by looking for the
	   colon character in the allocnamelist */
	if(strchr(allocnamelist[i].alloc_list[j], ':') == NULL) {
	  /* trying to initialize something that's not a structure */
	  MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
	  return(DSM_STRUCT_INVALID);
	}
	strcpy(hostname, allocnamelist[i].host_name);
	break;
      }
    }
    if(hostname[0]!='\0') break;
  }

  /* did we find it? */
  if(hostname[0]=='\0') {
    /* no! */
    MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
    return(DSM_NAME_INVALID);
  }    

  dprintf("dsm_structure_init: found host %s; call dsm_get_info\n", hostname);

  /* got the hostname; now we can ask for info */
  s = dsm_get_info(hostname, allocname, &ldsp);
  if(s != DSM_SUCCESS) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
    return(s);
  }

  if(ldsp->is_structure != DSM_TRUE) {
    MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
    return(DSM_STRUCT_INVALID);
  }

  /* got it; initialize the caller's space */
  dprintf("dsm_structure_init: allocating & initializing\n");
  dsp->elements = malloc(ldsp->n_elements * sizeof(*dsp->elements));
  if(dsp->elements == NULL) {
    perror("dsm_structure_init: malloc(elements)");
    MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
    return(DSM_INTERNAL_ERROR);
  }

  strcpy(dsp->name,   ldsp->name);
  dsp->size         = ldsp->size;
  dsp->n_elements   = ldsp->n_elements;
  dsp->is_structure = DSM_TRUE;

  for(i=0; i<dsp->n_elements; i++) {
    strcpy(dsp->elements[i].name, ldsp->elements[i].name);
    dsp->elements[i].size = ldsp->elements[i].size;
    dsp->elements[i].datap = malloc(dsp->elements[i].size);
    if(dsp->elements[i].datap == NULL) {
      perror("dsm_structure_init: malloc()");
      for(j=0; j<i; j++) free(dsp->elements[i].datap);
      free(dsp->elements);
      MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
      return(DSM_INTERNAL_ERROR);
    }
  }      

  MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_init", "");
  return(DSM_SUCCESS);
}

void dsm_structure_destroy(dsm_structure *dsp) {
  int i;
  
  for(i=0; i<dsp->n_elements; i++) {
    free(dsp->elements[i].datap);
    dsp->elements[i].datap = NULL;
  }
  free(dsp->elements);
  dsp->elements = NULL;
}

/* action='g' for get, 's' for set */
int dsm_structure_manipulate_element(dsm_structure *dsp,
				     char *elname,
				     void *buf,
				     char action) {
  int i;

  MUTEX_LOCK(&function_call_mutex, "dsm_structure_manip_element", "");
  for(i=0; i<dsp->n_elements; i++) {
    if(!strcmp(dsp->elements[i].name, elname)) {
      if(action=='g') 
	memcpy(buf, dsp->elements[i].datap, dsp->elements[i].size);
      else if(action=='s')
	memcpy(dsp->elements[i].datap, buf, dsp->elements[i].size);
      else {
	fprintf(stderr,
		"dsm_struct_manip_element: action %c unknown\n", 
		action);
	MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_manip_element", "");
	return(DSM_INTERNAL_ERROR);
      }

      MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_manip_element", "");
      return(DSM_SUCCESS);
    }
  }

  /* if we got here, the element wasn't found */
  MUTEX_UNLOCK(&function_call_mutex, "dsm_structure_manip_element", "");
  return(DSM_NAME_INVALID);
}


int dsm_structure_get_element(dsm_structure *dsp, char *elname, void *buf) {
  return(dsm_structure_manipulate_element(dsp, elname, buf, 'g'));
}

int dsm_structure_set_element(dsm_structure *dsp, char *elname, void *buf) {
  return(dsm_structure_manipulate_element(dsp, elname, buf, 's'));
}




/******************************/
/* allocation list management */

void dsm_destroy_allocation_list_unlocked(struct dsm_allocation_list **alp) {
  xdr_free((xdrproc_t)xdr_dsm_allocation_list, (char *)alp);
  *alp = (struct dsm_allocation_list *)NULL;
  return;
}
 

void dsm_destroy_allocation_list(struct dsm_allocation_list **alp) {
  MUTEX_LOCK(&function_call_mutex, "dsm_destroy_allocation_list", "");
  dsm_destroy_allocation_list_unlocked(alp);
  MUTEX_UNLOCK(&function_call_mutex, "dsm_destroy_allocation_list", "");

  return;
}


int dsm_get_allocation_list_unlocked(int *nhosts,
				     struct dsm_allocation_list **alp) {
  extern int nmachines; /* defined above; global because the xdr filter
			   uses it (I know I know, it's a kludge) */
  int s;

  /*********/
  /* Begin */
  /*********/

  s = dsm_client_call_with_retry(DSM_LOCAL_ALLOC_LIST_REQ,
				 (xdrproc_t)xdr_void,
				 NULL,
				 (xdrproc_t)xdr_dsm_allocation_list,
				 (char *)alp,
				 timeout);
  if(s!=DSM_SUCCESS) {
    /* free any allocated memory */
    dsm_destroy_allocation_list_unlocked(alp);
    return(DSM_RPC_ERROR);
  }
  
  /* nmachines got set directly in the xdr filter rather than returned
     via the RPC; please forgive me */
  *nhosts = nmachines;

  return(DSM_SUCCESS);
}

int dsm_get_allocation_list(int *nhosts, struct dsm_allocation_list **alp) {
  int s;

  /* is it initialized? */
  if(useropen == DSM_FALSE) return(DSM_NO_INIT);

  MUTEX_LOCK(&function_call_mutex, "dsm_get_allocation_list", "");
  s = dsm_get_allocation_list_unlocked(nhosts, alp);
  MUTEX_UNLOCK(&function_call_mutex, "dsm_get_allocation_list", "");

  return(s);
}
  


/********************************/
/* print a useful error message */
void dsm_error_message(int s, char *str) {
  char message[200];

  switch(s) {

  case DSM_SUCCESS:
    strcpy(message, "successful");
    break;

  case DSM_ERROR:
    strcpy(message, "unspecified failure");
    break;

  case DSM_UNAVAILABLE:
    strcpy(message, "dsm service unavailable");
    break;

  case DSM_NO_RESOURCE:
    strcpy(message, "shared memory segment unavailable");
    break;
    
  case DSM_ALREADY_OPEN:
    strcpy(message, "dsm already opened");
    break;

  case DSM_NO_INIT:
    strcpy(message, "dsm not opened");
    break;

  case DSM_ALLOC_VERS:
    sprintf(message, "allocation version mismatch");
    break;

  case DSM_NO_LOCAL_SVC:
    sprintf(message, "no connection to local server");

  case DSM_NO_REMOTE_SVC:
    sprintf(message, "no connection to remote server");
    break;

  case DSM_RPC_ERROR:
    sprintf(message, "RPC failure");
    break;

  case DSM_LOCAL_SERVICE:
    sprintf(message, "local service only");
    break;

  case DSM_REMOTE_SERVICE:
    sprintf(message, "remote service only");
    break;

  case DSM_TARGET_INVALID:
    strcpy(message, "no shared space for target");
    break;

  case DSM_NAME_INVALID:
    strcpy(message, "allocation name doesn't exist");
    break;

  case DSM_TOO_MANY_PROC:
    strcpy(message, "too many processes waiting for notification");
    break;

  case DSM_MON_LIST_EMPTY:
    strcpy(message, "monitor list is empty");
    break;
    
  case DSM_IN_MON_LIST:
    strcpy(message, "host/alloc already in monitor list");
    break;

  case DSM_NOT_IN_LIST:
    strcpy(message, "host/alloc not in monitor list");

  case DSM_INTERRUPTED:
    strcpy(message, "interrupted by signal");
    break;
    
  case DSM_STRUCT_INVALID:
    strcpy(message, "dsm structure is not valid");
    break;

  case DSM_INTERNAL_ERROR:
    sprintf(message, "internal error -- contact maintainer");
    break;
    
  default:
    sprintf(message, "unknown error (code %d) -- contact maintainer", s);
    break;
  }
  
  fprintf(stderr,"%s: %s\n", str, message);
  return;
}

