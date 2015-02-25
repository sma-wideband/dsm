/*
** dsm_util.c
**
** Contains a bunch of utility functions which are used in various
** places in the SMA distributed shared memory (dsm) system.  These tend
** to be somewhat more internal-data-structure oriented than RPC
** oriented, although there's no clear distinction among which helper
** functions should go in this file and which go in dsm_server.c.
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_util.c,v 2.5 2013/03/01 21:01:41 ckatz Exp $
*/



#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>


#define _DSM_INTERNAL
#include "dsm.h"
#undef _DSM_INTERNAL

static char rcsid[] = "$Id: dsm_util.c,v 2.5 2013/03/01 21:01:41 ckatz Exp $";

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
/* id of SysV semaphore set */
int semid;
#endif

/***********************/
/* diagnostic printf() */
/***********************/

int verbose=DSM_FALSE;
FILE *errfile;

int dprintf(char *format, ...) {
  extern int verbose;
  va_list ap;
  int s=0;
  char formatstring[1000];
  struct timeval now;
  struct timezone dummy;

  if(verbose && errfile!=(FILE *)NULL) {
    gettimeofday(&now, &dummy);
    sprintf(formatstring, "%d.%06d - ", (int)now.tv_sec, (int)now.tv_usec);
    strcat(formatstring, format);
    va_start(ap, format);
    s = vfprintf(errfile, formatstring, ap);
    va_end(ap);
    fflush(errfile);
  }

  return(s);
}



/********************************************************/
/* comparison functions for qsort() and bsearch() used  */
/* for sorting and finding allocation list entries,     */
/* allocation list head entries, and classshare entries */
/********************************************************/
int compare_alloc_entries(const void *e1, const void *e2) {
  return(strcmp( ((struct alloc_entry *)e1)->name,
		 ((struct alloc_entry *)e2)->name) );
}

int compare_alloc_list_head_addr(const void *e1, const void *e2) {
  return(  ((struct alloc_list_head *)e1)->machine_addr
	 - ((struct alloc_list_head *)e2)->machine_addr );
}

int compare_alloc_list_head_name(const void *e1, const void *e2) {
  return( strcmp( (*((struct alloc_list_head **)e1))->machine_name,
		  (*((struct alloc_list_head **)e2))->machine_name ) 
	  );
}

int compare_class_share_entries(const void *e1, const void *e2) {
  return( strcmp( ((struct class_share *)e1)->class->class_name,
		  ((struct class_share *)e2)->class->class_name ) 
	  );
}

int compare_class_share_allocnames(const void *e1, const void *e2) {
  return( strcmp(  ((struct class_share_alloc_entry *)e1)->allocname,
		   ((struct class_share_alloc_entry *)e2)->allocname) 
	  );
}

/********************************************************/
/* return a pointer to the member of alloclist for name */
/********************************************************/
struct alloc_list_head *lookup_machine_name(char *name) {
  extern struct alloc_list_head **alloclistbyname;
  extern int nmachines;
  
  struct alloc_list_head alh, *alhp, **alhpp;

  strcpy(alh.machine_name, name);
  alhp = &alh;

  alhpp = (struct alloc_list_head **)
    bsearch(&alhp,
	    alloclistbyname,
	    nmachines,
	    sizeof(struct alloc_list_head *),
	    compare_alloc_list_head_name);

  if(alhpp == (struct alloc_list_head **)NULL)
    return( (struct alloc_list_head *)NULL );
  else
    return(*alhpp);
}

/*****************************************************/
/* return a entire class's worth of pointers to the  */
/* members of alloclist for name                     */
/*****************************************************/
struct class_share *lookup_class_name(char *name) {
  extern struct class_share *sharelist;
  extern int nclassshares;

  struct class_share cs;
  struct class_entry class;

  cs.class = &class;
  strcpy(cs.class->class_name, name);
  return( (struct class_share *)bsearch(&cs,
					sharelist,
					nclassshares,
					sizeof(struct class_share),
					compare_class_share_entries)
	  );
}

/********************************************************/
/* return a pointer to the member of alloclist for addr */
/********************************************************/
struct alloc_list_head *lookup_machine_addr(u_long address) {
  extern struct alloc_list_head *alloclist;
  extern int nmachines;
  struct alloc_list_head alh;
  
  alh.machine_addr = address;

  return( (struct alloc_list_head *)
	  bsearch(&alh,
		  alloclist,
		  nmachines,
		  sizeof(struct alloc_list_head),
		  compare_alloc_list_head_addr)
	  );
}



/******************************************************************/
/* return a pointer to the member of the allocation list for the  */
/* allocation under the alloclist member pointed to by lhp (huh?) */
/******************************************************************/
struct alloc_entry *
lookup_allocation(struct alloc_list_head *lhp, char *name) {
  struct alloc_entry ae;

  strcpy(ae.name, name);
  return( (struct alloc_entry *)
	  bsearch(&ae,
		  lhp->allocs,
		  lhp->nallocs,
		  sizeof(struct alloc_entry),
		  compare_alloc_entries)
	  );
}


/******************************************************************/
/* return a pointer to the member of the allocation list for the  */
/* class share                                                    */
/******************************************************************/
struct class_share_alloc_entry *
lookup_class_share_alloc_entry(struct class_share *csp, char *name) {
  struct class_share_alloc_entry csae;

  strcpy(csae.allocname, name);
  return( (struct class_share_alloc_entry *)
	  bsearch(&csae,
		  csp->csaep,
		  csp->nallocs,
		  sizeof(struct class_share_alloc_entry),
		  compare_class_share_allocnames)
	  );
}


/**************************************************/
/* send back one of our own internal status codes */
/**************************************************/
bool_t dsm_return_status_code(SVCXPRT *transp, int status) {
  struct dsm_transaction reply;
  bool_t retval;

  /* all we are sending is the status */
  reply.targetname   = "dummy";
  reply.allocname    = "dummy";
  reply.data_size    = 0;
  reply.n_elements   = 0;
  reply.is_structure = DSM_FALSE;
  reply.timestamp    = 0; /* dummy */ 
  reply.notify       = DSM_FALSE; /* dummy */

  /* put in the status code */
  reply.status = status;

  /* and send it back */
  retval = svc_sendreply(transp, 
			 (xdrproc_t)xdr_dsm_transaction,
			 (char *)&reply);
  
  if(!retval) {
    dprintf("dsm_return_status_code(): svc_sendreply() failed\n");
    svcerr_systemerr(transp);
  }

  return(retval);
}

