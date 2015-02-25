/*
** dsm_xdr.c
**
** Contains xdr filters used by the SMA distributed shared memory (dsm)
** system.
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_xdr.c,v 2.4 2012/12/06 18:27:06 ckatz Exp $
*/



#include <rpc/rpc.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

#define _DSM_INTERNAL
#include "dsm.h"
#undef  _DSM_INTERNAL

static char rcsid[] = "$Id: dsm_xdr.c,v 2.4 2012/12/06 18:27:06 ckatz Exp $";

/********************************************************************/
/* filter for encoding/decoding a single element of an alloc_entry; */
/* this is not called by outside functions, just by other filters   */
/* in here; for decoding, memory must already be allocated          */
/********************************************************************/
bool_t xdr_dsm_alloc_element(XDR *xdrs, struct alloc_element *element) {
  size_t element_size;
  int is_array, j, s;

  /* table of elementary xdr filters (indexed by xdr_filter_index) */
  xdrproc_t xdr_filters[] =
    { xdr_void,   /* these two don't actually get used because they are */
      xdr_opaque, /* special cases; they're here for completeness       */
      xdr_char,
      xdr_short,
      xdr_long,
      xdr_float,
      xdr_double };

  /* if there's more than one subelement we have to use an array filter */
  is_array = (element->n_sub_elements > 1) ? 1 : 0;
  element_size = (size_t)(element->size / element->n_sub_elements);

  switch(element->xdr_filter_index) {
  case XDR_VOID:
    /* no data sent; */
    break;

  case XDR_FILTER_OPAQUE:
    /* just bytes */
      if(!xdr_opaque(xdrs, element->datap, element->size)) {
	fprintf(stderr, 
		"xdr_dsm_alloc_element(): xdr_opaque(element %s) failed\n",
		element->name);
	return(FALSE);
      }
      break;

  case XDR_FILTER_CHAR:
  case XDR_FILTER_SHORT:
  case XDR_FILTER_LONG:
  case XDR_FILTER_FLOAT:
  case XDR_FILTER_DOUBLE:
    /* an elementary type */
    if(is_array) s = xdr_vector(xdrs, 
				element->datap,
				element->n_sub_elements,
				element_size, 
				*xdr_filters[element->xdr_filter_index]);
    else
      s =
	(*xdr_filters[element->xdr_filter_index])(xdrs,(char *)element->datap);

    if(!s) {
      fprintf(stderr, 
	      "xdr_dsm_alloc_element(): %sxdr_filter[%d](element %s) failed\n",
	      is_array ? "xdr_vector:" : "",
	      element->xdr_filter_index, element->name);
      return(FALSE);
    }
    break;
    
  case XDR_FILTER_STRING:
    /* a string; since the string filter is not an elementary filter,
       we can't use the xdr_vector() filter; do the strings
       individually instead */
    for(j=0; j<element->n_sub_elements; j++) {
      char *cp = element->datap + j*element_size;
      /*      if(!xdr_string(xdrs, &cp, element_size)) {*/
      /* we're not guaranteed that the string is really a properly
	 null-terminated C string, so we use the xdr_opaque filter.  I
	 don't know of a platform on which C strings are stored in
	 reverse byte-order, so I have some hope that this will work. */
      if(!xdr_opaque(xdrs, cp, element_size)) {
	fprintf(stderr, 
		"xdr_dsm_alloc_element(): xdr_opaque(element %s, subel=%d) "
		"failed\n", element->name, j);
	return(FALSE);
      }
    }
    break;
    
  default:
    /* huh? */
    fprintf(stderr,
	    "xdr_dsm_alloc_element(): internal error - "
	    "xdr_filter_index[element %s]=%d\n",
	    element->name, element->xdr_filter_index);
    return(FALSE);
  } /* switch() on filter index */
  
  return(TRUE);
}


