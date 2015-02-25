/*
** dsm_dispatcher.c
**
** Contains the RPC dispatcher function for SMA distributed shared
** memory (dsm).  This is the function which gets registered with the
** portmapper to handle incoming requests.
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_dispatcher.c,v 2.6 2012/12/05 20:08:30 ckatz Exp $
*/



#include <stdio.h>
#include <stdlib.h>
#include <rpc/rpc.h>
#include <unistd.h>


#ifdef __Lynx__
#  include <socket.h>
#  include <net/tcp.h>
#  include <info.h>
#  include "smadaemon.h"
#endif

#ifdef __linux__
#  include <sys/socket.h>
#  include <netinet/tcp.h>
#  define SYSERR_RTN 0
#endif


SVCXPRT *svcfd_create(int, u_int, u_int);

/*#define TIMING 1*/
#undef TIMING
#ifdef TIMING
#include <time.h>
#endif

#define _DSM_INTERNAL
#include "dsm.h"
#undef  _DSM_INTERNAL


static char rcsid[] = "$Id: dsm_dispatcher.c,v 2.6 2012/12/05 20:08:30 ckatz Exp $";

#ifdef TIMING
static struct timeval start,stop;
static struct timezone dummy;

void start_timer(void) {
  gettimeofday(&start,&dummy);
}

void stop_timer(char *func) {
  gettimeofday(&stop, &dummy);
  dprintf("%s: %d us\n",
	  func,
	  stop.tv_usec-start.tv_usec 
	  + 1000000*(stop.tv_sec - start.tv_sec) );
}
#endif