/****************************************************************/
/* synchronize allocation tables on two ends of the connection; */
/* first argument must be local table, second copy remote table */
/****************************************************************/
void dsm_sync_allocations(struct alloc_list_head *alhp_local,
			  struct alloc_list_head *alhp_remote) {
  
  int i, j, s;

  /* since the time offset gets computed in decoding (because that's
     when we receive the timestamp from the other end), it gets returned
     in the remote copy; copy it to alhp */
  alhp_local->remote_clock_offset = alhp_remote->remote_clock_offset;
  
  /* consistency check */
  if(alhp_local->nallocs != alhp_remote->nallocs) {
    fprintf(stderr, "dsm_sync_allocations(): internal error: "
	    "nallocs mismatch\n");
    return;
  }

  for(i=0; i<alhp_local->nallocs; i++) {
    /* consistency check */
    if(strcmp(alhp_local->allocs[i].name, alhp_remote->allocs[i].name)) {
      fprintf(stderr,
	      "dsm_sync_alloc: host %s mismatch: el %d loc %s rem %s\n",
	      alhp_remote->machine_name, i, 
	      alhp_local->allocs[i].name,
	      alhp_remote->allocs[i].name);
    }

    /* make sure no one is monkeying around with the datum while we
       examine it */
    s = pthread_mutex_lock(&alhp_local->allocs[i].mutex);
    if(s!=0) {
      fprintf(stderr,
	      "dsm_sync_allocations(): pthread_mutex_lock() returns %d\n",
	      s);
      return;
    }

    dprintf("dsm_sync_allocations(): (%d of %d) checking %s:%s\n",
	    i+1, alhp_local->nallocs,
	    alhp_local->machine_name, 
	    alhp_local->allocs[i].name);

    /* if both sides are uninitialized, don't do anything */
    if((int)alhp_local->allocs[i].timestamp == (time_t)0
       &&
       (int)alhp_remote->allocs[i].timestamp == (time_t)0) {
      dprintf("                        neither side initialized\n");
    }
    else {
      /* size check for consistency if both are initialized */
      if((int)alhp_local->allocs[i].timestamp != (time_t)0
	 &&
	 (int)alhp_remote->allocs[i].timestamp != (time_t)0) {

	if(alhp_local->allocs[i].size != alhp_remote->allocs[i].size) {
	  fprintf(stderr,
		  "                        "
		  "size mismatch for %s:%s (l %d, r %d)\n",
		  alhp_local->machine_name,
		  alhp_local->allocs[i].name,
		  (int)(alhp_local->allocs[i].size),
		  (int)(alhp_remote->allocs[i].size));
	  continue;
	}
      }

      dprintf("                        local ts %d, adjusted remote ts %d\n",
	      (int)alhp_local->allocs[i].timestamp,
	      (int)alhp_remote->allocs[i].timestamp
	      + alhp_local->remote_clock_offset);
      
      /* if the remote version is newer, copy it in to our local storage;
	 otherwise keep the local value */
      if((int)alhp_local->allocs[i].timestamp 
	 <
	 (int)alhp_remote->allocs[i].timestamp 
	 + alhp_local->remote_clock_offset) {
	
	dprintf("                        remote is newer\n");
	alhp_local->allocs[i].timestamp = 
	  alhp_remote->allocs[i].timestamp + alhp_local->remote_clock_offset;

	for(j=0; j<alhp_local->allocs[i].n_elements; j++) {
	  memcpy(alhp_local->allocs[i].elements[j].datap,
		 alhp_remote->allocs[i].elements[j].datap,
		 alhp_local->allocs[i].elements[j].size);
	}

      }
      else {
	dprintf("                        keep local value\n");
      }

    }

    /* all done */
    s = pthread_mutex_unlock(&alhp_local->allocs[i].mutex);
    if(s!=0) {
      fprintf(stderr,
	      "dsm_sync_allocations(): pthread_mutex_unlock() returns %d\n",
	      s);
      return;
    }

  }

}