/********************************************************/
/* general purpose filter for sending/receiving various */
/* types of requests and replies; not all request types */
/* use all of these fields                              */ 
/********************************************************/
bool_t xdr_dsm_transaction(XDR *xdrs, struct dsm_transaction *arg) {
  int i;
  char *cp;

  /* the stream contains the following, in this order:
  **   targetname  (used only for requests)
  **   allocname (used only for requests)
  **   data_size
  **   n_elements
  **
  **   element_name; element_total_size; n_sub_elements; filter_index; data
  **   element_name; element_total_size; n_sub_elements; filter_index; data
  **     ... (repeated a total of n_elements times)
  **   
  **   timestamp (used only for replies)
  **   notify (used only for write requests)
  **   status (used only for replies)
  */
       
  if(!xdr_string(xdrs, &arg->targetname, DSM_NAME_LENGTH) ) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_string(targetname) failed\n");
    return(FALSE);
  }

  if(!xdr_string(xdrs, &arg->allocname, DSM_NAME_LENGTH) ) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_string(allocname) failed\n");
    return(FALSE);
  }

  if(!xdr_u_int(xdrs, &arg->data_size) ) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_u_int(data_size) failed\n");
    return(FALSE);
  }
  
  if(!xdr_u_int(xdrs, &arg->n_elements) ) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_u_int(n_elements) failed\n");
    return(FALSE);
  }
 
  if(!xdr_u_int(xdrs, &arg->is_structure) ) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_u_int(is_structure) failed\n");
    return(FALSE);
  }
 

  /* deal with (de)allocating the elements array */
  switch(xdrs->x_op) {
  case XDR_ENCODE:
    /* no action required because we already have an existing data
       structure */
    break;
    
  case XDR_DECODE:
    /* make space for the array of elements; note that we can't allocate
       space for the data yet because we won't know how much space to
       allocate until we start reading the incoming stream */
    if(arg->elements == NULL && arg->n_elements > 0) {
      arg->elements = 
	(struct alloc_element *)malloc(arg->n_elements 
				       * sizeof(*(arg->elements)));
      if(arg->elements == (struct alloc_element *)NULL) {
	perror("xdr_dsm_transaction(): malloc(elements array)");
	return(FALSE);
      }

      for(i=0; i<arg->n_elements; i++) arg->elements[i].datap = NULL;
    }
    break;

  case XDR_FREE:
    if(arg->elements != (struct alloc_element *)NULL) {
      /* free the data buffers */
      for(i=0; i<arg->n_elements; i++) {
	if(arg->elements[i].datap != NULL) {
	  free(arg->elements[i].datap);
	}
      }

      /* free the elements array */
      free(arg->elements);
      arg->elements = NULL;
    }

    /* this function does nothing else when freeing, so we can just
       return */
    return(TRUE);

  default:
    fprintf(stderr, 
	    "xdr_dsm_transaction(): internal error - xdrs->x_op=%d\n",
	    xdrs->x_op); 
    return(FALSE);
  } /* switch statement for (de)allocating memeory for elements */

  /* do the elements */
  for(i=0; i<arg->n_elements; i++) {
    /* get the name of the element */
    cp = arg->elements[i].name;
    if(!xdr_string(xdrs, &cp, DSM_NAME_LENGTH) ) {
      fprintf(stderr,
	      "xdr_dsm_transaction(): xdr_string(element name[%d]) "
	      "failed\n", i);
      return(FALSE);
    }

    /* get the total size of the element */
    if(!xdr_u_int(xdrs, &arg->elements[i].size)) {
      fprintf(stderr,
	      "xdr_dsm_transaction(): xdr_u_int(element_total_size[%d]) "
	      "failed\n", i);
      return(FALSE);
    }

    if(!xdr_u_int(xdrs, &arg->elements[i].n_sub_elements) ) {
      fprintf(stderr, "xdr_dsm_transaction(): xdr_u_int(n_sub_elements[%d]) "
	      "failed\n", i);
      return(FALSE);
    }

    /* which filter to use for this element */
    if(!xdr_int(xdrs, &(arg->elements[i].xdr_filter_index)) ) {
      fprintf(stderr, 
	      "xdr_dsm_transaction(): xdr_int(xdr_filter_index[%d]) failed\n",
	      i);
      return(FALSE);
    }

    /* if decoding, make space for this element */
    if(xdrs->x_op == XDR_DECODE) {
      if(arg->elements[i].datap == NULL) {
	arg->elements[i].datap = malloc(arg->elements[i].size);
	if(arg->elements[i].datap == NULL) {
	  fprintf(stderr, "xdr_dsm_transaction(): malloc(datap %s): %s",
		  arg->elements[i].name, strerror(errno));
	  return(FALSE);
	}

      }
    }

    /* process the element data */
    if(xdr_dsm_alloc_element(xdrs, &arg->elements[i]) != TRUE) { 
      fprintf(stderr, 
	      "xdr_dsm_transaction(): can't process %s\n",
	      arg->elements[i].name);
      return(FALSE); 
    }

  } /* loop over elements */
 

  /* timestamp */
  if(!xdr_u_long(xdrs, &arg->timestamp)) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_u_long(timestamp) failed\n");
    return(FALSE);
  }


  /* notify flag */
  if(!xdr_int(xdrs, &arg->notify) ) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_int() failed\n");
    return(FALSE);
  }

  /* status */
  if(!xdr_int(xdrs, &arg->status)) {
    fprintf(stderr, "xdr_dsm_transaction(): xdr_int(status) failed\n");
    return(FALSE);
  }

  return(TRUE);
}


