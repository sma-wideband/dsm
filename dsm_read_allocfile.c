/*
** dsm_read_allocfile.c
**
** Provides dsm_read_allocation_file(), which reads the binary format
** allocation file produced by the dsm_allocator script for SMA
** distributed shared memory (dsm), and puts the data into the dsm
** internal data structures.  This is kept in a separate file because
** some of the test programs need to call it, although normally a user
** application does not.
**
** Also provides dsm_determine_network_info() which was added to account
** for hosts which have multiple IP addresses. (Jan 2013)
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm_read_allocfile.c,v 2.8 2013/03/01 21:01:40 ckatz Exp $
*/



#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#ifdef __Lynx__
#  include <socket.h>
#  include <netdb.h>
#  include "smadaemon.h"
#endif

#ifdef __linux__
#  include <ifaddrs.h>
#endif


#define _DSM_INTERNAL
#include "dsm.h"
#undef _DSM_INTERNAL

static char rcsid[] = "$Id: dsm_read_allocfile.c,v 2.8 2013/03/01 21:01:40 ckatz Exp $";


/* given an open file pointed to by f, positioned at the beginning of an
   individual allocation, read the name (length l), size, number of
   elements, and type */
int read_next_alloc(FILE *f, 
		    char *name, u_long l,
		    u_long *sz,
		    u_long *n_el,
		    char *t) {
  int s;

  s = fread(name, l, 1, f);
  if(s!=1) {
    fprintf(stderr, "Error reading allocation name\n");
    fclose(f);
    return(DSM_ERROR);
  }
	  
  s = fread(sz, 4, 1, f);
  if(s!=1) {
    fprintf(stderr, "Error reading allocation size\n");
    fclose(f);
    return(DSM_ERROR);
  }

  s = fread(n_el, 4, 1, f);
  if(s!=1) {
    fprintf(stderr, "Error reading allocation element count\n");
    fclose(f);
    return(DSM_ERROR);
  }

  s = fread(t, 1, 1, f);
  if(s!=1) {
    fprintf(stderr, "Error reading allocation type\n");
    fclose(f);
    return(DSM_ERROR);
  }
  
  return(DSM_SUCCESS);
}


/* translate a type code to an XDR filter index; return -1 if error */
int filter_index(char type) {
  switch(type) {
  case 'b': return(XDR_FILTER_CHAR);
  case 's': return(XDR_FILTER_SHORT);
  case 'l': return(XDR_FILTER_LONG);
  case 'f': return(XDR_FILTER_FLOAT);
  case 'd': return(XDR_FILTER_DOUBLE);
  case 'c': return(XDR_FILTER_STRING);
  default:  
    fprintf(stderr, "Error: type %c unknown\n", type);
    return(-1);
  }
}