/********************************/
/* robustly connect to a server */
/********************************/
void dsm_robust_connect(struct alloc_list_head *alhp) {

  extern u_long my_address;
  extern u_long my_alloc_version;
  u_long remote_alloc_version;

  struct timeval timeout;
  enum clnt_stat rpcstat;

  struct alloc_list_head alh_remote;

  /*********/
  /* Begin */
  /*********/

  /* we do not set up the connection if we're being asked to connect to
     ourselves (i.e. a locally shared space) */
  if(alhp->machine_addr == my_address) {
    dprintf("dsm_robust_connect(%s): "
	    "locally shared space; no connection needed\n",
	    alhp->machine_name);
    return;
  }


  /* first make sure no one else is in the process of making this
     connection */
  MUTEX_LOCK(&alhp->connect_mutex,
	     "dsm_robust_connect",
	     alhp->machine_name);
  
  if(alhp->connect_in_progress == DSM_TRUE) {
    /* someone else is already working on this connection; let them
       proceed; we return */
    MUTEX_UNLOCK(&alhp->connect_mutex,
		 "dsm_robust_connect",
		 alhp->machine_name);
    return;
  }
  else {
    /* no one is trying to connect; set the flag so we can do it */
    dprintf("dsm_robust_connect(%s): setting connect_in_progress TRUE\n",
	    alhp->machine_name);
    alhp->connect_in_progress = DSM_TRUE;
    MUTEX_UNLOCK(&alhp->connect_mutex,
		 "dsm_robust_connect",
		 alhp->machine_name);
  }

  /* now get the client_call_mutex so we can lock out other processes
     which want to do client operations */
  MUTEX_LOCK(&alhp->client_call_mutex,
	     "dsm_robust_connect",
	     alhp->machine_name);
  
  /* if there's a NULL handle, we already know the connection is no
     good; otherwise we have to destroy it */
  if(alhp->cl != (CLIENT *)NULL) {
    dprintf("dsm_robust_connect(%s): destroying client handle\n",
	    alhp->machine_name);
    clnt_destroy(alhp->cl);
    alhp->cl = (CLIENT *)NULL;

    /* and clear the version number */
    MUTEX_LOCK(&alhp->version_mutex,
	       "dsm_robust_connect",
	       alhp->machine_name);
    alhp->version = DSM_VERSION_UNKNOWN;
    MUTEX_UNLOCK(&alhp->version_mutex,
	       "dsm_robust_connect",
	       alhp->machine_name);
  }

  /* okay, let's now try to make the connection */
  dprintf("dsm_robust_connect(%s): connecting\n", alhp->machine_name);
  alhp->cl = clnt_create(alhp->machine_addr_string, DSM, DSM_VERS, "tcp");
  
  if(alhp->cl == (CLIENT *)NULL) {
    dprintf("dsm_robust_connect(%s): %s\n",
	    alhp->machine_name,
	    clnt_spcreateerror(""));
  }
  else {
    dprintf("dsm_robust_connect(%s): connected successfully\n",
	    alhp->machine_name);

    /* we made the connection; fix up the timeout */
    timeout = DSM_CLIENT_CALL_TIMEOUT;
    dprintf("dsm_robust_connect(%s): calling clnt_control()\n",
	    alhp->machine_name);
    if( clnt_control(alhp->cl, CLSET_TIMEOUT, (char *)&timeout) != TRUE )
      fprintf(stderr, 
	      "dsm_robust_connect(%s): clnt_control() failed\n",
	      alhp->machine_name);
    else
      dprintf("dsm_robust_connect(%s): clnt_control() succeeded\n",
	      alhp->machine_name);


    /* the connection is now set up;  do a version check and alloc copy
       request */ 
    dprintf("dsm_robust_connect(%s): requesting version check\n",
	    alhp->machine_name);
    rpcstat = clnt_call(alhp->cl,
			DSM_ALLOC_VERSION_CHECK,
			(xdrproc_t)xdr_u_long,
			(char *)&my_alloc_version,
			(xdrproc_t)xdr_u_long,
			(char *)&remote_alloc_version,
			DSM_CLIENT_CALL_TIMEOUT);
      
    if(rpcstat != RPC_SUCCESS) {
      /* oh no, there was an error; better not use this connection */
      fprintf(stderr,
	      "dsm_robust_connect(%s): %s\n",
	      alhp->machine_name,
	      clnt_sperror(alhp->cl, "version check"));
      clnt_destroy(alhp->cl);
      alhp->cl = (CLIENT *)NULL;
	
      MUTEX_LOCK(&alhp->version_mutex,
		 "dsm_robust_connect",
		 alhp->machine_name);
      dprintf("dsm_robust_connect(%s): setting version to UNKNOWN\n",
	      alhp->machine_name);
      alhp->version = DSM_VERSION_UNKNOWN;
      MUTEX_UNLOCK(&alhp->version_mutex,
		   "dsm_robust_connect",
		   alhp->machine_name);
    }
    else if(remote_alloc_version != my_alloc_version) {
      /* call succeeded, but version doesn't match */
      dprintf("dsm_robust_connect(%s): "
	      "version mismatch local 0x%x, remote 0x%x\n",
	      alhp->machine_name,
	      my_alloc_version,
	      remote_alloc_version);
      
      MUTEX_LOCK(&alhp->version_mutex,
		 "dsm_robust_connect",
		 alhp->machine_name);
      dprintf("dsm_robust_connect(%s): setting version to MISMATCH\n",
	      alhp->machine_name);
      alhp->version = DSM_VERSION_MISMATCH;
      MUTEX_UNLOCK(&alhp->version_mutex,
		   "dsm_robust_connect",
		   alhp->machine_name);
    }
    else {
      /* call succeeded and version matches; wahoo */
      dprintf("dsm_robust_connect(%s): remote version 0x%x matches ours\n",
	      alhp->machine_name,
	      remote_alloc_version);
      
      MUTEX_LOCK(&alhp->version_mutex,
		 "dsm_robust_connect",
		 alhp->machine_name);
      dprintf("dsm_robust_connect(%s): setting version\n",
	      alhp->machine_name);
      alhp->version = remote_alloc_version;
      MUTEX_UNLOCK(&alhp->version_mutex,
		   "dsm_robust_connect",
		   alhp->machine_name);
      
      /* do allocation table synchronization; we send ours over there,
	 and get theirs in return */
      dprintf("dsm_robust_connect(%s): requesting "
	      "allocation synchronization\n",
	      alhp->machine_name);

      strcpy(alh_remote.machine_name, alhp->machine_name);
      alh_remote.nallocs = alhp->nallocs;
      alh_remote.allocs = (struct alloc_entry *)NULL;
      rpcstat = clnt_call(alhp->cl,
			  DSM_ALLOC_SYNC,
			  (xdrproc_t)xdr_dsm_alloc_entry_array,
			  (char *)alhp,
			  (xdrproc_t)xdr_dsm_alloc_entry_array,
			  (char *)&alh_remote,
			  DSM_CLIENT_CALL_TIMEOUT);
      if(rpcstat != RPC_SUCCESS) {
	/* there was an error; better not use this connection */
	fprintf(stderr,
		"dsm_robust_connect(%s): %s\n",
		alhp->machine_name,
		clnt_sperror(alhp->cl, "alloc sync req"));
	
	xdr_free((xdrproc_t)xdr_dsm_alloc_entry_array, (char *)&alh_remote);
	clnt_destroy(alhp->cl);
	alhp->cl = (CLIENT *)NULL;
      }	  
      else {
	dprintf("dsm_robust_connect(%s): received alloc copy\n",
		alhp->machine_name);

	/* since the time offset can only get computed in decoding
	   (because that's when we receive the timestamp from the other
	   end), it gets returned in alh_remote; copy it to alhp */
	alhp->remote_clock_offset = alh_remote.remote_clock_offset;
	
	dsm_sync_allocations(alhp, &alh_remote);

	xdr_free((xdrproc_t)xdr_dsm_alloc_entry_array, (char *)&alh_remote);
      }
      
    } /* if version number matches */

  } /* if the new client handle was created successfully */

  
  
  /* okay, we're done setting up this connection; clear all our
     lockouts */
    
  /* clear the connect_in_progress flag */
  MUTEX_LOCK(&alhp->connect_mutex,
	     "dsm_robust_connect",
	     alhp->machine_name);
  dprintf("dsm_robust_connect(%s): clearing connect_in_progress\n",
	  alhp->machine_name);
  alhp->connect_in_progress = DSM_FALSE;
  MUTEX_UNLOCK(&alhp->connect_mutex,
	       "dsm_robust_connect",
	       alhp->machine_name);
  
  /* and finally, we can release the client_call mutex to let client
     calls proceed */
  MUTEX_UNLOCK(&alhp->client_call_mutex,
	       "dsm_robust_connect",
	       alhp->machine_name);

  return;
}



