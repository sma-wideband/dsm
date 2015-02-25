/*
** dsm_server.c
**
** Contains all of the functions which provide RPC services in the SMA
** distributed shared memory (dsm) system.  These are the functions that
** are called by dsm_dispatcher().  There are also some helper functions
** in here that the service functions call.
**
** It isn't well defined how these helper functions are divided up
** between this file and dsm_util.c.
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_server.c,v 2.6 2009/06/15 16:58:07 ckatz Exp $
*/

#include <rpc/rpc.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <string.h>

#define _DSM_INTERNAL
#include "dsm.h"
#undef  _DSM_INTERNAL

static char rcsid[] = "$Id: dsm_server.c,v 2.6 2009/06/15 16:58:07 ckatz Exp $";

/* everyone looks at the allocation list */
extern struct alloc_list_head *alloclist;
extern int nmachines;


/*********************************************/
/* handles allocation version check requests */
/*********************************************/
void allocation_version_check(SVCXPRT *transp, 
			      struct alloc_list_head *alhp) {
  extern u_long my_alloc_version;
  u_long remote_alloc_version;

  /*********/
  /* Begin */
  /*********/

  dprintf("allocation_version_check(): running\n");

  /* decode the allocation version that the other end has sent */
  if(!svc_getargs(transp, 
		  (xdrproc_t)xdr_u_long, 
		  (char *)&remote_alloc_version)) {
    dprintf("allocation_version_check(): svc_getargs() failed\n");
    svcerr_decode(transp);
    return;
  }

  /* reply to the other end with our allocation version */
  dprintf("allocation_version_check(%s): sending our version to caller\n",
	  alhp->machine_name);
  if(!svc_sendreply(transp, (xdrproc_t)xdr_u_long, (char *)&my_alloc_version))
    svcerr_systemerr(transp);


  /* now do the version checking */

  /* check the version numbers and set the status for this machine */
  MUTEX_LOCK(&alhp->version_mutex,
	     "allocation_version_check",
	     alhp->machine_name);


  if(remote_alloc_version == my_alloc_version) {
    alhp->version = remote_alloc_version;
    dprintf("Alloc vers check from host %s: 0x%x matches our version\n",
	    alhp->machine_name, remote_alloc_version);
  }
  else {
    alhp->version = DSM_VERSION_MISMATCH;
    dprintf("Alloc vers check from host %s: "
	    "0x%x no match to our version 0x%x\n",
	    alhp->machine_name,
	    remote_alloc_version,
	    my_alloc_version);
  }
  
  MUTEX_UNLOCK(&alhp->version_mutex,
	       "allocation_version_check",
	       alhp->machine_name);

  return;
}

/***************************************************************/
/* synchronize our allocation table with that of the other end */
/***************************************************************/
void allocation_sync_reply(SVCXPRT *transp, 
			   struct alloc_list_head *alhp) {

  struct alloc_list_head alhp_remote;

  dprintf("allocation_sync_reply(): received request from host %s\n",
	  alhp->machine_name);

  strcpy(alhp_remote.machine_name, alhp->machine_name);
  alhp_remote.nallocs = alhp->nallocs;
  alhp_remote.allocs = (struct alloc_entry *)NULL;

  /* decode the allocation version that the other end has sent */
  if(!svc_getargs(transp, 
		  (xdrproc_t)xdr_dsm_alloc_entry_array,
		  (char *)&alhp_remote)) {
    dprintf("allocation_sync_reply(): svc_getargs() failed\n");
    svcerr_decode(transp);
    xdr_free((xdrproc_t)xdr_dsm_alloc_entry_array, (char *)&alhp_remote);
    return;
  }


  /* send our copy back */
  if(!svc_sendreply(transp,
		    (xdrproc_t)xdr_dsm_alloc_entry_array,
		    (char *)alhp)) {
    svcerr_systemerr(transp);
    fprintf(stderr, "allocation_sync_reply(): error replying\n");
  }

  /* synchronize */
  dsm_sync_allocations(alhp, &alhp_remote);

  xdr_free((xdrproc_t)xdr_dsm_alloc_entry_array, (char *)&alhp_remote);

  return;
}


/*********************************************/
/* a local process wants the allocation list */
/*********************************************/
void local_alloc_list_request(SVCXPRT *transp) {

  /*********/
  /* Begin */
  /*********/

  dprintf("local_alloc_list_request(): request received, sending reply\n");

  if(!svc_sendreply(transp,
		    (xdrproc_t)xdr_dsm_allocation_list,
		    (char *)alloclist)) {
    svcerr_systemerr(transp);
    fprintf(stderr, "local_alloc_list_request(): error replying\n");
  }

  return;
}
  