void dsm_dispatcher(struct svc_req *rqstp,  SVCXPRT *transp) {
  u_long address;
  struct sockaddr_in *addr;

  struct alloc_list_head *reqhostp;
  int local_call;

  /* this stuff is all for cleaning out stale sockets */
  static long usr_nfds;
  static u_long *sock_addr_list=(u_long *)NULL;
  SVCXPRT *temp_transp;
  extern fd_set svc_fdset;
  fd_set local_fdset;

  int i,s;

  /* first time through, make a table so we can keep track of our socket
     descriptors; the table is indexed by descriptor, and each entry is
     the address of the remote host which has sent a request through
     that socket (zero means no host associated with that descriptor) */
  if(sock_addr_list==(u_long *)NULL) {
    usr_nfds = 
#ifdef __Lynx__
      info(_I_USR_NFDS);
#elif defined __linux__
      getdtablesize();
#else
  #error "Do not know how to find fd table size on this platform"
#endif

    sock_addr_list = (u_long *)malloc(sizeof(u_long)*usr_nfds);
    if(sock_addr_list == (u_long *)NULL) {
      perror("malloc(sock_addr_list)");
      exit(SYSERR_RTN);
    }

    /* initialize it all to zero */
    for(i=0; i<usr_nfds; i++) sock_addr_list[i]=0;
  }      

#ifdef DSM_NONAGLE
  /* disable Nagle algorithm on the socket */
  i = 1;
  s = setsockopt(transp->xp_sock,
		 IPPROTO_TCP, 
		 TCP_NODELAY, 
		 (char *)&i,
		 sizeof(int));
  if(s < 0) perror("dsm_dispatcher: setsockopt");
#endif

  /* first find out who's calling and whether we're allowed to do
     business with that host */
  addr    = svc_getcaller(transp);
  address = ntohl(addr->sin_addr.s_addr);

  /* is it a local connection? */
  if(address == 0x7f000001) {
    /* yep, say so and don't look it up */
    dprintf("dsm_dispatcher(): (%d) received local "
	    "request--------------------\n",
	    time(NULL));
    local_call = DSM_TRUE;
  }
  else {
    local_call = DSM_FALSE;

    
    /* check to see whether this socket has been used to receive
       messages from a different host */
    if(sock_addr_list[transp->xp_sock]!=address) {
      /* this is a new host for this socket */
      sock_addr_list[transp->xp_sock] = address;

      /* now go through our list to find out we used to receive requests
	 from this host on a different socket */
      for(i=0; i<usr_nfds; i++) {

	/* skip the current socket */
	if(i==transp->xp_sock) continue;

	if(sock_addr_list[i]==address) {
	  /* this address appears elsewhere in our socket list */

	  /* check whether it's still active (i.e. monitored by
	     svc_run()) */ 
	  local_fdset = svc_fdset;
	  if(FD_ISSET(i, &local_fdset)) {
	    /* the old socket is still being used by RPC, but now we
	       know it's stale; clear it out */

	    /* make a temporary transport handle for this socket so we
	       can remove it from RPC use */
	    temp_transp = svcfd_create(i, 0, 0);
	    if(temp_transp==(SVCXPRT *)NULL) {
	      fprintf(stderr, 
		      "dsm_dispatcher(): svcfd_create() returned NULL\n");
	    }
	    else {
	      dprintf("dsm_dispatcher(): Clearing stale socket %d for "
		      "host %lu.%lu.%lu.%lu (now sd %d)\n",
		      i,
		      address >> 24 & 0xFF,
		      address >> 16 & 0xFF,
		      address >>  8 & 0xFF,
		      address >>  0 & 0xFF,
		      transp->xp_sock);
	      xprt_unregister(temp_transp);
	      svc_destroy(temp_transp);
	      
	      /* let's hope this doesn't cause a memory leak: we create
		 a SVCXPRT then destroy it, but we don't know for sure
		 whether the RPC system is also maintaining one
		 internally for this connection; let's hope not;
	      
	         NOTE added Jan 2005: it seems there is not.  There has
		 not been a memory leak associated with this code.  It
		 appears to be functioning as intended.
	      */
	    } 
	  } /* if socket is stale */

	  /* now mark it unused */
	  sock_addr_list[i] = 0;

	} /* if we found the current requester's address associated
	     with another socket */
      } /* for loop over all socket descriptors to see whether
	   requester's address is associated with another socket */
    } /* if socket on which we received request has already been used
	 for requests from this host */
    

    /* find the caller in the allocation list */
    reqhostp = lookup_machine_addr(address);

    if(reqhostp == (struct alloc_list_head *)NULL 
       && rqstp->rq_proc != NULLPROC) {
      /* couldn't find an entry for the caller */
      dprintf("dsm_dispatcher(): (%d) No alloc for host %lu.%lu.%lu.%lu\n",
	      time(NULL),
	      address >> 24 & 0xFF,
	      address >> 16 & 0xFF,
	      address >>  8 & 0xFF,
	      address >>  0 & 0xFF );
      svcerr_systemerr(transp);
      return;
    }
    else {
      dprintf("dsm_dispatcher(): (%d) received req from "
	      "host %lu.%lu.%lu.%lu------------\n",
	      time(NULL),
	      address >> 24         ,
	      (address >> 16) & 0xFF,
	      (address >>  8) & 0xFF,
	      address        & 0xFF);
    }

  }

  /***********************/
  /* service the request */
  /***********************/
  switch (rqstp->rq_proc) {

  case NULLPROC:
#ifdef TIMING
    start_timer();
#endif
    (void)svc_sendreply(transp, (xdrproc_t)xdr_void, (char *)NULL);
#ifdef TIMING
    stop_timer("NULLPROC");
#endif
    dprintf("NULLPROC request from host %lu.%lu.%lu.%lu\n",
 	       address >> 24        ,
	      (address >> 16) & 0xFF,
	      (address >>  8) & 0xFF,
	       address        & 0xFF);

    break;
  

  case DSM_ALLOC_VERSION_CHECK:
#ifdef TIMING
    start_timer();
#endif
    allocation_version_check(transp, reqhostp);
#ifdef TIMING
    stop_timer("ALLOC_VERSION_CHECK");
#endif
    break;



  case DSM_ALLOC_SYNC:
#ifdef TIMING
    start_timer();
#endif
    allocation_sync_reply(transp, reqhostp);
#ifdef TIMING
    stop_timer("ALLOC_SYNC");
#endif
    break;



  case DSM_ALLOC_INFO_QUERY:
  case DSM_LOCAL_READ:
  case DSM_LOCAL_READ_WAIT:
    if(local_call == DSM_FALSE) {
      /* a remote machine is trying to read! make it stop! */
      if(rqstp->rq_proc == DSM_ALLOC_INFO_QUERY)
	dprintf("DSM_ALLOC_INFO_QUERY: request from non-local client\n");
      
      if(rqstp->rq_proc == DSM_LOCAL_READ)
	dprintf("DSM_LOCAL_READ: request from non-local client\n");

      if(rqstp->rq_proc == DSM_LOCAL_READ_WAIT)
	dprintf("DSM_LOCAL_READ_WAIT: request from non-local client\n");

      (void)dsm_return_status_code(transp, DSM_LOCAL_SERVICE);
    }
    else {
      if(rqstp->rq_proc == DSM_LOCAL_READ_WAIT) {
#ifdef TIMING
    start_timer();
#endif
	local_read_wait(transp);
#ifdef TIMING
    stop_timer("LOCAL_READ_WAIT");
#endif
      }
      else {
#ifdef TIMING
    start_timer();
#endif
	local_query(transp, rqstp->rq_proc);
#ifdef TIMING
	if(rqstp->rq_proc==DSM_ALLOC_INFO_QUERY) 
	  stop_timer("ALLOC_INFO_QUERY");
	else
	  stop_timer("LOCAL_READ");
#endif
      }
    }

    break;

  case DSM_LOCAL_WRITE:
    if(local_call == DSM_FALSE) {
      /* a remote machine is trying to write */
      dprintf("DSM_LOCAL_WRITE: request from non-local client denied\n");

      (void)dsm_return_status_code(transp, DSM_LOCAL_SERVICE);
    }
    else {
#ifdef TIMING
    start_timer();
#endif
      local_write(transp, reqhostp);
#ifdef TIMING
    stop_timer("LOCAL_WRITE");
#endif
    }
    break;
    
  case DSM_REMOTE_WRITE:
    if(local_call == DSM_TRUE) {
      /* a local machine is trying to do a remote call */
      dprintf("DSM_REMOTE_WRITE: request from local client denied\n");
      
      (void)dsm_return_status_code(transp, DSM_REMOTE_SERVICE);
    }
    else {
#ifdef TIMING
    start_timer();
#endif
      remote_write_getter(transp);
#ifdef TIMING
    stop_timer("REMOTE_WRITE");
#endif
    }
    break;
    
  case DSM_LOCAL_ALLOC_LIST_REQ:
    if(local_call == DSM_FALSE) {
      /* a remote machine is asking for an allocation list */
      dprintf("DSM_LOCAL_ALLOC_LIST_REQ: "
	      "request from non-local client denied\n");
      (void)dsm_return_status_code(transp, DSM_LOCAL_SERVICE);
    }
    else {
#ifdef TIMING
      start_timer();
#endif
      local_alloc_list_request(transp);
#ifdef TIMING
      stop_timer("LOCAL_ALLOC_LIST_REQ");
#endif
    }
    break;

  
  default:
    svcerr_noproc(transp);
    break;
  } /* switch() on procedure number */

  return;
}