/*******************************************/
/* robustly make a call to a remote server */
/*******************************************/
int dsm_robust_client_call(struct alloc_list_head *alhp,
			   u_long procnum,
			   xdrproc_t inproc,
			   char *in,
			   xdrproc_t outproc,
			   char *out) {
  enum clnt_stat rpcstat;
  
  /*********/
  /* Begin */
  /*********/

  /* first we check to see whether anyone is in the process of trying to
     connect to this server */
  MUTEX_LOCK(&alhp->connect_mutex,
	     "dsm_robust_client_call",
	     alhp->machine_name);
  dprintf("dsm_robust_client_call(%s): checking connect_in_progress\n",
	  alhp->machine_name);
  if( alhp->connect_in_progress == DSM_TRUE ) {
    /* yep, someone is connecting */
    dprintf("dsm_robust_client_call(%s): "
	    "connection already in progress\n",
	    alhp->machine_name);
    MUTEX_UNLOCK(&alhp->connect_mutex,
		 "dsm_robust_client_call",
		 alhp->machine_name);
    return(DSM_CONNECT_IN_PROGRESS);
  }

  /* it's available */
  MUTEX_UNLOCK(&alhp->connect_mutex,
	       "dsm_robust_client_call",
	       alhp->machine_name);

  /* now get client_call_mutex so I'm the only one doing it */
  MUTEX_LOCK(&alhp->client_call_mutex,
	     "dsm_robust_client_call",
	     alhp->machine_name);

  /* first make sure there's really a connection */
  if( alhp->cl == (CLIENT *)NULL ) {
    /* no connection! launch a reconnection effort */
    dprintf("dsm_robust_client_call(%s): no connection; "
	    "launching retry\n",
	    alhp->machine_name);

    dsm_launch_connection_thread(alhp);

    MUTEX_UNLOCK(&alhp->client_call_mutex,
		 "dsm_robust_client_call",
		 alhp->machine_name);
    return(DSM_NO_CONNECTION);
  }


  /* if we got here, there's a good connection and we have all the
     lockouts we need; make the client call */

  dprintf("dsm_robust_client_call(%s): making client call\n",
	  alhp->machine_name);
  rpcstat = clnt_call(alhp->cl,
		      procnum,
		      inproc, in,
		      outproc, out,
		      DSM_CLIENT_CALL_TIMEOUT);

  if(rpcstat != RPC_SUCCESS) {
    /* the call failed; try to reestablish the connection */

    dprintf("dsm_robust_client_call(%s): %s\n",
	    alhp->machine_name,
	    clnt_sperror(alhp->cl, "clnt_call()"));
			 
    dsm_launch_connection_thread(alhp);
    
    MUTEX_UNLOCK(&alhp->client_call_mutex,
		 "dsm_robust_client_call",
		 alhp->machine_name);
    return(DSM_ERROR);
  }

  /* if we got here, the client call succeeded */
  dprintf("dsm_robust_client_call(%s): clnt_call() succeeded\n",
	  alhp->machine_name);
  MUTEX_UNLOCK(&alhp->client_call_mutex,
	       "dsm_robust_client_call",
	       alhp->machine_name);
  return(DSM_SUCCESS);
}




/**********************************************/
/* start dsm_robust_connect() in a new thread */
/**********************************************/
void dsm_launch_connection_thread(struct alloc_list_head *alhp) {

  pthread_t thread;
  int s;

  /*********/
  /* Begin */
  /*********/
  dprintf("dsm_launch_connection_thread(%s): running pthread_create()\n",
	  alhp->machine_name);
  s = pthread_create(&thread,
		     NULL,
		     (void *)dsm_robust_connect,
		     (void *)alhp);
  if(s!=0)
    fprintf(stderr, 
	    "dsm_launch_connection_thread(%s): pthread_create() returned %d\n",
	    alhp->machine_name,
	    s);
  else {
    s = pthread_detach(thread);
    if(s!=0)
      fprintf(stderr, 
	      "dsm_launch_connection_thread(%s): "
	      "pthread_detach() returned %d\n",
	      alhp->machine_name,
	      s);
  }
}