/***************************************************/
/* a local process wants to read a value, or wants */
/* to know information about an allocation         */
/***************************************************/
void local_query(SVCXPRT *transp, int function) {
  extern u_long                my_address;
  struct dsm_transaction       reply;
  struct decoded_request_args *drap;
  int                          i,j,s,class_as_target;

  /*********/
  /* Begin */
  /*********/
  dprintf("local_query(): request for %s\n",
    function==DSM_LOCAL_READ ? "DSM_LOCAL_READ" : "DSM_ALLOC_INFO_QUERY");

  class_as_target =  function==DSM_LOCAL_READ     ? 
		     DSM_PROHIBIT_CLASS_AS_TARGET :
		     DSM_ALLOW_CLASS_AS_TARGET;

  /* what does it want? */
  s = decode_request(transp, &drap, class_as_target);
  
  switch(s) {
  case DSM_SUCCESS:
    if(drap->alhp != (struct alloc_list_head *)NULL) {
      /* target is a host */
      dprintf("local_query(): arguments: %s:%s\n",
	      drap->alhp->machine_name, drap->aep->name);
    }
    else if(drap->csp != (struct class_share *)NULL) {
      /* target is a class */
      dprintf("local_query(): arguments: %s:%s\n",
	      drap->csp->class->class_name, drap->csaep->allocname);
    }
    else {
      fprintf(stderr, "local_query: drap->csp and drap->alhp both NULL\n");
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      return;
    }
    break;

  case DSM_GETARGS:
    fprintf(stderr, "local_query(): decode_request() can't decode args\n");
    svcerr_decode(transp);
    return;

  case DSM_INTERNAL_ERROR:
  case DSM_TARGET_INVALID:
  case DSM_ALLOC_VERS:
  case DSM_NAME_INVALID:
    dprintf("local_query(): decode_request returned %d\n", s);
    (void)dsm_return_status_code(transp, s);
    return;

  default:
    dprintf("local_query(): decode_request() return %d unknown\n", s);
    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
    return;
  }
     
  /* okay, we have a valid target name and a valid allocation name; send
     the results back to the other end */
  
  if(function == DSM_LOCAL_READ) {
    /* if this is call for a remotely shared value, and there has been
       no connection made with the other end, we won't have the most up
       to date value (the other end might have written values before we
       were started); if there's no connection, try to establish it, and
       tell the client to do its LOCAL_READ call again; by then we'll
       have the up to date value if there is one */
    if(drap->alhp->machine_addr != my_address) {

      dprintf("local_query(): locking connect_mutex (%s)\n",
	      drap->alhp->machine_name);
      s = pthread_mutex_lock(&drap->alhp->connect_mutex);
      if(s!=0) {
	fprintf(stderr,
		"local_query(): pthread_mutex_lock(connect) returned %d\n",
		s);
	(void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
	
	free_decoded_request_args(drap);
	return;
      }
      else
	dprintf("local_query(): connect mutex locked\n");
      
      /* if someone else is trying to make this connection, let them
	 proceed; tell the client to try again */
      if(drap->alhp->connect_in_progress == DSM_TRUE) {
	dprintf("local_query(): connection to %s in progress\n",
		drap->alhp->machine_name);
	reply.status = DSM_NO_CONNECTION;
      }
      else if(drap->alhp->cl == (CLIENT *)NULL) {
	/* no connection; try to establish it, the process of which, if
	   succesful, will include getting a copy of the data from the
	   other end */
	dprintf("local_query(): no connection to %s, launching it\n",
		drap->alhp->machine_name);
	reply.status = DSM_NO_CONNECTION;
	dsm_launch_connection_thread(drap->alhp);
      }
      else {
	reply.status = DSM_SUCCESS;
      }
      MUTEX_UNLOCK(&drap->alhp->connect_mutex,
		   "local_query",
		   drap->alhp->machine_name);
      if(reply.status == DSM_NO_CONNECTION) {
	dprintf("local_query(): requesting that client retry read request\n");
      }

    }
    else {
      reply.status = DSM_SUCCESS;
      dprintf("local_query(): locally shared space; no "
	      "connection check required\n");
    }


    /* now, whether or not the connection was good, we return the data
       value to the client; this is because it could be the second
       request in the dsm_read() call, in which case the client should
       get whatever we have, regardless of whether we were able to
       establish the connection */
    dprintf("local_query(): locking alloc mutex\n");
    s = pthread_mutex_lock(&drap->aep->mutex);
    if(s!=0) {
      fprintf(stderr,
	      "local_query(): pthread_mutex_lock(alloc) returned %d\n", 
	      s);
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      
      free_decoded_request_args(drap);
      return;
    }
    else
      dprintf("local_query(): alloc mutex locked\n");
    
    /* put together the reply */
    dprintf("local_query(): assembling reply\n");
    reply.targetname   = drap->alhp->machine_name;
    reply.allocname    = drap->aep->name;
    reply.data_size    = (u_int)drap->aep->size;
    reply.n_elements   = (u_int)drap->aep->n_elements;
    reply.is_structure = (u_int)drap->aep->is_structure;
    reply.timestamp    = (u_long)drap->aep->timestamp;
    reply.notify       = DSM_FALSE; /* dummy */

    /* get memory for the elements array */
    reply.elements = 
      (struct alloc_element *)malloc(drap->aep->n_elements
				     *
				     sizeof(struct alloc_element));
    if(reply.elements == (struct alloc_element *)NULL) {
      perror("local_query(): malloc(elements) failed");
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);

      dprintf("local_query(): unlocking alloc mutex\n");
      s = pthread_mutex_unlock(&drap->aep->mutex);
      if(s!=0)
	fprintf(stderr,
		"local_query(): pthread_mutex_unlock() returned %d\n",
		s);
      else 
	dprintf("local_query(): alloc mutex unlocked\n");

      free_decoded_request_args(drap);
      return;
    }

    /* fill in the elements */
    for(i=0; i<drap->aep->n_elements; i++) {
      strcpy(reply.elements[i].name, drap->aep->elements[i].name);
      reply.elements[i].size           = drap->aep->elements[i].size;
      reply.elements[i].n_sub_elements = drap->aep->elements[i].n_sub_elements;

      /* it's a local transaction, so there is no byte-order confusion */
      reply.elements[i].xdr_filter_index = XDR_FILTER_OPAQUE;

      reply.elements[i].datap = (char *)malloc(drap->aep->elements[i].size);
      if(reply.elements[i].datap == (char *)NULL) {
	fprintf(stderr, "local_query(): malloc(%s) failed\n",
		reply.elements[i].name);
	perror("malloc()");
	(void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);

	dprintf("local_query(): unlocking alloc mutex\n");
	s = pthread_mutex_unlock(&drap->aep->mutex);
	if(s!=0)
	  fprintf(stderr,
		  "local_query(): pthread_mutex_unlock() returned %d\n",
		  s);
	else 
	  dprintf("local_query(): alloc mutex unlocked\n");

	for(j=0; j<i; j++) free(reply.elements[j].datap);
	free(reply.elements);
	free_decoded_request_args(drap);
	return;
      }
      
      dprintf("local_query(): copying element %s to reply buffer\n",
	      reply.elements[i].name);
      memcpy(reply.elements[i].datap,
	     drap->aep->elements[i].datap,
	     drap->aep->elements[i].size);
    }

    /* now that we have the data we can release the mutex */
    MUTEX_UNLOCK(&drap->aep->mutex, "local_query", "local");
  }
  else if(function == DSM_ALLOC_INFO_QUERY) {
    if(drap->csp == (struct class_share *)NULL) {
      /* request target is a single host */
      reply.targetname = drap->alhp->machine_name;
      reply.allocname  = drap->aep->name;
    }
    else if(drap->alhp == (struct alloc_list_head *)NULL) {
      /* request target is a class */
      reply.targetname = drap->csp->class->class_name;
      reply.allocname  = drap->csaep->allocname;

      /* since this query returns a description of the alloc, but not
	 its data, we can use the alloc_entry from any of the hosts in
	 the class */
      drap->aep = drap->csaep->aepp[0];
    }
    else {
      fprintf(stderr, "local_query: request target not class or host\n");
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      free_decoded_request_args(drap);
      return;
    }

    reply.data_size    = (u_int)drap->aep->size;
    reply.n_elements   = (u_int)drap->aep->n_elements;
    reply.is_structure = (u_int)drap->aep->is_structure;
    
    /* get memory for the elements array */
    reply.elements = (struct alloc_element *)
      malloc(drap->aep->n_elements * sizeof(struct alloc_element));
    if(reply.elements == (struct alloc_element *)NULL) {
      perror("local_query(): malloc(elements) in SIZEQ failed");
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      free_decoded_request_args(drap);
      return;
    }

    for(i=0; i<drap->aep->n_elements; i++) {
      strcpy(reply.elements[i].name, drap->aep->elements[i].name);
      reply.elements[i].size           = (u_int)drap->aep->elements[i].size;
      reply.elements[i].n_sub_elements = drap->aep->elements[i].n_sub_elements;

      /* local transaction so don't need byte order info */
      reply.elements[i].xdr_filter_index = XDR_VOID;

      reply.elements[i].datap = NULL; /* dummy */
    }
    reply.timestamp = 0;           /* dummy */
    reply.notify    = DSM_FALSE;   /* dummy */
    reply.status    = DSM_SUCCESS; /* dummy */
  }

  dprintf("local_query(): calling svc_sendreply()\n");
  if(!svc_sendreply(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&reply)) {
    fprintf(stderr, "local_query(): svc_sendreply() failed\n");
    svcerr_systemerr(transp);
  }
  else 
    dprintf("local_query(): svc_sendreply() successful\n");

  if(function==DSM_LOCAL_READ)
    for(i=0; i<reply.n_elements; i++) free(reply.elements[i].datap);

  free(reply.elements);
  free_decoded_request_args(drap);
  return;
}