/**************************************************/
/* determine what our hostname and ip address are */
/* is, taking into account that there might be    */
/* multiple network interfaces in a computer      */
/**************************************************/
int dsm_determine_network_info(u_long **my_addresses,
			       int     *my_num_addresses,
			       char    *myhostname) {

  int s,i;
  char *cp;
  u_long this_address;

#ifdef __Lynx__
  struct hostent *hep;
  extern u_long my_address;
#endif

#ifdef __linux__
  struct ifaddrs *ifaddresses, *ifap;
#endif

  /* what are my host name and address? */
  s = gethostname(myhostname, DSM_NAME_LENGTH);
  if(s!=0) {
    perror("Can't get my host name");
    return(DSM_ERROR);
  }

  /* misconfigured (?) hosts can return the FQDN from gethostname(),
     which confuses us, so fix it here */
  cp = strstr(myhostname,".");
  if(cp != (char *)NULL) {
    dprintf("gethostname() returns %s;",myhostname);
    *cp= '\0';
    dprintf(" using %s instead\n", myhostname);
  }

#ifdef __Lynx__
  hep = gethostbyname(myhostname);
  if(hep == (struct hostent *)NULL) {
    fprintf(stderr, "gethostbyname() failed\n");
    return(DSM_ERROR);
  }

  if(hep->h_length != 4) {
    fprintf(stderr,
	    "gethostbyname() returned address of length %d bytes\n",
	    hep->h_length);
    exit(-1);
  }

  *my_addresses = (u_long *)malloc(sizeof(u_long));
  if(*my_addresses == (u_long *)NULL) {
    perror("Can't malloc my_addresses for LynxOS");
    return(DSM_ERROR);
  }
  *my_num_addresses = 1;
  (*my_addresses)[0] = ntohl(*( (u_long *)(hep->h_addr)));
  my_address = *my_addresses[0];
#endif

#ifdef __linux__
  /* Under Linux, account for the possibility of multiple network
     interfaces.  This is theoretically possible in LynxOS, too, but we
     expect we'll never do that so we'll continue using gethostbyname()
     for that case. */
  if(getifaddrs(&ifaddresses) == -1) {
    perror("getifaddrs");
    return(DSM_ERROR);
  }
  
  *my_num_addresses = 0;
  *my_addresses = (u_long *)NULL;
  for(ifap=ifaddresses; ifap!=(struct ifaddrs *)NULL; ifap=ifap->ifa_next) {
    /* INET addresses only */
    if(ifap->ifa_addr->sa_family != AF_INET) continue;

    this_address =
      htonl((u_long)(((struct sockaddr_in *)ifap->ifa_addr)->sin_addr.s_addr));

    /* dsm doesn't use localhost */
    if(this_address == 0x7f000001) continue;

    (*my_num_addresses)++;
    *my_addresses =
      (u_long *)realloc(*my_addresses, *my_num_addresses*sizeof(u_long));
    if(*my_addresses == (u_long *)NULL) {
      perror("realloc");
      return(DSM_ERROR);
    }
    (*my_addresses)[*my_num_addresses-1] = this_address;
  }

  freeifaddrs(ifaddresses);
#endif

  dprintf("my hostname is %s, with %d address%s:\n",
	  myhostname,
	  *my_num_addresses,
	  *my_num_addresses==1 ? "" : "es");
  for(i=0; i<*my_num_addresses; i++) {
    this_address = htonl((*my_addresses)[i]);
    dprintf("  0x%08lx (%u.%u.%u.%u)\n",
	    (*my_addresses)[i],
	    *((unsigned char *)(&this_address)  ),
	    *((unsigned char *)(&this_address)+1),
	    *((unsigned char *)(&this_address)+2),
	    *((unsigned char *)(&this_address)+3)
    	    );
  }
  
  return(DSM_SUCCESS);
}


/*************************************************
 * determine where to look for the allocation file,
 * trying these:
 *
 * 1. name specified with -f switch
 * 2. name specified in environment variable named by
 *    DSM_ALLOC_FILE_ENV_NAME 
 * 3. DSM_DEFAULT_ALLOC_FILE
 *
 * result will be put in *filename; unless it is already non-null, in
 * which case it's not changed
 */
void dsm_determine_alloc_file_location(char *filename) {
  char *cp;

  if(filename[0] == '\0') {
    /* no command line option specified; is there an env variable? */
    cp = getenv(DSM_ALLOC_FILE_ENV_NAME);
    if(cp != (char *)NULL) {
      strcpy(filename, cp);
      dprintf("Alloc file path specified in env(%s) as %s\n",
	      DSM_ALLOC_FILE_ENV_NAME, filename);
    }
    else {
      strcpy(filename, DSM_DEFAULT_ALLOC_FILE);
      dprintf("Alloc file path not specified; using default %s\n",
	      filename);
    }
  }
  else {
    dprintf("Alloc file path specified with -f %s\n", filename);
  }
}