/****************************************/
/* set up the shared memory space which */
/* holds the notification table         */
/****************************************/
int dsm_initialize_notification_table(size_t max_alloc_size) {
  extern char   *shared_memory;
  extern int    *dsm_nproc_notify_max_p;
  extern size_t *max_buffer_size_p;

  extern struct notification_table_entry *notification_table;
  extern char *data_buffers;
  
  extern pthread_mutex_t notification_table_mutex;

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  union semun semarg;
  struct sembuf sbuf;
#endif

  int shm_descr;
  int shm_size;
  int created;
  int error;

  int i,j,s;

  /*********/
  /* Begin */
  /*********/

  shm_descr = shm_open(DSM_NOTIFY_TABLE_SHM_NAME, O_RDWR
#ifdef __linux
		       ,0
#endif
		       );

  if(shm_descr == -1) {
    if(errno!=ENOENT) {
      perror("init_notf_tab: shm_open");
      return(DSM_ERROR);
    }
    else {
      /* we need to create it */
      umask(0111);
      shm_descr = shm_open(DSM_NOTIFY_TABLE_SHM_NAME,
			   O_RDWR | O_CREAT,
			   0666);
      if(shm_descr==-1) {
	perror("init_notf_tab (creating): shm_open");
	return(DSM_ERROR);
      }
      
      created = DSM_TRUE;
      dprintf("dsm_init_notf_tab(): created shm\n");
    }
  }
  else {
    created = DSM_FALSE;
    dprintf("dsm_init_notf_tab(): opened shm\n");
  }


  if(created == DSM_TRUE) {
    /* set the shm segment size */
    shm_size =
      sizeof(*dsm_nproc_notify_max_p)
      + sizeof(*max_buffer_size_p)
      + DSM_NPROC_NOTIFY_MAX * sizeof(struct notification_table_entry)
      + DSM_NPROC_NOTIFY_MAX * max_alloc_size;
    
    dprintf("dsm_init_notf_tab(): setting shm size to %d\n", shm_size);
    
    s = ftruncate(shm_descr, (off_t)shm_size);
    if(s!=0) {
      perror("dsm_init_notf_tab: ftruncate");
      if(created==DSM_TRUE) {
	s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
	if(s!=0) perror("dsm_init_notf_tab: shm_unlink");
      }
      return(DSM_ERROR);
    }
  }
  else {
    /* we have to figure out what the size of the already existing shm
       segment is; just map in the first two values, then remap */
    
    dprintf("dsm_init_notf_tab(): mapping shm to read size\n");
    shared_memory = mmap(0,
			 sizeof(*dsm_nproc_notify_max_p)
			 + sizeof(*max_buffer_size_p),
			 PROT_READ | PROT_WRITE,
			 MAP_SHARED,
			 shm_descr,
			 (off_t)0);
    if(shared_memory == MAP_FAILED) {
      perror("dsm_init_notf_tab: mmap1");
      return(DSM_ERROR);
    }
    
    /* set up the pointers into the memory */
    dsm_nproc_notify_max_p = (int *)shared_memory;
    
    max_buffer_size_p = (size_t *)(shared_memory
				   + sizeof(*dsm_nproc_notify_max_p));
    
    /* now we can read the sizes */
    shm_size =
      sizeof(*dsm_nproc_notify_max_p)
      + sizeof(*max_buffer_size_p)
      + *dsm_nproc_notify_max_p * sizeof(struct notification_table_entry)
      + *dsm_nproc_notify_max_p * *max_buffer_size_p;
    
    dprintf("dsm_init_notf_tab(): existing *dsm_nproc_notify_max_p=%d\n",
	    *dsm_nproc_notify_max_p);
    dprintf("dsm_init_notf_tab(): existing *max_buffer_size_p=%d\n",
	    *max_buffer_size_p);
    dprintf("dsm_init_notf_tab(): thus existing shm size is %d\n",
	    shm_size);

    /* unmap it; then it will get mapped in with the correct size */
    s = munmap(shared_memory,
	       sizeof(*dsm_nproc_notify_max_p) + sizeof(*max_buffer_size_p));
    if(s!=0) perror("dsm_init_notf_tab: munmap");
  }


  /* now we have the correct size, whether or not we created it or it
     already existed; now map it in */
  dprintf("dsm_init_notf_tab(): mapping shm with size %d\n", shm_size);
  shared_memory = mmap(0, shm_size, PROT_READ | PROT_WRITE,
		       MAP_SHARED, shm_descr, (off_t)0);
  if(shared_memory == MAP_FAILED) {
    perror("dsm_init_notf_tab: mmap");
    if(created==DSM_TRUE) {
      s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
      if(s!=0) perror("dsm_init_notf_tab: shm_unlink");
    }
    return(DSM_ERROR);
  }

  /* set up the pointers into the memory */
  dsm_nproc_notify_max_p = (int *)shared_memory;

  max_buffer_size_p = (size_t *)(shared_memory
				 + sizeof(*dsm_nproc_notify_max_p));

  notification_table =
    (struct notification_table_entry *)((void *)max_buffer_size_p
					+ sizeof(*max_buffer_size_p));
  
  data_buffers = (char *)notification_table
    + DSM_NPROC_NOTIFY_MAX * sizeof(*notification_table);

  /* at this point we should have the shared memory mapped in; now, if
     we created it, we must initialize it; if it was already there, we
     have to check to see whether any processes are waiting for
     notification */
  if(created == DSM_TRUE) {
    /* we created it */
    dprintf("dsm_init_notf_tab(): initializing notification table\n");

    /* set the constants */
    *dsm_nproc_notify_max_p = DSM_NPROC_NOTIFY_MAX;
    *max_buffer_size_p      = max_alloc_size;

    dprintf("dsm_init_notf_tab(): set *dsm_nproc_notify_max_p = %d\n",
	    *dsm_nproc_notify_max_p);
    dprintf("dsm_init_notf_tab(): set *max_buffer_size_p = %d\n",
	    *max_buffer_size_p);

    /* make the array of notification table entries */
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
    semid = semget(DSM_SYSV_SEM_KEY,
		   DSM_NPROC_NOTIFY_MAX,
		   IPC_CREAT | S_IRWXU | S_IRWXG | S_IRWXO);
    if(semid==-1) {
      fprintf(stderr, "dsm_init_notf_tab: error getting semaphore set\n");
      perror("semget");
      
      s = munmap(shared_memory, shm_size);
      if(s!=0) perror("dsm_init_notf_tab: munmap");
      
      s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
      if(s!=0) perror("dsm_init_notf_tab: shm_unlink");
      
      return(DSM_ERROR);
    }
#endif

    for(i=0; i<DSM_NPROC_NOTIFY_MAX; i++) {
      notification_table[i].hostname[0]  = '\0';
      notification_table[i].allocname[0] = '\0';
      notification_table[i].size         = 0;

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
      /* put the id into the shared memory struct so the clients can
	 find it */
      notification_table[i].semid = semid;
      /* initialize semaphore value to 0 (indicating no one waiting) */
      semarg.val = 0;
      s = semctl(semid, i, SETVAL, semarg);
      if(s==-1) {
	fprintf(stderr, "dsm_init_notf_tab: error initializing entry %d\n", i);
	perror("semctl");

	s = munmap(shared_memory, shm_size);
	if(s!=0) perror("dsm_init_notf_tab: munmap");

	s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
	if(s!=0) perror("dsm_init_notf_tab: shm_unlink");

	return(DSM_ERROR);
      }
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
      /* initialize the semaphore value to zero, indicating that no one
	 is waiting */
      s = sem_init(&notification_table[i].sem, 1, 0);
      if(s!=0) {
	fprintf(stderr, "dsm_init_notf_tab: error initializing entry %d\n", i);
	perror("sem_init");

	for(j=0; j<i; j++) {
	  s = sem_destroy(&notification_table[i].sem);
	  if(s!=0) {
	    fprintf(stderr,"On index %d ", j);
	    perror("dsm_init_notf_tab: sem_destroy");
	  }
	}

	s = munmap(shared_memory, shm_size);
	if(s!=0) perror("dsm_init_notf_tab: munmap");

	s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
	if(s!=0) perror("dsm_init_notf_tab: shm_unlink");

	return(DSM_ERROR);
      }
#endif
      dprintf("dsm_init_notf_tab: initialized sem %d to 0\n", i);

    } /* loop over notification table entries */
  } /* if we created the shm */
  else {

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
    semid = semget(DSM_SYSV_SEM_KEY,
		   DSM_NPROC_NOTIFY_MAX,
		   S_IRWXU | S_IRWXG | S_IRWXO);
    if(semid==-1) {
      fprintf(stderr, 
	      "dsm_init_notf_tab: error getting existing semaphore set\n");
      perror("semget");
      
      s = munmap(shared_memory, shm_size);
      if(s!=0) perror("dsm_init_notf_tab: munmap");
      
      s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
      if(s!=0) perror("dsm_init_notf_tab: shm_unlink");
      
      return(DSM_ERROR);
    }
#endif

    /* the shm was already there; check consistency */
    dprintf("dsm_init_notf_tab(): checking consistency\n");
    if(*dsm_nproc_notify_max_p != DSM_NPROC_NOTIFY_MAX
       ||
       *max_buffer_size_p != max_alloc_size) {
      error = DSM_INTERNAL_CHANGE;
      dprintf("dsm_init_notf_tab(): internal change detected\n");
    }
    else {
      dprintf("dsm_init_notf_tab(): consistent; "
	      "sending resubmit requests if needed\n");
      error = DSM_RESUBMIT_REQ;
    }

    /* loop over sems, posting those that are waiting and sending back
       the error message */
    for(i=0; i<DSM_NPROC_NOTIFY_MAX; i++) {

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
      /* semarg is ignored here */
      j = semctl(semid, i, GETNCNT, &semarg);
      if(j==-1) {
	fprintf(stderr, "index %d ", i);
	perror("dsm_init_notf_tab(): semctl(GETNCNT)");
	continue;
      }
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
      s = sem_getvalue(&notification_table[i].sem, &j);
      if(s!=0) {
	fprintf(stderr, "index %d ", i);
	perror("dsm_init_notf_tab(): sem_getvalue");
	continue;
      }
#endif

      /* if j==0, no processes waiting */
      dprintf("dsm_init_notf_tab(): sem %d value is %d; %s\n",
	      i,
	      j,
	      j!=0 ? "posting" : "no post needed");

      if(j!=0) {
	/* there's a process waiting; fill in the error information */
	
	/* we indicate an error by putting 0 in the hostname, and we put
	   the error code in allocname */
	notification_table[i].hostname[0] = '\0';
	*((int *)notification_table[i].allocname) = error;

	/* release everyone who's waiting on the semapore */
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
	sbuf.sem_num = i;
	sbuf.sem_op  = j;
	sbuf.sem_flg = 0;
	s = semop(semid, &sbuf, 1);
	if(s!=0) {
	  fprintf(stderr, "index %d ", i);
	  perror("dsm_init_notf_tab(): semop(post)");
	}
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
	{
	  int k;
	  j = -j;
	  for(k=0; k<j; k++) {
	    s = sem_post(&notification_table[i].sem);
	    if(s!=0) {
	      fprintf(stderr, "dsm_init_notf_tab(): index %d iter %d", i, k);
	      perror("dsm_init_notf_tab(): sem_post");
	    }
	  }
	}
#endif
	dprintf("dsm_init_notf_tab(): incr sem %d by %d\n", i, j);
      }
    } /* loop over notification table entries */

    /* if there was a real inconsistency, we need to kill and regenerate
       the shm */ 
    if(error==DSM_INTERNAL_CHANGE) {
      s = munmap(shared_memory, shm_size);
      if(s!=0) perror("dsm_init_notf_tab: munmap");

      s = close(shm_descr);
      if(s!=0) perror("dsm_init_notf_tab(): close()");

      s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
      if(s!=0) perror("dsm_init_notf_tab: shm_unlink");
      
      /* now call ourselves to start the process again */
      return( dsm_initialize_notification_table(max_alloc_size) );
    }

  } /* the shm was already there */

  /* finally, intialize the mutex which protects the notification table */
  dprintf("dsm_init_notf_tab(): intializing notf table mutex\n");
  bzero((char *)&notification_table_mutex, sizeof(pthread_mutex_t));
  s = pthread_mutex_init(&notification_table_mutex, NULL);
  if(s!=0) {
    fprintf(stderr, "dsm_init_notf_tab(): pthread_mutex_init returns %d\n", s);
    return(DSM_ERROR);
  }
  
  dprintf("dsm_init_notf_tab(): finished\n");
  return(DSM_SUCCESS);
}
    


