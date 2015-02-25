# 
# Makefile for building SMA distributed shared memory (dsm).
#
#
# Charlie Katz, 16 Mar 2001
#
# $Id: Makefile,v 2.6 2012/05/21 15:32:23 ckatz Exp $
#

####################################################################
# Definitions for building under LynxOS
LYNXOSCC     = gcc
LYNXOSCFLAGS = -g -Wall -mthreads -I$(COMMONINC) \
               -I$(ENV_PREFIX)/sys/include/family/ppc \
	       -I$(ENV_PREFIX)/sys/include/kernel     
LYNXOSLIBS   = $(COMMONLIB)/commonLib -lrpc -lnetinet


###################################################################
# Definitions for building under Linux
LINUXCC     = gcc
LINUXCFLAGS = -g -Wall -D_POSIX_SOURCE -D_BSD_SOURCE -D_SVID_SOURCE
LINUXLIBS   = -lpthread -lrt

##############################################################
# OS-independent definitions
DEBUG =

OBJS = dsm_main.o dsm_client.o dsm_server.o dsm_dispatcher.o dsm_xdr.o \
       dsm_util.o dsm_read_allocfile.o


###################################################################
# pick the definitions based on the OS
ifeq ($(HOSTTYPE),i386-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),i486-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),i686-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),x86_64-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS) -m32
  LIBS     = $(LINUXLIBS)
  OSFOUND  = 1
endif

# PowerPC Linux
ifeq ($(HOSTTYPE),powerpc)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),lynxos-powerpc)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LYNXOSCC)
  CFLAGS   = $(LYNXOSCFLAGS)
  LIBS     = $(LYNXOSLIBS)
  OSFOUND  = 1
endif

# assume lynxos cross-development environment
ifeq ($(HOSTTYPE),sun4)
  HOSTDESC = "lynxos-powerpc (cross-compiled)"
  CC       = $(LYNXOSCC)
  CFLAGS   = $(LYNXOSCFLAGS)
  LIBS     = $(LYNXOSLIBS)
  OSFOUND  = 1
endif

# roach
UNAMER = $(shell uname -r)
ifeq ($(findstring borph, $(UNAMER)),borph)
  HOSTDESC = "roach"
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  OSFOUND  = 1
endif


########################################################
DSMLIB = $(COMMONLIB)/libdsm.a
DSMBIN = $(COMMONBIN)/dsm


####################################################################
####################################################################
####################################################################

all: report dsm api symlinks testsuite


report:
ifndef OSFOUND
	@echo HOSTTYPE "$(HOSTTYPE)" unknown
	$(warning HOSTTYPE "$(HOSTTYPE)" unknown)
	$(error HOSTTYPE "$(HOSTTYPE)" unknown)
else
	@echo Building dsm for $(HOSTDESC)
endif


#############
# the server
dsm: $(DSMBIN)

$(DSMBIN): $(OBJS) dsm.h
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

dsm_main.o:           dsm_main.c           dsm.h
dsm_client.o:         dsm_client.c         dsm.h
dsm_server.o:         dsm_server.c         dsm.h
dsm_dispatcher.o:     dsm_dispatcher.c     dsm.h
dsm_xdr.o:            dsm_xdr.c            dsm.h
dsm_util.o:           dsm_util.c           dsm.h
dsm_read_allocfile.o: dsm_read_allocfile.c dsm.h

dsm_dprintf.o:	      dsm_dprintf.c

###################
# allocation stuff 
allocation: dsm_allocation.bin

dsm_allocation.bin: dsm_allocation dsm_allocator dsm.h
	dsm_allocator $(DEBUG) dsm_allocation dsm_allocation.bin


#####################################
# the API simply lives in dsm_client
api: $(DSMLIB)

$(DSMLIB): dsm_client.o dsm_xdr.o dsm_read_allocfile.o dsm_dprintf.o dsm.h
	/bin/rm -f $(DSMLIB)
	ar r $(DSMLIB) dsm_client.o dsm_read_allocfile.o dsm_xdr.o dsm_dprintf.o
	/bin/rm -f $(OBJS) dsm_dprintf.o

####################################
# symlinks
symlinks: $(GLOBALINC)/dsm.h

$(GLOBALINC)/dsm.h: 
	cd $(GLOBALINC); /bin/rm -f dsm.h;    ln -s ../dsm/dsm.h    .

$(GLOBALCFG)/dsm_allocation: 
	cd $(GLOBALCFG); /bin/rm -f dsm_allocation; ln -s ../dsm/dsm_allocation .

#######################################
# test suite
testsuite:
	cd test; make; /bin/rm -f dsm_dprintf.o


##############
# cleanup 
clean:
	/bin/rm -f $(DSMBIN) $(DSMLIB) $(OBJS) $(GLOBALINC)/dsm.h dsm_dprintf.o *~ core*
	cd test; make clean