/***********************************************************************/
/* filter used when establishing a connection to a remote server; used */
/* for transferring the entire set of allocations for one machine;     */ 
/* if decoding, alhp structure must be allocated and initialized, but  */
/* alloc_entry pointer inside it should be NULL; it will be allocated  */
/* by the filter, and must be freed by the user with xdr_free()        */
/***********************************************************************/
bool_t xdr_dsm_alloc_entry_array(XDR *xdrs, 
				 struct alloc_list_head *alhp) {
  struct dsm_transaction tr;
  int i, j, s1, s2;
  unsigned long local_clock, remote_clock;

  local_clock = (unsigned long)time(NULL);

  /* with this filter, the stream contains
  **   system clock timestamp
  **   alhp->nallocs entries containing
  **     result of running xdr_dsm_transaction for a single alloc_entry
   */

  /* if encoding, put local clock time on stream */
  if(xdrs->x_op == XDR_ENCODE) {
    if(!xdr_u_long(xdrs, &local_clock)) {
      fprintf(stderr, 
	      "xdr_dsm_alloc_entry_array(): xdr_u_long() failed "
	      "in encoding\n");
      return(FALSE);
    }
  }

  /* if decoding, get remote clock time from stream, compute time
     offset, get memory for array of alloc_entry */
  if(xdrs->x_op == XDR_DECODE) {
    if(!xdr_u_long(xdrs, &remote_clock)) {
      fprintf(stderr, 
	      "xdr_dsm_alloc_entry_array(): xdr_u_long() failed "
	      "in decoding\n");
      return(FALSE);
    }
    
    /* this is the simplest possible time offset computation we can do;
       since we only have 1-second time resolution, we assume that the
       entire alloc_entry synchronization takes less than 1 second, and
       compare the received timestamp to the current local time */
    alhp->remote_clock_offset = (int)difftime(local_clock, remote_clock);

    /* allocate memory for this alloc_entry list */
    alhp->allocs =
      (struct alloc_entry *)malloc(alhp->nallocs * sizeof(struct alloc_entry));
    if(alhp->allocs == (struct alloc_entry *)NULL) {
      fprintf(stderr, 
	      "xdr_dsm_alloc_entry_array(): malloc(alloc_entry for "
	      "for %s) failed\n", alhp->machine_name);
      perror("malloc()");
      return(FALSE);
    }

  } /* if decoding */

  for(i=0; i<alhp->nallocs; i++) {

    switch(xdrs->x_op) {
      /* E N C O D I N G */
    case XDR_ENCODE:
      /* make sure no one is fooling around with it */
      s1 = pthread_mutex_lock(&alhp->allocs[i].mutex);
      if(s1!=0) {
	fprintf(stderr,
		"xdr_dsm_alloc_entry_array(): "
		"pthread_mutex_lock() returns %d\n",
		s1);
	return(FALSE);
      }

      /* prepare the transaction data, then put it on the stream */
      tr.allocname    = alhp->allocs[i].name;
      tr.targetname   = "dummy";  /* don't need this */
      tr.data_size    = (u_int)alhp->allocs[i].size;
      tr.n_elements   = (u_int)alhp->allocs[i].n_elements;
      tr.is_structure = (u_int)alhp->allocs[i].is_structure;
      tr.elements     = alhp->allocs[i].elements;
      tr.timestamp    = (u_long)alhp->allocs[i].timestamp;
      tr.notify       = 0; /* dummy */
      tr.status       = 0; /* dummy */
      
      /* encode it */
      s1 = xdr_dsm_transaction(xdrs, &tr);

      s2 = pthread_mutex_unlock(&alhp->allocs[i].mutex);
      if(s2!=0) {
	fprintf(stderr,
		"xdr_dsm_alloc_entry_array(): "
		"pthread_mutex_unlock() returns %d\n",
		s2);
	return(FALSE);
      }
    
      if(s1!=TRUE) {
	fprintf(stderr,
		"xdr_dsm_alloc_entry_array(): can't encode %s:%s\n",
		alhp->machine_name, alhp->allocs[i].name);
	return(FALSE);
      }
      break;


      /* D E C O D I N G */
    case XDR_DECODE:
      /* decode the transaction data; we allocate space for targetname
	 and allocname here, because we need to free them here when we're
	 done with the transaction */
      tr.targetname = (char *)malloc(DSM_NAME_LENGTH);
      if(tr.targetname == (char *)NULL) {
	perror("xdr_dsm_alloc_entry_array(): malloc(targetname)");
	return(FALSE);
      }

      tr.allocname  = (char *)malloc(DSM_NAME_LENGTH);
      if(tr.allocname == (char *)NULL) {
	perror("xdr_dsm_alloc_entry_array(): malloc(allocname)");
	return(FALSE);
      }

      /* this one will be allocated by the filter, then later freed
	 by the caller with a call to xdr_free()  */
      tr.elements   = (struct alloc_element *)NULL;

      /* decode it */
      s1 = xdr_dsm_transaction(xdrs, &tr);
      if(s1!=TRUE) {
	fprintf(stderr,
		"xdr_dsm_alloc_entry_array(): can't decode %s:%s\n",
		alhp->machine_name, alhp->allocs[i].name);
	free(alhp->allocs);
	alhp->allocs = (struct alloc_entry *)NULL;
	return(FALSE);
      }
      
      /* copy the data into the caller's data structure */
      strcpy(alhp->allocs[i].name, tr.allocname);
      alhp->allocs[i].size         = (size_t)tr.data_size;
      alhp->allocs[i].n_elements   = (int)tr.n_elements;
      alhp->allocs[i].is_structure = (int)tr.is_structure;
      alhp->allocs[i].elements     = tr.elements;
      alhp->allocs[i].timestamp    = (time_t)tr.timestamp;

      /* and now we are done with the transaction */
      free(tr.targetname);
      free(tr.allocname);
      break;


      /* F R E E I N G */
    case XDR_FREE:
      /* we can't call xdr_dsm_transaction to free, because that deals
	 with dsm_transaction structures, while this
	 dsm_alloc_entry_array filter deals with alloc_list_head
	 structures; free them manually */
      if(alhp->allocs != (struct alloc_entry *)NULL
	 &&
	 alhp->allocs[i].elements != (struct alloc_element *)NULL) {
	for(j=0; j<alhp->allocs[i].n_elements; j++) {
	  if(alhp->allocs[i].elements[j].datap != NULL) {
	    free(alhp->allocs[i].elements[j].datap);
	  }
	}
	free(alhp->allocs[i].elements);
	
      }
      break;

    default:
      fprintf(stderr, 
	      "xdr_dsm_alloc_entry_array(): unknown case %d in switch()\n",
	      xdrs->x_op);
      return(FALSE);

    }
  }

  if(xdrs->x_op == XDR_FREE && alhp->allocs != (struct alloc_entry *)NULL) {
    free(alhp->allocs);
    alhp->allocs = (struct alloc_entry *)NULL;
  }

  return(TRUE);
}