/************************************************/
/* see whether anyone is waiting to be notified */
/* of a new value, and if so, notify them       */
/************************************************/
void dsm_notify_waiting_procs(char *hostname,
			      struct alloc_entry *aep) {

  extern size_t *max_buffer_size_p;
  extern struct notification_table_entry *notification_table;
  extern pthread_mutex_t notification_table_mutex;
  extern pthread_mutex_t notification_ring_mutex;
  extern char *data_buffers;

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  struct sembuf sbuf;
  union semun dummy_semarg;
#endif

  struct notification_table_reference *ntrp, *ntrptemp;
  int j,k,s;

  /*********/
  /* Begin */
  /*********/
  
  /* is anyone waiting to be notified about this alloc? */

  /* lock the mutex so no one bothers us */
  dprintf("dsm_notify_waiting_procs(): locking alloc mutex\n");
  s = pthread_mutex_lock(&aep->mutex);
  if(s!=0) {
    fprintf(stderr,
	    "dsm_notify_waiting_procs(): pthread_mutex_lock returns %d\n",
	    s);
    return;
  }

  if(aep->notification_list == (struct notification_table_reference *)NULL) {
    s = pthread_mutex_unlock(&aep->mutex);
    if(s!=0) {
      fprintf(stderr,
	      "dsm_notify_waiting_procs(): pthread_mutex_unlock returns %d\n",
	      s);
      return;
    }
    dprintf("dsm_notify_waiting_procs(): no notification ring\n");
    return;
  }

  /* loop through the list, signalling each of the processes and
     removing its reference from the list */
  dprintf("dsm_notify_waiting_procs(): locking notification_table_mutex\n");
  s = pthread_mutex_lock(&notification_table_mutex);
  if(s!=0) {
    fprintf(stderr,
	    "dsm_notify_waiting_procs(): pthread_mutex_lock(ntm) returns %d\n",
	    s);
    MUTEX_UNLOCK(&aep->mutex, "dsm_notify_waiting_procs", "");
    return;
  }

  dprintf("dsm_notify_waiting_procs(): locking notification_ring_mutex\n");
  s = pthread_mutex_lock(&notification_ring_mutex);
  if(s!=0) {
    fprintf(stderr,
	    "dsm_notify_waiting_procs(): pthread_mutex_lock(ntr) returns %d\n",
	    s);
    MUTEX_UNLOCK(&notification_table_mutex, "dsm_notify_waiting_procs", "");
    MUTEX_UNLOCK(&aep->mutex,               "dsm_notify_waiting_procs", "");
    return;
  }

  /* go down the notification list, notifying each pid/tid */
  ntrp = aep->notification_list;

  while(ntrp != (struct notification_table_reference *)NULL) {
    /* we only notify if there's still a process/thread waiting on the
       semaphore, and it's the process which originally made the request */

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
    j = semctl(semid, ntrp->index, GETNCNT, &dummy_semarg);
    if(j==-1) {
      fprintf(stderr, "index %d ", ntrp->index);
      perror("dsm_notify_waiting_procs(): semctl(GETNCNT)");
      ntrp = ntrp->next;
      continue;
    }
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
    s = sem_getvalue(&notification_table[ntrp->index].sem, &j);
    if(s!=0) {
      fprintf(stderr, "index %d ", ntrp->index);
      perror("dsm_notify_waiting_procs(): sem_getvalue");
      ntrp = ntrp->next;
      continue;
    }
#endif
    if(j==0
       &&
       notification_table[ntrp->index].pid == ntrp->pid
       &&
       notification_table[ntrp->index].tid == ntrp->tid
       ) {
      dprintf("dsm_notify_waiting_procs(): no proc/thread "
	      "waiting for this alloc\n");

      /* check the next node in the notification list */
      ntrp = ntrp->next;
    }
    else {
      dprintf("dsm_notify_waiting_procs(): notifying pid %d, tid %d\n",
	      notification_table[ntrp->index].pid,
	      notification_table[ntrp->index].tid);
      
      /* copy the data into the buffers for this process */
      strncpy(notification_table[ntrp->index].hostname,
	      hostname,
	      DSM_NAME_LENGTH);
      strncpy(notification_table[ntrp->index].allocname,
	      aep->name,
	      DSM_NAME_LENGTH);
      
      notification_table[ntrp->index].size = aep->size;
      
      k=0;
      for(j=0; j<aep->n_elements; j++) {
	memcpy(data_buffers +  ntrp->index * *max_buffer_size_p + k,
	       aep->elements[j].datap,
	       aep->elements[j].size);
	k += aep->elements[j].size;
      }

      /* all the data are in place; post the semaphore until its value is
	 0 (indicating no one waiting) */
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
      j = semctl(semid, ntrp->index, GETNCNT, &dummy_semarg);
      if(j==-1) {
	fprintf(stderr, "index %d ", ntrp->index);
	perror("dsm_notify_waiting_procs(): semctl(GETNCNT)");
	ntrp = ntrp->next;
	continue;
      }
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
      s = sem_getvalue(&notification_table[ntrp->index].sem, &j);
      if(s!=0) {
	fprintf(stderr, "index %d ", ntrp->index);
	perror("dsm_notify_waiting_procs(): sem_getvalue");
	ntrp = ntrp->next;
	continue;
      }
#endif
      
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
      sbuf.sem_num = ntrp->index;
      sbuf.sem_op  = j;
      sbuf.sem_flg = 0;
      s = semop(semid, &sbuf, 1);
      if(s!=0) {
	fprintf(stderr, "index %d ", ntrp->index);
	perror("dsm_notify_waiting_procs(): semop(post)");
      }
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
      { 
	int i;
	j = -j;
	for(i=0; i<j; i++) {
	  s = sem_post(&notification_table[ntrp->index].sem);
	  if(s!= 0) {
	    fprintf(stderr,
		    "dsm_notify_waiting_procs(): index %d, iteration %d ",
		    ntrp->index, j);
	    perror("dsm_notify_waiting_procs(): sem_post");
	  }
	}
      }
#endif
      dprintf("dsm_notify_waiting_procs(): incr sem %d by %d\n",
	      ntrp->index, j);
      
      /* now destroy the notification ring that this
	 notification_table_reference node belongs to */
      dprintf("dsm_notify_waiting_procs(): "
	      "destroy notf ring for pid:tid %d:%d (index %d)\n",
	      ntrp->pid, ntrp->tid, ntrp->index);
      
      ntrptemp = ntrp;
      ntrp = ntrp->next;
      
      s = destroy_notification_ring(ntrptemp);
      if(s!=0) {
	fprintf(stderr,
		"dsm_notify_waiting_procs(): "
		"destroy_notification_ring() failed\n");
      }
    } /* if there was a pid/tid to notify */
  } /* while loop over notification list for alloc */

  /* done; clean up and exit */
  MUTEX_UNLOCK(&notification_ring_mutex,  "dsm_notify_waiting_procs", "");
  MUTEX_UNLOCK(&notification_table_mutex, "dsm_notify_waiting_procs", "");
  MUTEX_UNLOCK(&aep->mutex, "dsm_notify_waiting_procs", "");

  return;
}