/************************************/
/* wait for an allocation to change */
/************************************/
void local_read_wait(SVCXPRT *transp) {
  extern pthread_mutex_t notification_table_mutex;
  extern pthread_mutex_t notification_ring_mutex;
  extern struct notification_table_entry *notification_table;
  extern int *dsm_nproc_notify_max_p;

  struct dsm_read_wait_req rwq;
  struct dsm_transaction reply;

  struct notification_table_reference *ntrp,*ntrptemp;

  struct alloc_list_head *alhp;
  struct alloc_entry     *aep;
  struct alloc_element   element;

  struct alloc_entry     **aep_array;
  
  int i,j,s;

  int notification_table_index;

  bool_t retval;

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  union semun dummy_semarg;
  extern int semid;
#endif

  /*********/
  /* Begin */
  /*********/

  dprintf("local_read_wait(): decoding args\n");
  
  if(!svc_getargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq)) {
    /* couldn't decode the arguments */
    fprintf(stderr, "local_read_wait(): svc_getargs() failed\n");

    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
      fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
    
    svcerr_decode(transp);
    return;
  }

  /* get the mutex which protects the notification table */
  dprintf("local_read_wait(): getting notification_table_mutex\n");
  s = pthread_mutex_lock(&notification_table_mutex);
  if(s!=0) {
    fprintf(stderr,
	    "local_read_wait(): pthread_mutex_lock(nottab) returned %d\n",
	    s);

    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
      fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
    
    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
    
    return;
  }

  /***************************************/
  /* first, we have to get a spot in the */
  /* notification table for this pid/tid */
  /***************************************/

  /* is there already an entry in the notification table for this pid? */
  dprintf("local_read_wait(): looking for notification table entry"
	  " for pid %d, tid %d\n", 
	  (int)rwq.pid,
	  (int)rwq.tid);
  notification_table_index = -1;

  for(i=0; i<*dsm_nproc_notify_max_p; i++) {
    if(notification_table[i].pid == (pid_t)(rwq.pid)
       &&
       notification_table[i].tid == (int)(rwq.tid)) {
      notification_table_index = i;
      dprintf("local_read_wait(): found pid:tid %d:%d at index %d\n",
	      (int)rwq.pid, (int)rwq.tid, i);
      break;
    }
  }
  
  if(notification_table_index == -1) {
    /* we have to make an entry in the notification table for this pid/tid */
    dprintf("local_read_wait(): no entry found for pid:tid %d:%d\n",
	    (int)rwq.pid, (int)rwq.tid);
    for(i=0; i<*dsm_nproc_notify_max_p; i++) {
      if(notification_table[i].pid == (pid_t)0) {
	/* here's an empty slot; use it */
	notification_table_index = i;
	dprintf("local_read_wait(): using empty (pid=0) slot %d\n", i);
	break;
      }
    }
    
    /* did we find a free one? */
    if(notification_table_index == -1) {
      /* no; get a non-free, but unused one */
      dprintf("local_read_wait(): no empty (pid=0) slot; "
	      "finding unused (sem=1) entry\n");
      for(i=0; i<*dsm_nproc_notify_max_p; i++) {

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
	j = semctl(semid, i, GETNCNT, &dummy_semarg);
	if(j==-1) {
	  perror("local_read_wait: semctl(GETNCNT)");
	  if(!svc_freeargs(transp, 
			   (xdrproc_t)xdr_dsm_read_wait_req, 
			   (char *)&rwq))
	    fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
	  (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
	  MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
	  return;
	}
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
	s = sem_getvalue(&notification_table[i].sem, &j);
	if(s!=0) {
	  perror("local_read_wait: sem_getvalue");
	  if(!svc_freeargs(transp, 
			   (xdrproc_t)xdr_dsm_read_wait_req, 
			   (char *)&rwq))
	    fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");

	  (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
	  MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
	  return;
	}
#endif
	if(j==0) {
	  /* we can use this one */
	  notification_table_index = i;
	  dprintf("local_read_wait(): found slot %d with sem=0\n", i);
	  break;
	}
      }
    }

    /* now did we find one? */
    if(notification_table_index == -1) {
      if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
	fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");

      dprintf("local_read_wait(): no notification table slots left\n");
      (void)dsm_return_status_code(transp, DSM_TOO_MANY_PROC);
      MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
      return;
    }
  }

  /* clear the hostname, allocname, and size fields just for aesthetics */
  dprintf("local_read_wait(): clearing hostname, allocname, and size\n");
  notification_table[notification_table_index].hostname[0]  = '\0';
  notification_table[notification_table_index].allocname[0] = '\0';
  notification_table[notification_table_index].size         = 0;

  notification_table[notification_table_index].pid = (pid_t)(rwq.pid);
  notification_table[notification_table_index].tid = (int)  (rwq.tid);

  /**********************************************/
  /* okay, we have a notification table index;  */
  /* now we can set up for notification on this */
  /* pid/tid                                    */
  /**********************************************/
  
  /* loop through the requests, adding each one to the alloc list as
     necessary */

  /* get the mutex which protects the notification rings */
  dprintf("local_read_wait(): getting notification_ring_mutex\n");
  s = pthread_mutex_lock(&notification_ring_mutex);
  if(s!=0) {
    fprintf(stderr,
	    "local_read_wait(): pthread_mutex_lock(notring) returned %d\n",
	    s);
    
    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
    
    MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
      
    return;
  }


  /* go over the list of requests, doing some housekeeping for each one */

  /* we need an array to store the aep values */
  dprintf("local_read_wait(): malloc array of alloc_entry pointers\n");
  aep_array = (struct alloc_entry **)
    malloc(rwq.num_alloc * sizeof(struct alloc_entry *));

  for(i=0; i<rwq.num_alloc; i++) {
    dprintf("local_read_wait(): examining element %d in monitor list\n", i);
    
    /* find the machine name */
    alhp = lookup_machine_name(rwq.hostnames + i*rwq.string_size);
    if(alhp == (struct alloc_list_head *)NULL) {
      dprintf("local_read_wait(): host invalid\n");

      if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
	fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
      
      (void)dsm_return_status_code(transp, DSM_TARGET_INVALID);

      MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
      MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
    
      free(aep_array);

      return;
    }
    dprintf("local_read_wait(): host %s okay\n", 
	    rwq.hostnames + i*rwq.string_size);

    aep = lookup_allocation(alhp, rwq.allocnames + i*rwq.string_size);
    if(aep == (struct alloc_entry *)NULL) {
      dprintf("local_read_wait(): alloc name invalid\n");

      if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
	fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
      
      (void)dsm_return_status_code(transp, DSM_NAME_INVALID);
    
      MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
      MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");

      free(aep_array);

      return;
    }      
    dprintf("local_read_wait(): alloc %s okay\n", 
	    rwq.allocnames + i*rwq.string_size);


    /* now we have the alloc entry; do the checking and add its address
       to the array */
    dprintf("local_read_wait(): locking alloc mutex\n");
    s = pthread_mutex_lock(&aep->mutex);
    if(s!=0) {
      fprintf(stderr,
	      "local_read_wait(): pthread_mutex_lock(alloc) returned %d\n",
	      s);
      
      if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
	fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
      
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);

      MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
      MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");

      free(aep_array);

      return;
    }

    /* put the address of this alloc into the array so
       create_notification_ring() can find it without looking it up
       again */
    dprintf("local_read_wait(): adding aep = 0x%p to aep_array[%d]\n",
	    aep, i);
    aep_array[i] = aep;


    /* first we do some housekeeping; go through the list, checking
    ** whether this pid/tid is already in it; if it is, it means that a
    ** process or thread died before it was notified, so its
    ** notification ring is still here; remove it
    **
    ** we also check that the entries already in the list are associated
    ** with semaphores that processes/threads are actually waiting on;
    ** if not, we remove them 
    */
    
    ntrp = aep->notification_list;

    while(ntrp != (struct notification_table_reference *)NULL) {
      dprintf("local_read_wait(): notf list checking for ntrp=0x%p\n",ntrp);

      if(ntrp->pid==(pid_t)(rwq.pid)  &&  ntrp->tid==(int)(rwq.tid)) {
	/* there's already an entry in this list for this pid/tid;
	   remove its notification ring (a pid/tid may only have one
	   notification ring) */
	dprintf("local_read_wait(): already an entry for pid:tid %d:%d\n",
		ntrp->pid, ntrp->tid);
	dprintf("local_read_wait(): calling destroy_notification_ring for "
		"pid:tid %d:%d\n",
		ntrp->pid, ntrp->tid);
	
	ntrptemp = ntrp;
	ntrp = ntrp->next;

	s = destroy_notification_ring(ntrptemp);
	if(s!=0) {
	  fprintf(stderr,
		  "local_read_wait(): destroy_notification_ring() failed\n");
      
	  if(!svc_freeargs(transp,
			   (xdrproc_t)xdr_dsm_read_wait_req, 
			   (char *)&rwq))
	    fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
      
	  (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);

	  MUTEX_UNLOCK(&aep->mutex,               "local_read_wait", "");
	  MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
	  MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");

	  free(aep_array);

	  return;
	}
	
	continue;
      }

      /* at this point we know that this entry is not for this pid:tid;
      ** now do some garbage collection unrelated to this pid:tid: make
      ** sure there's really still a process waiting on the semaphore
      ** for this entry 
      */

      /* When a process sets up for read_wait() notification, it has
	 some things to do between receiving the notification table
	 index from the server and waiting on the semaphore.  In that
	 time, if another local_read_wait() call is made, this
	 housecleaning code will incorrectly determine that the
	 notification table slot belongs to a defunct process when in
	 reality it just hasn't quite gotten to waiting on the
	 semaphore.  To prevent this, local_read_wait() (this function),
	 just before returning the notification table index to the
	 client, notes the current time.  The garbage collection code
	 here checks that time against the clock and takes no action
	 unless it's been a while since the setup.  This should give the
	 clients enough time to get to waiting on the sempahores. 
      
	 2 seconds should be way more than enough, no? Actually, 2 means
	 > 1, since the time could be set at 0.99999 and then read at
	 1.00001, which with the time() call would be a difference of 1.
      */

      if(time(NULL) - notification_table[ntrp->index].when_set > 2) {

	dprintf("local_read_wait(): no notf ring; check for blocked sem\n");
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
	j = semctl(semid, ntrp->index, GETNCNT, &dummy_semarg);
	if(j==-1) {
	  fprintf(stderr, 
		  "local_read_wait(): index %d semctl(GETNCNT): %s",
		  ntrp->index,
		  strerror(errno));
	  ntrp = ntrp->next;
	  continue;
	}
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
	s = sem_getvalue(&notification_table[ntrp->index].sem, &j);
	if(s!=0) {
	  fprintf(stderr, 
		  "local_read_wait(): index %d sem_getvalue: %s",
		  ntrp->index,
		  strerror(errno));
	  ntrp = ntrp->next;
	  continue;
	}
#endif
	if(j==0) {
	  /* no one is waiting; notification ring not needed */
	  dprintf("local_read_wait(): sem[%d]: 0 waiters; "
		  "removing notification ring\n",
		  ntrp->index);
	  
	  ntrptemp = ntrp;
	  ntrp = ntrp->next;
	  
	  s = destroy_notification_ring(ntrptemp);
	  if(s!=0) {
	    fprintf(stderr,
		    "local_read_wait(): destroy_notification_ring() failed\n");
	    
	    if(!svc_freeargs(transp, 
			     (xdrproc_t)xdr_dsm_read_wait_req, 
			     (char *)&rwq))
	      fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
	    
	    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
	    
	    MUTEX_UNLOCK(&aep->mutex,               "local_read_wait", "");
	    MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
	    MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
	    
	    free(aep_array);
	    
	    return;
	  }
	  
	  continue;
	} /* if no one was waiting on this semaphore */

      } /* if when_set is far enough in the past */
      else {
	dprintf("local_read_wait(): no notf ring cleanup done "
		"on ind=%d; too fresh\n", ntrp->index);
      }

      /* go on to the next node in this alloc's notification list */
      ntrp = ntrp->next;

    } /* while loop over notification list for this alloc */

    MUTEX_UNLOCK(&aep->mutex, "local_read_wait", "");
	  
  } /* loop over allocs in read_wait request */


  /* at this point all old references to this pid/tid are removed from
  ** any notification rings, and any notification_table_reference
  ** entries which refer to semaphores that no one is waiting on, are
  ** also removed 
  **
  ** we may now add a new notification ring for this request 
  */
  dprintf("local_read_wait(): calling create_notification_ring()\n");

  s = create_notification_ring(&rwq, aep_array, notification_table_index);
  if(s!=0) {
    fprintf(stderr,
	    "local_read_wait(): create_notification_ring() failed\n");
    
    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
      fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
    
    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
    
    MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
    MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");

    free(aep_array);

    return;
  }

  /* send back the index number (as long) */
  reply.targetname   = "dummy";
  reply.allocname    = "dummy";
  reply.data_size    = (u_int)sizeof(long);
  reply.n_elements   = (u_int)1;
  reply.is_structure = (u_int)DSM_FALSE;

  strcpy(element.name, "dummy");
  element.size             = reply.data_size;
  element.n_sub_elements   = 1;
  element.xdr_filter_index = XDR_FILTER_LONG;
  element.datap            = &notification_table_index;

  reply.elements  = &element;
  reply.timestamp = 0; /* dummy */
  reply.notify    = DSM_FALSE; /* dummy */
  reply.status    = DSM_SUCCESS;

  /* note the time we set this up; this is used to keep subsequent
     local_read_wait() calls from harvesting the notification ring,
     thinking that the waiting process exited when in reality it simply
     hasn't had enough time to finish its own setup and wait on the
     semaphore */
  notification_table[notification_table_index].when_set = time(NULL);  

  dprintf("local_read_wait(): returning index %d to caller\n",
	  *((long *)reply.elements->datap));
  
  retval = svc_sendreply(transp, 
			 (xdrproc_t)xdr_dsm_transaction,
			 (char *)&reply);
  if(!retval) {
    fprintf(stderr, "local_read_wait(): svc_sendreply() failed\n");
    svcerr_systemerr(transp);
  }
  
  /* done; clean up */
  MUTEX_UNLOCK(&notification_ring_mutex,  "local_read_wait", "");
  MUTEX_UNLOCK(&notification_table_mutex, "local_read_wait", "");
  
  if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_read_wait_req, (char *)&rwq))
    fprintf(stderr, "local_read_wait(): svc_freeargs() failed\n");
  
  free(aep_array);

  dprintf("local_read_wait(): finished\n");
  return;
}