/********************************************************************/
/* filter used for making a read_wait() call; contains two variable */
/* length string arrays which hold the list of host names and       */
/* allocation names to be monitored; also contains the pid and      */
/* thread id of the caller, and some other bookkeeping information  */
/********************************************************************/
bool_t xdr_dsm_read_wait_req(XDR *xdrs, struct dsm_read_wait_req *rwq) {
  int i;
  char *cp;

  if(!xdr_u_int(xdrs, &rwq->pid)) {
    fprintf(stderr, "xdr_dsm_read_wait_req(): xdr_u_int(pid) failed\n");
    return(FALSE);
  }

  if(!xdr_u_int(xdrs, &rwq->tid)) {
    fprintf(stderr, "xdr_dsm_read_wait_req(): xdr_u_int(tid) failed\n");
    return(FALSE);
  }

  if(!xdr_u_int(xdrs, &rwq->num_alloc)) {
    fprintf(stderr, "xdr_dsm_read_wait_req(): xdr_u_int(na) failed\n");
    return(FALSE);
  }

  if(!xdr_u_int(xdrs, &rwq->string_size)) {
    fprintf(stderr, "xdr_dsm_read_wait_req(): xdr_u_int(ss) failed\n");
    return(FALSE);
  }

  switch(xdrs->x_op) {
  case XDR_DECODE:
    rwq->hostnames = (char *)malloc(rwq->num_alloc * rwq->string_size);
    if(rwq->hostnames == (char *)NULL) {
      fprintf(stderr, "xdr_dsm_read_wait_req(): hostnames malloc failed\n");
      return(FALSE);
    }

    rwq->allocnames = (char *)malloc(rwq->num_alloc * rwq->string_size);
    if(rwq->allocnames == (char *)NULL) {
      fprintf(stderr, "xdr_dsm_read_wait_req(): allocnames malloc failed\n");
      free(rwq->hostnames);
      return(FALSE);
    }
    break;

  case XDR_FREE:
    free(rwq->hostnames);
    free(rwq->allocnames);
    break;

  default:
    break;
  }

  if(xdrs->x_op!=XDR_FREE) {
    for(i=0; i<rwq->num_alloc; i++) {
      cp = rwq->hostnames + i*rwq->string_size;
      if(!xdr_string(xdrs, &cp, rwq->string_size)) {
	free(rwq->hostnames);
	free(rwq->allocnames);
	fprintf(stderr, "xdr_dsm_read_wait_req(): xdr_string(hn) failed\n");
	return(FALSE);
      }
      
      cp = rwq->allocnames + i*rwq->string_size;
      if(!xdr_string(xdrs, &cp, rwq->string_size)) {
	free(rwq->hostnames);
	free(rwq->allocnames);
	fprintf(stderr, "xdr_dsm_read_wait_req(): xdr_string(an) failed\n");
	return(FALSE);
      }
      
    }
  }

  return(TRUE);
}