/************************************************************/
/* remove an entire notification ring from the allocations' */
/* notification_list_reference structures                   */
/************************************************************/

/* YOU MUST HOLD THE NOTIFICATION_RING_MUTEX before calling this
   function */


int destroy_notification_ring(struct notification_table_reference *ntrp) {

  struct notification_table_reference *ntrptemp;
  
  /*********/
  /* Begin */
  /*********/

  /* just make sure */
  if(ntrp == (struct notification_table_reference *)NULL) {
    fprintf(stderr, "destroy_notf_ring(): called with NULL pointer arg\n");
    return(DSM_ERROR);
  }


  /* go through the ring, freeing each node */
 
  while(ntrp != (struct notification_table_reference *)NULL) {

    /* is this the only notification_table reference in the ring? */
    if(ntrp->left==ntrp  &&  ntrp->right==ntrp) {
      dprintf("destroy_notf_ring(): removing final ring member %s\n",
	      ntrp->aep->name);
      
      /* setting these to NULL will make ntrp get set to NULL when it
	 advances to the next node */
      ntrp->left  = (struct notification_table_reference *)NULL;
      ntrp->right = (struct notification_table_reference *)NULL;
    }
    else {
      /* consistency check */
      if(ntrp->left==ntrp  ||  ntrp->right==ntrp) {
	fprintf(stderr, "destroy_notf_ring(): internal inconsistency\n");
	return(DSM_ERROR);
      }

      /* adjust the pointers around this one, shrinking the ring by one */
      dprintf("destroy_notf_ring(): removing ring member: pid %d: %s\n",
	      ntrp->pid, ntrp->aep->name);
      ntrp->left->right = ntrp->right;
      ntrp->right->left = ntrp->left;
    }


    /* fix up the next/prev pointers */

    /* is this one at the top of the alloc's notification list? */
    if(ntrp->prev == (struct notification_table_reference *)NULL) {
      /* this is the top of the list */
      ntrp->aep->notification_list = ntrp->next;

      /* if there's one after this one, fix its prev pointer */
      if(ntrp->next != (struct notification_table_reference *)NULL)
	ntrp->next->prev = (struct notification_table_reference *)NULL;
    }
    else {
      /* not at the top of the list */
      ntrp->prev->next = ntrp->next;

      /* if there's one after this one, fix its prev pointer */
      if(ntrp->next != (struct notification_table_reference *)NULL)
	ntrp->next->prev = ntrp->prev;
    }

    
    /* now nothing connects to this node; destroy it and go on to the
       next one in the ring */

    ntrptemp = ntrp;
    ntrp = ntrp->left;

    free(ntrptemp);

  }  /* while loop going around ring */

  return(DSM_SUCCESS);
}