/******************************************/
/* a local process wants to write a value */
/******************************************/
void local_write(SVCXPRT *transp, struct alloc_list_head *alhp) {
  extern u_long my_address;
  extern struct decoded_request_args *request_queue[];
  extern int request_queue_entries;
  extern pthread_mutex_t request_queue_mutex;
  extern sem_t request_queue_semaphore;
  
  struct decoded_request_args *drap,*newdrap;
  time_t now;
  int i,j,s,end;

  /*********/
  /* Begin */
  /*********/

  dprintf("local_write(): request for DSM_LOCAL_WRITE; decoding args\n");
  
  /* extract the arguments to find out what the client wants */
  s = decode_request(transp, &drap, DSM_ALLOW_CLASS_AS_TARGET);

  switch(s) {
  case DSM_SUCCESS:
    if(drap->alhp != (struct alloc_list_head *)NULL) {
      /* target is a host */
      dprintf("local_write(): arguments: %s:%s\n", 
	      drap->alhp->machine_name, drap->aep->name);
    }
    else if(drap->csp != (struct class_share *)NULL) {
      /* target is a class */
      dprintf("local_write(): arguments: %s:%s\n",
	      drap->csp->class->class_name, drap->csaep->allocname);
    }
    else {
      fprintf(stderr, "local_write: drap->csp and drap->alhp both NULL\n");
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      return;
    }
    break;
    
  case DSM_GETARGS:
    fprintf(stderr, "local_write(): decode_request() can't decode args\n");
    svcerr_decode(transp);
    return;
    
  case DSM_INTERNAL_ERROR:
  case DSM_TARGET_INVALID:
  case DSM_ALLOC_VERS:
  case DSM_NAME_INVALID:
    dprintf("local_write(): decode_request returned %d\n", s);
    (void)dsm_return_status_code(transp, s);
    return;
    
  default:
    fprintf(stderr, "local_write(): decode_request() return %d unknown\n", s);
    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
    return;
  }
  
  /******************************************/
  /* okay, everything seems to be in order; */
  /* write the new value to local storage   */
  now = time(NULL);

  if(drap->csp != (struct class_share *)NULL) 
    end = drap->csp->class->nmembers - 1;
  else
    end = 0;

  for(i=0; i<=end; i++) {
    if(drap->csp != (struct class_share *)NULL) 
      drap->aep = drap->csaep->aepp[i];

    dprintf("local_write(): locking alloc mutex\n");
    s = pthread_mutex_lock(&drap->aep->mutex);
    if(s!=0) {
      fprintf(stderr, "local_write(): pthread_mutex_lock() returned %d\n", s);
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      free_decoded_request_args(drap);
      return;
    }
    else {
      dprintf("local_write(): got alloc mutex\n");
    }
    
    /* got the mutex; copy the data into local storage, set timestamp */
    dprintf("local_write(): copying data to local storage for %s\n",
	    drap->aep->owner->machine_name);
    for(j=0; j<drap->aep->n_elements; j++)
      memcpy(drap->aep->elements[j].datap,
	     drap->datapp[j],
	     drap->aep->elements[j].size);
    drap->aep->timestamp = now;

    dprintf("local_write(): unlocking alloc mutex\n");
    s = pthread_mutex_unlock(&drap->aep->mutex);
    if(s!=0)
      fprintf(stderr, "local_write: pthread_mutex_unlock returned %d\n", s);
    else { 
      dprintf("local_write: alloc mutex unlocked\n"); 
    }
  
    /* if notify is specified, call the function to do it */
    if(drap->notify==DSM_TRUE) {
      dprintf("local_write(): calling dsm_notify_waiting_procs()\n");
      dsm_notify_waiting_procs(drap->aep->owner->machine_name, drap->aep);
    }
  }
  

  /* for each space that is shared remotely, put a request on the
     queue and signal the thread pool to service it */
  for(i=0; i<=end; i++) {
    s = duplicate_decoded_request_args(&newdrap, drap);
    if(s!=DSM_SUCCESS) {
      fprintf(stderr, "dup_dec_req_args: failed\n");
      (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
      free_decoded_request_args(drap);
      return;
    }

    if(drap->csp != (struct class_share *)NULL) {
      newdrap->aep = newdrap->csaep->aepp[i];
      newdrap->alhp = newdrap->csaep->aepp[i]->owner;
    }

    if(newdrap->alhp->machine_addr != my_address) {
      MUTEX_LOCK(&request_queue_mutex, "local_write", "");

      if(request_queue_entries >= DSM_REQ_QUEUE_LENGTH) {
	MUTEX_UNLOCK(&request_queue_mutex, "local_write", "");
	fprintf(stderr,
		"local_write(): request_queue full - no RPC request made\n");
      }
      else {
	/* put the request on the request queue, and post the semaphore to
	   get one of the threads to service it */
	dprintf("local_write(): putting request on queue position %d\n", 
		request_queue_entries);
	
	request_queue[request_queue_entries] = newdrap;
	request_queue_entries++;

	MUTEX_UNLOCK(&request_queue_mutex, "local_write", "");

	s = sem_post(&request_queue_semaphore);
	if(s!=0) {
	  perror("local_write(): sem_post()");
	  (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
	  free_decoded_request_args(newdrap);
	  free_decoded_request_args(drap);
	  return;
	}
	else {
	  dprintf("local_write(): posted request_queue_semaphore\n");
	}
      }

      /* we do not free drap or drap->data because remote_write_giver()
	 will do that when it is done servicing the request */
    }
    else {
      /* locally shared; newdrap won;'t be handled by
	 remote_write_giver, so we must free it ourselves */
      free_decoded_request_args(newdrap);
    }
  }

  /* now reply, clean up, and exit */
  dprintf("local_write(): sending local reply (DSM_SUCCESS)\n");
  (void)dsm_return_status_code(transp, DSM_SUCCESS);

  /* done with the original request */
  free_decoded_request_args(drap);

  dprintf("local_write(): completed successfully\n");

  return;
}


/*******************************************************/
/* this is run as a thread in the thread pool; it is   */
/* activated via the request_queue_semaphore by        */
/* local_write(), and does all the actual writing and  */
/* synchronizing work (the name means it gives written */
/* values to the remote end)                           */
/*******************************************************/
void remote_write_giver(int threadpoolno) {
  struct dsm_transaction       req,reply;
  struct decoded_request_args *current_request_p;
  int                          i, s, queue_index,
                               on_in_progress_list, serviced_a_request,
                               attempts;
  pthread_t                    thread;
  char                         threadlabel[10];

  extern u_long                       my_address;
  extern char                         myhostname[];
  extern struct decoded_request_args *request_queue[];
  extern struct decoded_request_args *inprogress_list[];
  extern int                          request_queue_entries, 
                                      inprogress_list_entries;
  extern sem_t                        request_queue_semaphore;
  extern pthread_mutex_t              request_queue_mutex;

  /*********/
  /* Begin */
  /*********/

  sprintf(threadlabel,"%d",threadpoolno);

  /* Unfortunately, this threadpool thing doesn't work under unpatched
  ** LynxOS 3.1.0a.  The machine locks up with no error messages.  User
  ** space code just should not be able to do this.  It appears to be
  ** due to some sort of non-threadsafe behavior in the TCP/IP stack or
  ** in the RPC library, since when I removed the clnt_call() statement,
  ** the crashing stopped.
  **
  ** Patching LynxOS 3.1.0a seems to fix the problem, but it causes much
  ** of the rest of the SMA software to break, so it's not a good
  ** solution.  Perhaps things will be better under LynxOS 4.0 when we
  ** get there.  Meanwhile, to get around this while maintaining the
  ** basic threadpool idea, I'll change these threads so that instead of
  ** being persistent, they start a new one and themselves exit when
  ** they are done with their work.  This behavior is activated by
  ** defining the macro BROKEN_THREADPOOL; without it, we get the
  ** normal, persistent thread pool.
  */
  while(1) {
    dprintf("remote_write_giver(%d): waiting on semaphore\n",
	    threadpoolno);
    s = sem_wait(&request_queue_semaphore);
    if(s!=0) {
      dprintf("remote_write_giver(%d): sem_wait() returned %d\n",
	      threadpoolno, s);
      sleep(1); /* prevent infinite cpu bound loop */
      continue;
    }

    dprintf("remote_write_giver(%d): request received\n", threadpoolno);

#ifdef BROKEN_THREADPOOL
    break;
  }
#endif

    /******************************************************************/
    /* we got signalled; there must be a request on the queue; get it */
    /* and service it */

    /* get the mutex */
    MUTEX_LOCK(&request_queue_mutex, "remote_write_giver", threadlabel);

    serviced_a_request = DSM_FALSE;

    /* start at the front of the queue */
    for(queue_index = 0;
	queue_index < request_queue_entries;
	queue_index++) {

      /* look at this queue entry; is there already in progress a
	 request for this allocation? */
      dprintf("remote_write_giver(%d): checking in-progress list\n",
	      threadpoolno);
      on_in_progress_list = DSM_FALSE;
      for(i=0; i<inprogress_list_entries; i++) {
	/* compare the host and the allocation */
	if( !strcmp(request_queue[queue_index]->alhp->machine_name,
		    inprogress_list[i]->alhp->machine_name) 
	    &&
	    !strcmp(request_queue[queue_index]->aep->name,
		    inprogress_list[i]->aep->name)   ) {
	  /* it matches! bad bad bad */
	  on_in_progress_list = DSM_TRUE;
	  dprintf("remote_write_giver(%d): request matches one "
		  "in progress; skipping\n",threadpoolno);
	  break;
	}
      }

      /* if it's on the list, we have to go on to the next one */
      if(on_in_progress_list == DSM_TRUE) continue;
      else {
	/* otherwise, service it */

	/* put it on the in-progress list */
	dprintf("remote_write_giver(%d): request not on in-progress list\n",
		threadpoolno);
	dprintf("remote_write_giver(%d): move from queue pos %d to "
		"in-progress pos %d\n",
		threadpoolno, queue_index, inprogress_list_entries);


	if(inprogress_list_entries==DSM_REQ_QUEUE_LENGTH) {
	  /* the list is full; we can't process this */
	  dprintf("remote_write_giver(%d): error - inprogress list full\n",
		  threadpoolno);
	  break;
	}

	inprogress_list[inprogress_list_entries] = request_queue[queue_index];
	current_request_p = request_queue[queue_index];
	inprogress_list_entries++;

	/* remove it from the request queue */
	dprintf("remote_write_giver(%d): removing request "
		"from request queue\n",
		threadpoolno);
	for(i=queue_index; i<request_queue_entries-1; i++)
	  request_queue[i] = request_queue[i+1];

	request_queue[request_queue_entries-1] = 
	  (struct decoded_request_args *)NULL;

	request_queue_entries--;

	dprintf("remote_write_giver(%d): req q entries=%d  "
		"inprog list entries=%d\n",
		threadpoolno,
		request_queue_entries,
		inprogress_list_entries);

	/* we're done with the lists for now, so release the mutex */
	MUTEX_UNLOCK(&request_queue_mutex, "remote_write_giver", threadlabel);
	
	/* finally, we can service the request */

	/* assemble args */
	dprintf("remote_write_giver(%d): assembling args: %s:%s\n",
		threadpoolno,
		current_request_p->alhp->machine_name, 
		current_request_p->aep->name);
	req.targetname   = myhostname;
	req.allocname    = current_request_p->aep->name;
	req.data_size    = current_request_p->aep->size;
	req.n_elements   = current_request_p->aep->n_elements;
	req.is_structure = current_request_p->aep->is_structure;

	req.elements = 
	  (struct alloc_element *)malloc(req.n_elements 
					 *
					 sizeof(struct alloc_element));
	if(req.elements == (struct alloc_element *)NULL) {
	  fprintf(stderr,
		  "remote_write_giver(%d): malloc(req.elements): %s\n",
		  threadpoolno, strerror(errno));
	  fprintf(stderr, "remote_write_giver(%d): can't service request\n",
		  threadpoolno);
	  serviced_a_request = DSM_INTERNAL_ERROR;
	}
	else {
	  for(i=0; i<req.n_elements; i++) {
	    struct alloc_element *el = &req.elements[i];
	    strcpy(el->name, current_request_p->aep->elements[i].name);
	    el->size             = 
	      (u_int)current_request_p->aep->elements[i].size;
	    el->n_sub_elements   =
	      (u_int)current_request_p->aep->elements[i].n_sub_elements;
	    el->xdr_filter_index =
	      current_request_p->aep->elements[i].xdr_filter_index;
	    el->datap            = current_request_p->datapp[i];
	  }
	  
	  req.notify           = current_request_p->notify;
	  req.timestamp        = 0; /* dummy */
	  req.status           = DSM_SUCCESS; /* dummy */
	  
	  /* make the filter allocate space */
	  reply.targetname = (char *)NULL;
	  reply.allocname  = (char *)NULL;
	  reply.elements   = (struct alloc_element *)NULL;
	  
	  serviced_a_request = DSM_TRUE;
	  attempts = 0;
	  
	  while(1) {
	    dprintf("remote_write_giver(%d): calling "
		    "dsm_robust_client_call(DSM_REMOTE_WRITE)\n",
		    threadpoolno);
	    s = dsm_robust_client_call(current_request_p->alhp,
				       DSM_REMOTE_WRITE,
				       (xdrproc_t)xdr_dsm_transaction,
				       (char *)&req,
				       (xdrproc_t)xdr_dsm_transaction,
				       (char *)&reply);
	    attempts++;
	    if(s!=DSM_SUCCESS) {

	      if( (s==DSM_NO_CONNECTION || s==DSM_CONNECT_IN_PROGRESS)
		  &&
		  attempts < 3 ) {
		/* write couldn't be performed because there was no
		   connection; a connection attempt is now in progress,
		   so wait a little and try again */
		dprintf("remote_write_giver(%d): dsm_robust_client_call(): "
			"no connection after try %d\n",
			threadpoolno, attempts);
		usleep(300000);
	      }
	      else {
		dprintf("remote_write_giver(%d): "
			"dsm_robust_client_call() returned %d on try %d\n",
			threadpoolno, s, attempts);
		break;
	      }
	    }
	    else {
	      /* the RPC call succeeded; was the server able to fulfill the
		 request? */ 
	      if(reply.status == DSM_WRITE_REBUFFED) {
		/* ouch!  the other end refused to complete our request
		   because it was already doing some writing to this
		   location; shut down and try again in a little while */
		dprintf("remote_write_giver(%d): rebuffed!\n",threadpoolno);
		
		/* free buffers allocated in dsm_robust_client_call() */
		xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
		
		/* conflict resolution: whoever has the larger address
		   gets to go first */ 
		if(current_request_p->alhp->machine_addr > my_address) {
		  dprintf("remote_write_giver(%d): "
			  "I lose arbitration and wait\n",
			  threadpoolno);
		  sleep(1);
		}
		else {
		  dprintf("remote_write_giver(%d): "
			  "I win arbitration and proceed\n",
			  threadpoolno);
		}
		
	      } /* if write was rebuffed by remote */
	      else {
		/* we weren't rebuffed! finish up */
		dprintf("remote_write_giver(%d): synchronization successful\n",
			threadpoolno);
		break;
	      }
	      
	    } /* RPC call succeeded */
	    
	  } /* while 1 */
	  
	  dprintf("remote_write_giver(%d): freeing RPC buffers\n", threadpoolno);
	  xdr_free((xdrproc_t)xdr_dsm_transaction, (char *)&reply);
	  
	  free(req.elements);
	} /* if malloc(req.elements) failed */

	/* remove the request from the in progress list */
	MUTEX_LOCK(&request_queue_mutex, "remote_write_giver", threadlabel);
	for(i=0; i<inprogress_list_entries; i++) {
	  if(inprogress_list[i]==current_request_p) break;
	}
	dprintf("remote_write_giver(%d): removing request from "
		"inprogress pos %d\n",
		threadpoolno, 
		i);
	for( ; i<inprogress_list_entries-1; i++)
	  inprogress_list[i] = inprogress_list[i+1];
	
	inprogress_list[inprogress_list_entries-1] =
	  (struct decoded_request_args *)NULL;

	inprogress_list_entries--;

	free_decoded_request_args(current_request_p);

	dprintf("remote_write_giver(%d): inprogress_list_entries=%d\n",
		threadpoolno,
		inprogress_list_entries);

	MUTEX_UNLOCK(&request_queue_mutex, "remote_write_giver", threadlabel);
      } /* we've serviced a request */

      if(serviced_a_request==DSM_TRUE
	 ||
	 serviced_a_request==DSM_INTERNAL_ERROR) {
	/* break out of the for loop */
	break;
      }
    } /* for loop over queue entries */

    /* if we did not service a request, we need to release the mutex */
    if(serviced_a_request == DSM_FALSE) {
      MUTEX_UNLOCK(&request_queue_mutex, "remote_write_giver", threadlabel);
      
      /* if we're in here, it's because we went through the whole queue
	 and did not find any entry which was not on the in-progress
	 list; since we began execution because a request was added to
	 the queue, we need to re-post the semaphore so someone can try
	 once again to service the request; wait a little time to give
	 the request on the in-progress queue a chance to complete */
      sleep(1);

      dprintf("remote_write_giver(%d): req not processed; posting"
	      " request_queue_semaphore\n",
	      threadpoolno);
      
      s = sem_post(&request_queue_semaphore);
      if(s!=0) {
	fprintf(stderr,
		"remote_write_giver(%d): sem_post: %s\n",
		threadpoolno, strerror(errno));
      }
      else
        dprintf("remote_write_giver(%d): posted request_queue_semaphore\n",
		threadpoolno);
    }

    dprintf("remote_write_giver(%d): completed successfully\n",
	    threadpoolno);
#ifndef BROKEN_THREADPOOL
  } /* while 1 */
#else
#if 0
/* this is here only to make emacs C mode get the indenting right :-( */
{
#endif
  /* start a new thread to replace myself, then exit */
  dprintf("remote_write_giver(%d): starting my replacement thread\n",
	  threadpoolno);
  s = pthread_create(&thread,
		     NULL,
		     (void *)remote_write_giver,
		     (void *)threadpoolno);
  if(s!=0)
    fprintf(stderr,
	    "remote_write_giver(%d): pthread_create() returned %d\n",
	    threadpoolno, s);
  else {
    s = pthread_detach(thread);
    if(s!=0)
      fprintf(stderr,
	      "remote_write_giver(%d): pthread_detach() returned %d\n",
		threadpoolno, s);
  }

  dprintf("remote_write_giver(old %d): exiting\n", threadpoolno);
#endif

}


/*********************************************************************/
/* this receives requests from remote_write_giver() on the other end */
/*********************************************************************/
void remote_write_getter(SVCXPRT *transp) {
  struct decoded_request_args *drap;

  int i,s;

  /*********/
  /* Begin */
  /*********/

  dprintf("remote_write_getter(): calling decode_request()\n");

  /* decode the arguments (let xdr filter allocate memory) */
  s = decode_request(transp, &drap, DSM_PROHIBIT_CLASS_AS_TARGET);

  switch(s) {
  case DSM_SUCCESS:
    dprintf("remote_write_getter(): args: %s:%s\n",
      drap->alhp->machine_name, drap->aep->name);
    break;

  case DSM_GETARGS:
    fprintf(stderr,
	    "remote_write_getter(): decode_request() can't decode args\n");
    svcerr_decode(transp);
    return;
    
  case DSM_INTERNAL_ERROR:
  case DSM_TARGET_INVALID:
  case DSM_ALLOC_VERS:
  case DSM_NAME_INVALID:
    fprintf(stderr,
	    "remote_write_getter(): decode_request returned %d\n", s);
    (void)dsm_return_status_code(transp, s);
    return;
    
  default:
    dprintf("remote_write_getter(): decode_request() return %d unknown\n", s);
    (void)dsm_return_status_code(transp, DSM_INTERNAL_ERROR);
    return;
  }


  /* get the mutex to make sure no one else is working on this alloc */
  dprintf("remote_write_getter(): trying to lock mutex\n");
  s = pthread_mutex_trylock(&drap->aep->mutex);
  if(s==EBUSY) {
    /* mutex is already locked; you are so rebuffed! */
    dprintf("remote_write_getter(): rebuffed!\n");
    (void)dsm_return_status_code(transp, DSM_WRITE_REBUFFED);
    
    free_decoded_request_args(drap);
    return;
  }
  else if(s!=0) {
    fprintf(stderr,
	    "remote_write_getter(): pthread_mutex_trylock() returned %d\n",
	    s);
    free_decoded_request_args(drap);
    return;
  }

  /* we got the mutex; now we can store the new value */
  dprintf("remote_write_getter(): got mutex; copying data\n");
  for(i=0; i<drap->aep->n_elements; i++) 
    memcpy(drap->aep->elements[i].datap, 
           drap->datapp[i], 
           drap->aep->elements[i].size);

  /* set the timestamp for the new value */
  drap->aep->timestamp = time(NULL);

  /* unlock the mutex, clean up, and return */
  dprintf("remote_write_getter(): unlocking mutex\n");
  s = pthread_mutex_unlock(&drap->aep->mutex);
  if(s!=0) 
    fprintf(stderr,
	    "remote_write_getter(): pthread_mutex_unlock() returns %d\n",
	    s);

  /* do the notification if it's requested */
  dprintf("remote_write_getter(): notify = %d\n", drap->notify);
  if(drap->notify==DSM_TRUE) {
    dprintf("remote_write_getter(): calling dsm_notify_waiting_procs\n");
    dsm_notify_waiting_procs(drap->alhp->machine_name, drap->aep);
  }

  /* send an answer to the other side */
  dprintf("remote_write_getter(): sending success to other end\n");
  (void)dsm_return_status_code(transp, DSM_SUCCESS);

  free_decoded_request_args(drap);

  return;
}

/******************************************************************/
/* Deocde the arguments that come in a dsm_transaction structure. */
/* If successful, makes space for a struct decoded_request_args   */
/* and fills in the values, including allocating a buffer for     */
/* the data.  Returns DSM_SUCCESS if successfuly, otherwise       */
/* returns an error code describing the error (see dsm.h).        */
/*                                                                */
/* allow_class_as_target may be set to either                     */
/* DSM_ALLOW_CLASS_AS_TARGET or DSM_PROHIBIT_CLASS_AS_TARGET      */
/* to indicate whether it's allowed to accept a class name        */
/* as a valid target                                              */
/*                                                                */
/*  IF THE CALL IS SUCCESSFUL, IT IS THE CALLER'S                 */
/*  RESPONSIBILITY TO FREE THE ALLOCATED MEMORY!!!                */
/*  free_decoded_request_args() can be used for this.             */
/******************************************************************/
int decode_request(SVCXPRT *transp, 
		   struct decoded_request_args **drapp,
		   int allow_class_as_target) {
  struct dsm_transaction req;
  int i,j;

  /*********/
  /* Begin */
  /*********/

  /* read the arguments (let xdr filter allocate memory)*/
  req.targetname  = (char                 *)NULL;
  req.allocname   = (char                 *)NULL;
  req.elements    = (struct alloc_element *)NULL;

  dprintf("decode_request(): calling svc_getargs()\n");
  if(!svc_getargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req)) {
    /* couldn't decode the arguments */
    dprintf("decode_request(): svc_getargs() failed\n");

    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
      dprintf("decode_request(): svc_freeargs() failed\n");

    return(DSM_GETARGS);
  }

  /* make some space for the decoded args */
  dprintf("decode_request(): calling malloc(*drapp)\n");
  *drapp = (struct decoded_request_args *)malloc(sizeof(**drapp));
  if(*drapp == (struct decoded_request_args *)NULL) {
    fprintf(stderr, "decode_request(): malloc() failed\n");

    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
      dprintf("decode_request(): svc_freeargs() failed\n");

    return(DSM_INTERNAL_ERROR);
  }
  (*drapp)->csp    = (struct class_share             *)NULL;
  (*drapp)->csaep  = (struct class_share_alloc_entry *)NULL;
  (*drapp)->alhp   = (struct alloc_list_head         *)NULL;
  (*drapp)->aep    = (struct alloc_entry             *)NULL;
  (*drapp)->datapp =                                   NULL;
  (*drapp)->notify =                                   0;


  /* do we share with the requested target as a host? */
  dprintf("decode_request(): calling lookup_machine_name()\n");
  (*drapp)->alhp = lookup_machine_name(req.targetname);

  /* or maybe with a class? */
  if((*drapp)->alhp == (struct alloc_list_head *)NULL
     && 
     allow_class_as_target == DSM_ALLOW_CLASS_AS_TARGET) {
    dprintf("decode_request(): calling lookup_class_name()\n");
    (*drapp)->csp  = lookup_class_name(req.targetname);
  }

  if((*drapp)->alhp == (struct alloc_list_head *)NULL
     &&
     (*drapp)->csp  == (struct class_share     *)NULL) {
    dprintf("decode_request(): target %s invalid\n", req.targetname);
    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
      dprintf("decode_request(): svc_freeargs() failed\n");
    free(*drapp);
    return(DSM_TARGET_INVALID);
  }

  /* is the version okay? */
  dprintf("decode_request(): checking version\n");
  if((*drapp)->alhp != (struct alloc_list_head *)NULL
     &&
     (*drapp)->alhp->version == DSM_VERSION_MISMATCH) {
    dprintf("decode_request(): allocation version mismatch\n");
    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
      dprintf("decode_request(): svc_freeargs() failed\n");
    free(*drapp);
    return(DSM_ALLOC_VERS);
  }

  if((*drapp)->csp != (struct class_share *)NULL) {
    int mismatch=DSM_FALSE;
    for(i=0; i<(*drapp)->csp->class->nmembers; i++) {
      if((*drapp)->csp->alhpp[i]->version == DSM_VERSION_MISMATCH) {
	dprintf("decode_request(): allocation version mismatch for %s\n",
		(*drapp)->csp->alhpp[i]->machine_name);
	mismatch = DSM_TRUE;
      }
    }
    if(mismatch==DSM_TRUE) {
      if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
	dprintf("decode_request(): svc_freeargs() failed\n");
      free(*drapp);
      return(DSM_ALLOC_VERS);
    }
  }

  /* is the allocation a valid one? */
  if((*drapp)->alhp != (struct alloc_list_head *)NULL) {
    dprintf("decode_request(): calling lookup_allocation()\n");
    (*drapp)->aep = lookup_allocation((*drapp)->alhp, req.allocname);
  }
  else {
    /* look it up in the class share alloc list */
    (*drapp)->csaep = 
      lookup_class_share_alloc_entry((*drapp)->csp, req.allocname);
  }

  if( (*drapp)->aep == (struct alloc_entry *)NULL
      &&
      (*drapp)->csaep == (struct class_share_alloc_entry *)NULL ) {
    dprintf("decode_request(): allocation name %s invalid\n",
	    req.allocname);
    if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
      fprintf(stderr, "decode_request(): svc_freeargs() failed\n");
    free(*drapp);
    return(DSM_NAME_INVALID);
  }

  /* everything seems okay; make buffers and copy the data into them */
  if(req.data_size != 0) {
    dprintf("decode_request(): calling malloc((*drapp)->data[] %d bytes)\n",
            req.n_elements * sizeof(void *));
    (*drapp)->datapp = (void **)malloc(req.n_elements * sizeof(void *));
    if((*drapp)->datapp == (void **)NULL) {
      perror("decode_request(): malloc(*datap)");
      if (!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
        fprintf(stderr, "decode_request(): svc_freeargs() failed 1\n");
      free(*drapp);
      return(DSM_INTERNAL_ERROR);
    }

    for(i=0; i<req.n_elements; i++) {
      (*drapp)->datapp[i] = malloc(req.elements[i].size);
      if((*drapp)->datapp[i] == NULL) {
        fprintf(stderr, "decode_request(): malloc(%s) failed\n",
                req.elements[i].name);
        perror("malloc()");
        if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
          fprintf(stderr, "decode_request(): svc_freeargs() failed\n");
        for(j=0; j<i; j++) free((*drapp)->datapp[j]);
	free((*drapp)->datapp);
        free(*drapp);
        return(DSM_INTERNAL_ERROR);
      }

      dprintf("decode_request(): calling memcpy(%s)\n", req.elements[i].name);
      memcpy((*drapp)->datapp[i], req.elements[i].datap, req.elements[i].size);
   }
  }
  else {
    dprintf("decode_request(): data_size=0 so no data buffer allocated\n");
    (*drapp)->datapp = NULL;
  }

  (*drapp)->notify = req.notify;

  dprintf("decode_request(): calling svc_freeargs()\n");
  if(!svc_freeargs(transp, (xdrproc_t)xdr_dsm_transaction, (char *)&req))
    dprintf("decode_request(): svc_freeargs() failed\n");
  
  return(DSM_SUCCESS);
}

/******************************************************************/
/* Duplicate a struct decoded_request_args.                       */
/* If successful, makes space for a struct decoded_request_args   */
/* and fills in the values, including allocating a buffer for     */
/* the data.  Returns DSM_SUCCESS if successfuly, otherwise       */
/* returns an error code describing the error (see dsm.h).        */
/*                                                                */
/*  IF THE CALL IS SUCCESSFUL, IT IS THE CALLER'S                 */
/*  RESPONSIBILITY TO FREE THE ALLOCATED MEMORY!!!                */
/*  free_decoded_request_args() can be used for this.             */
/******************************************************************/
int duplicate_decoded_request_args(struct decoded_request_args **drapp_dest,
                                   struct decoded_request_args *drap_src
                                   ) {
  int i,j;

  /* make some space for the decoded args */
  dprintf("duplicate_decoded_request(): calling malloc(*drapp_dest)\n");
  *drapp_dest = (struct decoded_request_args *)malloc(sizeof(**drapp_dest));
  if(*drapp_dest == (struct decoded_request_args *)NULL) {
    fprintf(stderr, "duplicate_decoded_request(): malloc() failed\n");
    return(DSM_INTERNAL_ERROR);
  }

  dprintf("duplicate_decoded_request(): copying non-datapp fields\n");
  (*drapp_dest)->csp    = drap_src->csp;
  (*drapp_dest)->csaep  = drap_src->csaep;
  (*drapp_dest)->alhp   = drap_src->alhp;
  (*drapp_dest)->aep    = drap_src->aep;
  (*drapp_dest)->notify = drap_src->notify;

  /* make buffers and copy the data into them */
  if(drap_src->datapp != NULL) {
    dprintf("duplicate_decoded_request(): "
            "calling malloc((*drapp_dest)->data[] %d bytes)\n",
            drap_src->aep->n_elements * sizeof(void *));
    (*drapp_dest)->datapp = 
      (void **)malloc(drap_src->aep->n_elements * sizeof(void *));
    if((*drapp_dest)->datapp == (void **)NULL) {
      perror("duplicate_decoded_request(): malloc(*datapp)");
      free(*drapp_dest);
      return(DSM_INTERNAL_ERROR);
    }

    for(i=0; i<drap_src->aep->n_elements; i++) {
      (*drapp_dest)->datapp[i] = malloc(drap_src->aep->elements[i].size);
      if((*drapp_dest)->datapp[i] == NULL) {
        fprintf(stderr, "duplicate_decoded_request(): malloc(%s) failed\n",
                drap_src->aep->elements[i].name);
        perror("malloc()");
        for(j=0; j<i; j++) free((*drapp_dest)->datapp[j]);
        free((*drapp_dest)->datapp);
        free(*drapp_dest);
        return(DSM_INTERNAL_ERROR);
      }

      dprintf("duplicate_decoded_request(): calling memcpy(%s)\n",
              drap_src->aep->elements[i].name);
      memcpy((*drapp_dest)->datapp[i], 
             drap_src->datapp[i],
             drap_src->aep->elements[i].size);
    }
  }
  else {
    dprintf("duplicate_decoded_request(): "
            "data_size=0 so no data buffer allocated\n");
    (*drapp_dest)->datapp = NULL;
  }
  
  return(DSM_SUCCESS);
}

/******************************************/
/* free a decoded_request_args structure  */
/* that was allocated by decode_request() */
/******************************************/
void free_decoded_request_args(struct decoded_request_args *drap) {
  int i;

  if(drap != (struct decoded_request_args *)NULL) {
    if(drap->datapp != NULL) {
      for(i=0; i<drap->aep->n_elements; i++)
        if(drap->datapp[i] != NULL) free(drap->datapp[i]);
      free(drap->datapp);
    }
    free(drap);
  }
  return; 
}