/***************************************/
/* filter used for returning the whole */
/* allocation list to a local client   */
/***************************************/

/* This is one ugly filter; it encodes one data type for sending, and
** decodes into a different data type! This really breaks the spirit of
** the xdr filters which are supposed to be symmetric.  Oh well.
*/

bool_t xdr_dsm_allocation_list(XDR *xdrs, void *p) {

  /* this is horrible, just horrible: nmachines is actually two
     different variables; one in the server and another in the client */
  extern int nmachines;

  int i,j,k;
  char *cp, string[DSM_FULL_NAME_LENGTH];

  /* data structure used for encoding */
  struct alloc_list_head *al = (struct alloc_list_head *)p;
  
  /* data structure used for decoding and freeing */
  struct dsm_allocation_list **dalp = (struct dsm_allocation_list **)p;

  /*********/
  /* Begin */
  /*********/

  /* here's what goes on or comes off the stream:
  **     nmachines (nm)
  **       hostname 1
  **       number of entries for hostname 1 (ne1)
  **         entry 1 (allocname[:elementname])
  **         ...
  **         entry ne1
  **       ...
  **       hostname nm
  **       number of entries for hostname nm (nenm)
  **         ...
  */

  switch(xdrs->x_op) {
    
    /* E N C O D I N G */
  case XDR_ENCODE:
    /* put number of machines on the list */
    if(!xdr_int(xdrs, &nmachines)) {
      fprintf(stderr,
	      "xdr_dsm_allocation_list(enc): xdr_int(nmachines) failed\n");
      return(FALSE);
    }

    /* now loop over the list of machines, putting on the allocation
       list for each */
    for(i=0; i<nmachines; i++) {

      /* machine name */
      cp = al[i].machine_name;
      if(!xdr_string(xdrs, &cp, DSM_NAME_LENGTH)) {
	fprintf(stderr, 
		"xdr_dsm_allocation_list(enc): "
		"xdr_string(machname=%s) failed\n",
		al[i].machine_name);
	return(FALSE);
      }

      /* number of elements */
      if(!xdr_int(xdrs, &(al[i].nnames))) {
	fprintf(stderr,
		"xdr_dsm_allocation_list(enc): "
		"xdr_int(nelements(%s)) failed\n",
		al[i].machine_name);
	return(FALSE);
      }

      for(j=0; j<al[i].nallocs; j++) {
	/* add allocation */

	/* if it's a structure, tack on the element names */
	if(al[i].allocs[j].is_structure == DSM_TRUE) {
	  /* element names */
	  for(k=0; k<al[i].allocs[j].n_elements; k++) {
	    sprintf(string, "%s:%s",
		    al[i].allocs[j].name, al[i].allocs[j].elements[k].name);
	    cp = string;
	    if(!xdr_string(xdrs, &cp, DSM_FULL_NAME_LENGTH)) {
	      fprintf(stderr,
		      "xdr_dsm_allocation_list(enc): xdr_string(%s:%s:%s) "
		      "failed\n",
		      al[i].machine_name, al[i].allocs[j].name,
		      al[i].allocs[j].elements[k].name);
	      return(FALSE);
	    }
	  } /* loop over elements of allocation */
	}
	else {
	  /* not a structure; just stick the allocation name in */
	  strcpy(string, al[i].allocs[j].name);
	  cp = string;
	  if(!xdr_string(xdrs, &cp, DSM_FULL_NAME_LENGTH)) {
	      fprintf(stderr,
		      "xdr_dsm_allocation_list(enc): xdr_string(%s:%s) "
		      "failed\n",
		      al[i].machine_name, al[i].allocs[j].name);
	      return(FALSE);
	  }
	}

      } /* loop over allocations of machine */
    } /* loop over machines */
   
    break;


    /* D E C O D I N G */
  case XDR_DECODE:
    /* the decoder removes the items from the stream and puts them into
       an array of dsm_allocation_list structures */

    *dalp = (struct dsm_allocation_list *)NULL;
    
    /* first find out how many there are */
    if(!xdr_int(xdrs, &nmachines)) {
      fprintf(stderr,
	      "xdr_dsm_allocation_list(dec): xdr_int(nmachines) failed\n");
      return(FALSE);
    }

    /* now make space for the allocation list (make sure this is freed
       before doing this, or there will be a memory leak) */
    *dalp = (struct dsm_allocation_list *)
      malloc(nmachines*sizeof(struct dsm_allocation_list));
    if(*dalp == (struct dsm_allocation_list *)NULL) {
      perror("xdr_dsm_allocation_list(dec): malloc(dal)");
      return(FALSE);
    }

    /* initialize array */
    for(i=0; i<nmachines; i++) {
      (*dalp)[i].host_name[0] = '\0';
      (*dalp)[i].n_entries    = 0;
      (*dalp)[i].alloc_list   = (char **)NULL;
    }
    
    /* loop, reading info for each machine */
    for(i=0; i<nmachines; i++) {
      
      /* get machine name */
      cp = (*dalp)[i].host_name;
      if(!xdr_string(xdrs, &cp, DSM_NAME_LENGTH)) {
	fprintf(stderr,
		"xdr_dsm_allocation_list(dec): "
		"xdr_string(hostname %d) failed\n", i);
	xdr_free((xdrproc_t)xdr_dsm_allocation_list, (char *)dalp);
	return(FALSE);
      }

      /* how many entries for this machine? */
      if(!xdr_int(xdrs, &((*dalp)[i].n_entries))) {
	fprintf(stderr,
		"xdr_dsm_allocation_list(dec): xdr_int(nentries(%s)) failed\n",
		(*dalp)[i].host_name);
	xdr_free((xdrproc_t)xdr_dsm_allocation_list, (char *)dalp);
	return(FALSE);
      }
      
      /* make space for the allocation list */
      (*dalp)[i].alloc_list = 
	(char **)malloc((*dalp)[i].n_entries * sizeof(char *));
      if((*dalp)[i].alloc_list == (char **)NULL) {
	perror("xdr_dsm_allocation_list(dec): malloc(alloc_list)");
	xdr_free((xdrproc_t)xdr_dsm_allocation_list, (char *)dalp);
	return(FALSE);
      }

      for(j=0; j<(*dalp)[i].n_entries; j++) 
	(*dalp)[i].alloc_list[j] = (char *)NULL;

      for(j=0; j<(*dalp)[i].n_entries; j++) {
	(*dalp)[i].alloc_list[j] = (char *)malloc(DSM_FULL_NAME_LENGTH);
	if((*dalp)[i].alloc_list[j] == (char *)NULL) {
	  perror("xdr_dsm_allocation_list(dec): malloc(alloc)");
	  xdr_free((xdrproc_t)xdr_dsm_allocation_list, (char *)dalp);
	  return(FALSE);
	}


	/* get allocation name off stream */
	if(!xdr_string(xdrs, 
		       &((*dalp)[i].alloc_list[j]),
		       DSM_FULL_NAME_LENGTH)) {
	  fprintf(stderr,
		  "xdr_dsm_allocation_list(dec): xdr_string(%s:%d) failed\n",
		  (*dalp)[i].host_name, j);
	  xdr_free((xdrproc_t)xdr_dsm_allocation_list, (char *)dalp);
	  return(FALSE);
	}
      } /* for loop over allocation names for one machine */

    } /* for loop over machines */

    break;

    
    /* F R E E I N G */
  case XDR_FREE:
    if((*dalp) != (struct dsm_allocation_list *)NULL) {

      for(i=0; i<nmachines; i++) {

	if((*dalp)[i].alloc_list != (char **)NULL) {

	  /* free individual alloc name spaces */
	  for(j=0; j<(*dalp)[i].n_entries; j++) {
	    if((*dalp)[i].alloc_list[j] != (char *)NULL) {
	      free((*dalp)[i].alloc_list[j]);
	    }
	  }
     
	  /* free pointer to array of alloc name spaces */
	  free((*dalp)[i].alloc_list);
	}

      }
      
      /* free top level */
      free(*dalp);
      *dalp = (struct dsm_allocation_list *)NULL;
    }      
    break;
  }

  return(TRUE);
}