/*************************************************/
/* given a read_wait request structure, create a */
/* notification ring to fulfill the request      */
/*************************************************/

/* YOU MUST HOLD THE NOTIFICATION_RING_MUTEX before calling this
   function */


int create_notification_ring(struct dsm_read_wait_req *rwq,
			     struct alloc_entry **aepp,
			     int notification_table_index) {
  
  int i, s;
  struct notification_table_reference *ntrp, *priorntrp, *firstntrp;


  /*********/
  /* Begin */
  /*********/

  firstntrp = (struct notification_table_reference *)NULL;
  priorntrp = (struct notification_table_reference *)NULL;
  
  /* loop over the allocations which have been requested for waiting */
  for(i=0; i<rwq->num_alloc; i++) {

    /* protect the allocation */
    s = pthread_mutex_lock(&aepp[i]->mutex);
    if(s!=0) {
      fprintf(stderr,
	      "create_notf_ring(): pthread_mutex_lock(alloc) returned %d\n",
	      s);
      return(DSM_ERROR);
    }

    
    /* make a new notification_table_reference node */
    dprintf("create_notf_ring(): malloc() space for new node\n");
    ntrp = (struct notification_table_reference *)malloc(sizeof(*ntrp));
    if(ntrp == (struct notification_table_reference *)NULL) {
      perror("create_notf_ring(): malloc()");
      return(DSM_ERROR);
    }
    
    /* remember where we started */
    if(firstntrp == (struct notification_table_reference *)NULL) 
      firstntrp = ntrp;
    
    
    dprintf("create_notf_ring(): filling in ntrp info for alloc %d\n",i+1);
    ntrp->index = notification_table_index;
    ntrp->pid   = (pid_t)(rwq->pid);
    ntrp->tid   = (int)  (rwq->tid);
    ntrp->aep   = aepp[i];
    
    /* add it to the top of the alloc's notification list */
    ntrp->next = aepp[i]->notification_list;
    ntrp->prev = (struct notification_table_reference *)NULL;
    
    aepp[i]->notification_list = ntrp;

    /* if there was already one in the list, adjust its prev node */
    if(ntrp->next != (struct notification_table_reference *)NULL)
      ntrp->next->prev = ntrp;
    
    /* now connect it into the notification ring */
    if(priorntrp != (struct notification_table_reference *)NULL) {
      priorntrp->left = ntrp;
      ntrp->right     = priorntrp;
    }
    
    priorntrp = ntrp;

    dprintf("create_notf_ring(): new node attached to notification ring\n");

    MUTEX_UNLOCK(&aepp[i]->mutex, "create_notf_ring", "");

  } /* for loop over allocs to monitor */


  /* they're all hooked in; now close the ring */
  priorntrp->left  = firstntrp;
  firstntrp->right = priorntrp;

  dprintf("create_notf_ring(): notification ring created\n");

  return(DSM_SUCCESS);
}