int dsm_read_allocation_file(struct alloc_list_head **alhp, u_long *nmachp,
			     struct class_entry **clp,      u_long *nclassesp,
			     struct class_share **csp,   u_long *nclasssharesp,
			     u_long *myversion,
			     u_long local_addresses[],
			     int    num_local_addresses,
			     u_long *my_locally_used_address,
			     char   *filename) {
  extern int verbose;

  char **my_classes;

  char class1[DSM_NAME_LENGTH], class2[DSM_NAME_LENGTH],
       string[DSM_NAME_LENGTH], allocname[DSM_NAME_LENGTH];

  FILE *fp;
  fpos_t shareallocs;

  struct alloc_entry *new_alloc;

  u_long maxstringsize,nshares,nallocs,size,n_elements,dummy;
  u_long *my_addresses_in_allocfile;

  char type;
  int myclasscount, machinecount;
  int inclass1,inclass2,foundone;
  int thisclass,thisshare,thismachine;
  int added_to_share_list;
  int i, j, k, l, m, n, s;

  /****************************/
  /* Read the allocation file */
  /****************************/
  fp = fopen(filename, "rb");
  if(fp == (FILE *)NULL) {
    fprintf(stderr, "Can't open allocation file %s\n", filename);
    perror("fopen()");
    return(DSM_ERROR);
  }

  dprintf("Reading allocation from %s\n", filename);


  /*
  ** read version number and make sure byte order is okay 
  */
  s = fread(myversion, 4, 1, fp);
  if(s!=1) {
    fprintf(stderr, "Error reading allocation version from %s\n", filename);
    fclose(fp);
    return(DSM_ERROR);
  }
  *myversion = ntohl(*myversion);
  dprintf("Read my allocation version=0x%x\n", *myversion);

  /*
  ** read max string size and make sure byte order is okay 
  */
  s = fread(&maxstringsize, 4, 1, fp);
  if(s!=1) {
    fprintf(stderr, "Error reading max string size from %s\n", filename);
    fclose(fp);
    return(DSM_ERROR);
  }
  maxstringsize = ntohl(maxstringsize);
  dprintf("Read maxstringsize=%d\n", maxstringsize);

  /*
  ** read number of classes and make sure byte order is okay 
  */
  s = fread(nclassesp, 4, 1, fp);
  if(s!=1) {
    fprintf(stderr, "Error reading number of classes from %s\n", filename);
    fclose(fp);
    return(DSM_ERROR);
  }
  *nclassesp = ntohl(*nclassesp);
  dprintf("Read nclasses=%d\n", *nclassesp);

  /*
  ** read number of shares and make sure byte order is okay 
  */
  s = fread(&nshares, 4, 1, fp);
  if(s!=1) {
    fprintf(stderr, "Error reading number of shares from %s\n", filename);
    fclose(fp);
    return(DSM_ERROR);
  }
  nshares = ntohl(nshares);
  dprintf("Read nshares=%d\n", nshares);


  /*
  ** get class list 
  */
  *clp = (struct class_entry *)malloc(*nclassesp*sizeof(struct class_entry));

  /* my_classes is an array of strings; each non-null entry will be the
     name of a class of which I am a member */
  my_classes = (char **)malloc(*nclassesp*sizeof(char *));
  myclasscount = 0;

  /* This is to keep track of which of this machine's IP addresses
     appear in the allocation file.  Since DSM was designed for a
     network on which each host has one IP address, we don't allow DSM
     to share on multiple IP addresses from the same host.  This count
     is how we check that condition. */
  my_addresses_in_allocfile = (u_long *)calloc(num_local_addresses,sizeof(int));
  if(my_addresses_in_allocfile == (u_long *)NULL) {
    perror("calloc failed");
    return(DSM_ERROR);
  }

  for(i=0; i<*nclassesp; i++) {
    my_classes[i] = (char *)malloc(DSM_NAME_LENGTH);
    my_classes[i][0] = '\0';

    s = fread(string, maxstringsize, 1, fp);
    if(s!=1) {
      fprintf(stderr, "Error reading class name from %s\n", filename);
      fclose(fp);
      return(DSM_ERROR);
    }
    
    s = fread(&machinecount, 4, 1, fp);
    if(s!=1) {
      fprintf(stderr,
	      "Error reading number of machines in class %s from %s\n",
	      string, filename);
      fclose(fp);
      return(DSM_ERROR);
    }
    machinecount = ntohl(machinecount);

    /* copy the class name and number of machines into the list */
    strcpy((*clp)[i].class_name, string);
    (*clp)[i].nmembers = machinecount;

    dprintf("Reading class %s with %u member%s\n",
	    (*clp)[i].class_name, 
	    (*clp)[i].nmembers,
	    (*clp)[i].nmembers==1 ? "" : "s");

    /* read in the machine names for this class */
    (*clp)[i].machinelist = 
      (struct class_member *)malloc(machinecount*sizeof(struct class_member));

    for(j=0; j<machinecount; j++) {

      s = fread((*clp)[i].machinelist[j].machine_name, maxstringsize, 1, fp);
      if(s!=1) {
	fprintf(stderr, "Error reading machine name from %s\n", filename);
	fclose(fp);
	return(DSM_ERROR);
      }

      s = fread(&((*clp)[i].machinelist[j].machine_addr), 4, 1, fp);
      if(s!=1) {
	fprintf(stderr,	"Error reading machine address from %s\n", filename);
	fclose(fp);
	return(DSM_ERROR);
      }
      
      sprintf((*clp)[i].machinelist[j].machine_addr_string,
	      "%u.%u.%u.%u",
	      ((unsigned char *)(&((*clp)[i].machinelist[j].machine_addr)))[0],
	      ((unsigned char *)(&((*clp)[i].machinelist[j].machine_addr)))[1],
	      ((unsigned char *)(&((*clp)[i].machinelist[j].machine_addr)))[2],
	      ((unsigned char *)(&((*clp)[i].machinelist[j].machine_addr)))[3]
	      );

      /* we'll keep all addresses internally in host byte order */
      (*clp)[i].machinelist[j].machine_addr = 
	ntohl((*clp)[i].machinelist[j].machine_addr);

      dprintf("  Read machine %s with address %s (0x%x)\n",
	      (*clp)[i].machinelist[j].machine_name,
	      (*clp)[i].machinelist[j].machine_addr_string,
	      (*clp)[i].machinelist[j].machine_addr);
      
      /* if this machine name refers to me, add this class to the list
	 of classes in which I'm a member */
      for(k=0; k<num_local_addresses; k++) {
	if( (*clp)[i].machinelist[j].machine_addr == local_addresses[k]) {
	  strcpy(my_classes[myclasscount++], (*clp)[i].class_name);
	  my_addresses_in_allocfile[k] = 1;
	}
      }

      
    } /* for loop over machine list */

  } /* for loop over class list */
  
  /* make sure we're only using one IP address refer to ourselves */
  k = 0;
  for(i=0; i<num_local_addresses; i++) k += my_addresses_in_allocfile[i];
  if(k>1) {
    fprintf(stderr,
	    "Allocation refers to this host through multiple IP addresses:\n"
	    );
    for(i=0; i<num_local_addresses; i++) {
      if(my_addresses_in_allocfile[i]) {
	fprintf(stderr,
		"  %lu.%lu.%lu.%lu\n",
		(local_addresses[i] >> 24) & 0xFF,
		(local_addresses[i] >> 16) & 0xFF,
		(local_addresses[i] >>  8) & 0xFF,
		(local_addresses[i]      ) & 0xFF);
      }
      fprintf(stderr, "This is prohibited. Please fix allocation file.\n");
    }
    return(DSM_ERROR);
  }

  /* which address do we use for ourselves? */
  for(i=0; i<num_local_addresses; i++) {
    if(my_addresses_in_allocfile[i]) *my_locally_used_address=local_addresses[i];
  }
  dprintf("Using my local address %u.%u.%u.%u\n",
		(*my_locally_used_address >> 24) & 0xFF,
		(*my_locally_used_address >> 16) & 0xFF,
		(*my_locally_used_address >>  8) & 0xFF,
		(*my_locally_used_address      ) & 0xFF);



  for(i=0; i<myclasscount; i++) 
    dprintf("I am a member of class %s\n", my_classes[i]);
  
  /*
  ** Now we have the list of classes and all the machines in them.
  ** Next, read the share list, keeping only those in which the machine
  ** we're running on is a member.
  */
  *nmachp       = 0;
  *nclasssharesp = 0;

  for(i=0; i<nshares; i++) {
    s = fread(class1, maxstringsize, 1, fp);
    if(s!=1) {
      fprintf(stderr, "Error reading class 1 from share %d\n", i+1);
      fclose(fp);
      return(DSM_ERROR);
    }

    s = fread(class2, maxstringsize, 1, fp);
    if(s!=1) {
      fprintf(stderr, "Error reading class 2 from share %d\n", i+1);
      fclose(fp);
      return(DSM_ERROR);
    }
    s = fread(&nallocs, 4, 1, fp);
    if(s!=1) {
      fprintf(stderr,
	      "Error reading number of allocations in share %d\n",
	      i);
      fclose(fp);
      return(DSM_ERROR);
    }
    nallocs = ntohl(nallocs);

    /* remember where we the allocations for this share start */
    s = fgetpos(fp, &shareallocs);
    if(s!=0) {
      fprintf(stderr, "Can't get file position with fgetpos()\n");
      fclose(fp);
      return(DSM_ERROR);
    }

    /* are we involved in this share? */
    inclass1 = DSM_FALSE;
    inclass2 = DSM_FALSE;
    for(j=0; j<myclasscount; j++) {
      if(!strcmp(class1, my_classes[j])) inclass1=DSM_TRUE;
      if(!strcmp(class2, my_classes[j])) inclass2=DSM_TRUE;
    }
    
    if(inclass1==DSM_FALSE && inclass2==DSM_FALSE) {
      dprintf("Share %s-%s does not involve me; skipping it\n",
	      class1, class2);

      /* skip the allocations for this share */
      for(j=0; j<nallocs; j++) {
	s = read_next_alloc(fp, 
			    allocname, maxstringsize,
 			    &size, &n_elements, &type);
	if(s!=DSM_SUCCESS) {
	  fprintf(stderr,
		  "read_allocfile(): skipping alloc failed\n");
	}
	if(type == 'x') {
	  for(k=0; k<ntohl(n_elements); k++) {
	    s = read_next_alloc(fp, 
				allocname, maxstringsize,
				&dummy, &dummy, &type);
	    if(s!=DSM_SUCCESS) {
	      fprintf(stderr,
		      "read_allocfile()2: skipping structure elements"
		      " failed\n");
	    }
	  }
	}
      }    

      /* go on to the next share */
      continue;
    }

    /* for each of the machines I'm sharing with, add an entry to the
       allocation head list, and create the allocation list itself */
    for(l=1; l<=2; l++) {
      thisclass = -1;

      /* first see if we're in class 1 */
      if(l==1 && inclass1==DSM_TRUE) {
	/* if we are in class 1, we need a space for each of the machines
	   in class 2 */
	
	/* find class 2 in the class list */
	for(j=0; j<*nclassesp; j++) 
	  if(!strcmp((*clp)[j].class_name, class2)) thisclass=j;

	dprintf("Share %s-%s: my class %s shares with class %s (%d allocs)\n",
		class1,class2,class1,(*clp)[thisclass].class_name,nallocs);
      }

      /* or see if we're in class 2 */
      if(l==2 && inclass2==DSM_TRUE) {
	/* if we are in class 2, we need a space for each of the machines
	   in class 1 */
	
	/* find class 1 in the class list */
	for(j=0; j<*nclassesp; j++)
	  if(!strcmp((*clp)[j].class_name, class1)) thisclass=j;

	dprintf("Share %s-%s: my class %s shares with class %s (%d allocs)\n",
		class1,class2,class2,(*clp)[thisclass].class_name,nallocs);
      }

      if(thisclass==-1) continue;

      /* add this class name to the sharelist */
      foundone = DSM_FALSE;
      for(j=0; j<*nclasssharesp; j++) {
	if( (*csp)[j].class == &(*clp)[thisclass] ) {
	  dprintf("  Class %s already entered in sharelist\n",
		  (*clp)[thisclass].class_name);
	  foundone = DSM_TRUE;
	  thisshare = j;
	}
      }
      
      if(foundone==DSM_FALSE) {
	/* we need to add this class to the sharelist */
	dprintf("  No entry for class %s in sharelist; creating it\n",
		  (*clp)[thisclass].class_name);
	*csp = (struct class_share *)
	  realloc(*csp, ++*nclasssharesp * sizeof(struct class_share));
	if(*csp == (struct class_share *)NULL) {
	  perror("realloc(sharelist)");
	  fclose(fp);
	  return(DSM_ERROR);
	}
	thisshare = *nclasssharesp - 1;

	(*csp)[thisshare].class = &(*clp)[thisclass];
	(*csp)[thisshare].alhpp = (struct alloc_list_head **)
	  malloc((*clp)[thisclass].nmembers*sizeof(struct alloc_list_head *));
	if((*csp)[thisshare].alhpp == (struct alloc_list_head **)NULL) {
	  perror("malloc((*csp)[thisclass].alhpp)");
	  fclose(fp);
	  return(DSM_ERROR);
	}
	(*csp)[thisshare].nallocs = 0;
	(*csp)[thisshare].csaep   = (struct class_share_alloc_entry *)NULL;
      }

      /* go through the machine list for this class, adding a node for
	 each one, unless there's already one for the machine */
      for(j=0; j<(*clp)[thisclass].nmembers; j++) {
	/* check whether there's already an entry for this machine */
	foundone = DSM_FALSE;
	for(k=0; k<*nmachp; k++) {
	  if(!strcmp( (*alhp)[k].machine_name,
		     (*clp)[thisclass].machinelist[j].machine_name)) {
	    foundone = DSM_TRUE;
	    thismachine=k;
	  }
	}

	/* if we haven't already made an entry for this machine, do it */
	if(foundone==DSM_FALSE) {
	  dprintf("  No share space for machine %s; creating it\n",
		  (*clp)[thisclass].machinelist[j].machine_name);

	  /* we don't yet have an entry, so make one */
	  *alhp = (struct alloc_list_head *)
	    realloc(*alhp,
		    ++*nmachp*sizeof(struct alloc_list_head));

	  if( *alhp==(struct alloc_list_head *)NULL) {
	    fprintf(stderr, "Can't realloc() *alhp\n");
	    fclose(fp);
	    return(DSM_ERROR);
	  }

	  /* fill in the information in the alloc_list_head structure */
	  thismachine = *nmachp-1;

	  strcpy( (*alhp)[thismachine].machine_name,
		 (*clp)[thisclass].machinelist[j].machine_name);
	  
	  strcpy( (*alhp)[thismachine].machine_addr_string,
		 (*clp)[thisclass].machinelist[j].machine_addr_string);
	  
	  (*alhp)[thismachine].machine_addr = 
	    (*clp)[thisclass].machinelist[j].machine_addr;
	  
	  (*alhp)[thismachine].nallocs = 0;
	  (*alhp)[thismachine].nnames  = 0;
	  (*alhp)[thismachine].largest = (size_t)0;
	  (*alhp)[thismachine].allocs  = (struct alloc_entry *)NULL;
	}

	dprintf("  Adding allocations for machine %s\n",
		  (*clp)[thisclass].machinelist[j].machine_name);
	  
	/* read the allocations */

	/* go to the beginning of the allocations for this share */
	s = fsetpos(fp, &shareallocs);
	if(s!=0) {
	  fprintf(stderr, "Can't fsetpos()\n");
	  fclose(fp);
	  return(DSM_ERROR);
	}

	for(k=0; k<nallocs; k++) {
	  s = read_next_alloc(fp, 
			      allocname, maxstringsize,
			      &size, &n_elements, &type);
	  if(s!=DSM_SUCCESS) return(s);

	  /* add the alloc name to the share list if it's not already
	     there */
	  foundone = DSM_FALSE;
	  for(m=0; m<(*csp)[thisshare].nallocs; m++) {
	    if(!strcmp((*csp)[thisshare].csaep[m].allocname, allocname))
	      foundone = DSM_TRUE;
	  }
	  added_to_share_list = DSM_FALSE;
	  if(foundone == DSM_FALSE) {
	    added_to_share_list = DSM_TRUE;
	    (*csp)[thisshare].csaep = (struct class_share_alloc_entry *)
	      realloc((*csp)[thisshare].csaep,
		      ++(*csp)[thisshare].nallocs 
		         * sizeof(struct class_share_alloc_entry) );
	    if((*csp)[thisshare].csaep == 
	       (struct class_share_alloc_entry *)NULL) {
	      perror("realloc((*csp)[thisshare].csaep)");
	      fclose(fp);
	      return(DSM_ERROR);
	    }

	    strcpy(
              (*csp)[thisshare].csaep[(*csp)[thisshare].nallocs-1].allocname,
		   allocname);
	    (*csp)[thisshare].csaep[(*csp)[thisshare].nallocs-1].aepp =
	      (struct alloc_entry **)NULL;
	  }

	  /* does this allocation already exist in the main allocation
	     list for this machine? */
	  foundone=DSM_FALSE;
	  for(m=0; m<(*alhp)[thismachine].nallocs; m++) {
	    if(!strcmp(allocname, (*alhp)[thismachine].allocs[m].name)) 
	      foundone=DSM_TRUE;
	  }

	  if(foundone==DSM_TRUE) {
	    dprintf("    Allocation %s exists for machine %s",
		    allocname, (*alhp)[thismachine].machine_name);
	    if(added_to_share_list==DSM_TRUE && verbose==DSM_TRUE)
	      fprintf(stderr, " (but do add to sharelist)");
	    if(verbose==DSM_TRUE) fprintf(stderr, "\n");

	    /* if it's a structure, skip its elements */
	    if(type == 'x') {
	      for(n=0; n<ntohl(n_elements); n++) {
		s = read_next_alloc(fp, 
				    allocname, maxstringsize,
				    &dummy, &dummy, &type);
		if(s!=DSM_SUCCESS) {
		  fprintf(stderr,
			  "read_allocfile()3: skipping structure elements"
			  " failed\n");
		}
	      }
	    }
	    continue;
	  }
	  
	  /* make a new entry in the alloclist */
	  (*alhp)[thismachine].allocs = (struct alloc_entry *)
	    realloc( (*alhp)[thismachine].allocs,
		    ++(*alhp)[thismachine].nallocs
		    *sizeof(struct alloc_entry));
	  if( (*alhp)[thismachine].allocs == (struct alloc_entry *)NULL) {
	    perror("Can't realloc() (*alhp).allocs");
	    fclose(fp);
	    return(DSM_ERROR);
	  }
	    
	  /* make a pointer to the new alloc (makes the rest of the code
	     a little more readable */
	  new_alloc = 
	    &( (*alhp)[thismachine].allocs[(*alhp)[thismachine].nallocs - 1]
	       );

	  /* set the top level info for the new alloc */
	  strcpy(new_alloc->name, allocname);
     	  new_alloc->size = (size_t)ntohl(size);

	  if(type == 'x') {
	    new_alloc->is_structure = DSM_TRUE;
	    (*alhp)[thismachine].nnames += (int)ntohl(n_elements);
	    new_alloc->n_elements = (int)ntohl(n_elements);
	  }
	  else {
	    new_alloc->is_structure = DSM_FALSE;
	    (*alhp)[thismachine].nnames += 1;
	    new_alloc->n_elements = 1;
	  }

	  /* create the array of elements */
	  new_alloc->elements = 
	    (struct alloc_element *)malloc(new_alloc->n_elements 
					   * 
					   sizeof(struct alloc_element));
	  if(new_alloc->elements == (struct alloc_element *)NULL) {
	    perror("malloc(elements)");
	    fclose(fp);
	    return(DSM_ERROR);
	  }

	  dprintf("    type=%c  n_elem=%-3d totsize=%-3d  alloc=%s",
		  type,
		  new_alloc->n_elements,
		  new_alloc->size,
		  new_alloc->name);
	  if(added_to_share_list==DSM_TRUE && verbose==DSM_TRUE)
	    fprintf(stderr, " (also add to sharelist)");
	  if(verbose==DSM_TRUE) fprintf(stderr, "\n");

	  /* now fill in the element details */
	  if(type == 'x') {
 	    /* it's a structure; we have to read the elements */
	    for(n=0; n<new_alloc->n_elements; n++) {
	      s = read_next_alloc(fp, 
				  allocname, maxstringsize,
				  &size, &n_elements, &type);
	      if(s!=DSM_SUCCESS) return(s);

	      strcpy( new_alloc->elements[n].name, allocname);
	      new_alloc->elements[n].size = (size_t)ntohl(size);
	      new_alloc->elements[n].n_sub_elements = (int)ntohl(n_elements);

	      m = filter_index(type);
	      if(m==-1) {
		fprintf(stderr,
			"filter_index() failed for alloc %s element %s\n",
			new_alloc->name, allocname);
		fclose(fp);
		return(DSM_ERROR);
	      }
	      new_alloc->elements[n].xdr_filter_index = m;

	      dprintf("         %c  sub_el=%-3d    size=%-3d  |--%s\n",
		      type,
		      new_alloc->elements[n].n_sub_elements,
		      new_alloc->elements[n].size,
		      allocname);
	    } /* loop over structure elements (loopvar n) */
	  }
	  else {
	    /* it's not a structure, so we already have the alloc info;
	       some of these values are redundant with the top-level
	       info, but it's a small price to pay to significantly
	       reduce the complexity of the rest of the code */
	    strcpy( new_alloc->elements[0].name, new_alloc->name);
	    new_alloc->elements[0].size = new_alloc->size;
	    new_alloc->elements[0].n_sub_elements = (int)ntohl(n_elements);

	    m = filter_index(type);
	    if(m==-1) {
	      fprintf(stderr,
		      "filter_index() failed for alloc %s\n", allocname);
	      fclose(fp);
	      return(DSM_ERROR);
	    }
	    new_alloc->elements[0].xdr_filter_index = m;
	  }

	  /* and keep track of the largest alloc */
	  if(new_alloc->size > (*alhp)[thismachine].largest) {
	    (*alhp)[thismachine].largest = new_alloc->size;
	  }

	} /* loop over allocations (loopvar k) */
	
	dprintf("  (%d names total)\n", (*alhp)[thismachine].nnames);

      } /* loop over list of machines in class (loopvar j) */

    } /* loop over checking whether we're in class 1 or class 2 (loopvar l) */

  } /* loop over all shares (loopvar i) */

  fclose(fp);

  for(i=0; i<*nclassesp; i++) free(my_classes[i]);
  free(my_classes);

  return(DSM_SUCCESS);
}
