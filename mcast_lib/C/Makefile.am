# Copyright 2014 University Corporation for Atmospheric Research
#
# This file is part of the Unidata LDM package.  See the file COPYRIGHT in
# the top-level source-directory of the package for copying and redistribution
# conditions.
#
# Process this file with automake(1) to produce file Makefile.in

EXTRA_DIST              = down7.hin \
			  mldm_receiver.hin \
			  mldm_sender_memory.hin \
			  mldm_receiver_memory.hin \
                          prod_index_queue.hin \
                          server_info.hin \
                          sock_funcs.c sock_funcs.hin \
                          up7.hin
BUILT_SOURCES           = down7.h \
			  mldm_receiver.h \
			  mldm_sender_memory.h \
			  mldm_receiver_memory.h \
                          prod_index_queue.h \
                          server_info.h \
                          sock_funcs.h \
                          up7.h
noinst_LTLIBRARIES	= lib.la
lib_la_SOURCES		= down7.c \
                          down7_manager.c down7_manager.h \
			  up7.c \
			  prod_index_map.c prod_index_map.h \
			  mcast_info.c mcast_info.h \
			  mldm_receiver.c \
			  mldm_receiver_memory.c \
			  mldm_sender_manager.c mldm_sender_manager.h \
			  mldm_sender_memory.c \
			  mldm_sender_map.c mldm_sender_map.h \
			  prod_index_queue.c \
			  server_info.c
CPPFLAGS		= -I$(builddir) -I$(srcdir) \
			  -I$(top_builddir) -I$(top_srcdir) \
                          -I$(top_builddir)/pq -I$(top_srcdir)/pq \
                          -I$(top_builddir)/protocol -I$(top_srcdir)/protocol \
                          -I$(top_builddir)/protocol2 -I$(top_srcdir)/protocol2 \
                          -I$(top_builddir)/registry -I$(top_srcdir)/registry \
                          -I$(top_builddir)/ulog -I$(top_srcdir)/ulog \
                          -I$(srcdir)/../vcmtp/VCMTPv3 \
                          -I$(srcdir)/../vcmtp/VCMTPv3/fatma \
                          -I$(srcdir)/../vcmtp/VCMTPv3/shawn \
                          -I$(srcdir)/../vcmtp/VCMTPv3/shawn/recv \
                          -I$(top_builddir)/misc -I$(top_srcdir)/misc \
                          -I$(top_srcdir)/rpc \
                          -I$(top_srcdir)/mcast_lib/C++
CLEANFILES		= *.yaml *_stub*
DISTCLEANFILES          = $(BUILT_SOURCES)

.hin.h:
	$(top_srcdir)/extractDecls $(srcdir)/$*.hin $(srcdir)/$*.c >$@.tmp
	mv -f $@.tmp $@
down7.h:		down7.hin down7.c
mldm_receiver.h:	mldm_receiver.hin mldm_receiver.c
mldm_sender_memory.h:	mldm_sender_memory.hin mldm_sender_memory.c
mldm_receiver_memory.h:	mldm_receiver_memory.c mldm_receiver_memory.hin
prod_index_queue.h:	prod_index_queue.hin prod_index_queue.c
server_info.h:		server_info.hin server_info.c
sock_funcs.h:		sock_funcs.hin sock_funcs.c
up7.h:			up7.hin up7.c

if HAVE_OPMOCK
VCMTPReceiver_stubs	= vcmtpRecvv3_stub.cpp vcmtpRecvv3_stub.hpp
mcast_stubs		= mcast_stub.c mcast_stub.h
pq_stubs		= pq_stub.c pq_stub.h
globals_stubs		= globals_stub.c globals_stub.h
mldm_sender_map_stubs	= mldm_sender_map_stub.c mldm_sender_map_stub.h
unistd_stubs		= unistd_stub.c unistd_stub.h
all_stubs		= $(VCMTPReceiver_stubs) \
                          $(mcast_stubs) \
                          $(pq_stubs) \
                          $(globals_stubs) \
                          $(mldm_sender_map_stubs) \
                          $(unistd_stubs)
BUILT_SOURCES		+= $(all_stubs)
DISTCLEANFILES		+= TEST-test.xml
OPMOCK_INCLUDES 	= -I/usr/lib/gcc/x86_64-redhat-linux/4.8.3/include \
			  -I/usr/include/c++/4.8.3 \
                          -I/usr/local/include \
                          -I/usr/include
$(VCMTPReceiver_stubs):		../vcmtp/VCMTPv3/shawn/recv/vcmtpRecvv3.h
	opmock2 -cpp -i $? -o . $(CPPFLAGS) $(OPMOCK_INCLUDES)
$(mcast_stubs):			../C++/mcast.h
	opmock2 -i $? -o . $(CPPFLAGS) $(OPMOCK_INCLUDES)
$(pq_stubs):			$(top_srcdir)/pq/pq.h
	opmock2 -i $? -o . $(CPPFLAGS) $(OPMOCK_INCLUDES)
$(globals_stubs):		$(top_srcdir)/registry/globals.h
	opmock2 -i $? -o . $(CPPFLAGS) $(OPMOCK_INCLUDES)
$(mldm_sender_map_stubs):	mldm_sender_map.h
	opmock2 -i $? -o . $(CPPFLAGS) $(OPMOCK_INCLUDES)
$(unistd_stubs):		/usr/include/unistd.h
	opmock2 -i $? -o . $(CPPFLAGS) $(OPMOCK_INCLUDES) -k fork

opmock_check_progs			= mldm_sender_manager_test \
                                          mldm_receiver_memory_test \
                                          #mldm_receiver_test
test_cppflags				= $(CPPFLAGS) @OPMOCK_CPPFLAGS@

mldm_sender_manager_test_stub_objs	= mcast_stub.o mldm_sender_map_stub.o \
	pq_stub.o unistd_stub.o globals_stub.o
mldm_sender_manager_test_SOURCES 	= mldm_sender_manager_test.c \
                                          mcast_info.c mcast_info.h
mldm_sender_manager_test_CPPFLAGS	= $(test_cppflags)
mldm_sender_manager_test_LDFLAGS	= @OPMOCK_LDFLAGS@
mldm_sender_manager_test_LDADD		= $(mldm_sender_manager_test_stub_objs) \
					  $(top_builddir)/lib/libldm.la \
				  	  @OPMOCK_LDADD@

mldm_receiver_test_stub_objs		= mcast_stub.o pq_stub.o
mldm_receiver_test_SOURCES 		= mldm_receiver_test.c mldm_receiver.c
mldm_receiver_test_CPPFLAGS		= $(test_cppflags)
mldm_receiver_test_LDFLAGS		= @OPMOCK_LDFLAGS@
mldm_receiver_test_LDADD  		= $(mldm_receiver_test_stub_objs) \
                                          $(top_builddir)/lib/libldm.la \
                                          @OPMOCK_LDADD@

mldm_receiver_memory_test_stub_objs	= globals_stub.o
mldm_receiver_memory_test_SOURCES 	= mldm_receiver_memory_test.c
mldm_receiver_memory_test_CPPFLAGS	= $(test_cppflags)
mldm_receiver_memory_test_LDFLAGS	= @OPMOCK_LDFLAGS@
mldm_receiver_memory_test_LDADD  	= $(mldm_receiver_memory_test_stub_objs) \
                                          $(top_builddir)/lib/libldm.la \
                                          @OPMOCK_LDADD@

opmock_tests				= $(opmock_check_progs)
endif

if HAVE_CUNIT
cunit_check_progs		= prod_index_map_test \
                                  mldm_sender_map_test \
                                  prod_index_queue_test
mldm_sender_map_test_SOURCES	= mldm_sender_map_test.c mldm_sender_map.c
mldm_sender_map_test_CPPFLAGS	= $(CPPFLAGS) @CPPFLAGS_CUNIT@
mldm_sender_map_test_LDADD	= \
    $(top_builddir)/lib/libldm.la \
    @LIBS_CUNIT@
prod_index_map_test_SOURCES	= prod_index_map_test.c prod_index_map.c
prod_index_map_test_CPPFLAGS	= $(CPPFLAGS) @CPPFLAGS_CUNIT@
prod_index_map_test_LDADD	= \
    $(top_builddir)/lib/libldm.la \
    @LIBS_CUNIT@
prod_index_queue_test_SOURCES	= prod_index_queue_test.c prod_index_queue.c
prod_index_queue_test_CPPFLAGS	= $(CPPFLAGS) @CPPFLAGS_CUNIT@
prod_index_queue_test_LDADD	= \
    $(top_builddir)/lib/libldm.la \
    @LIBS_CUNIT@
cunit_tests			= $(cunit_check_progs)

prod_index_map_test_valgrind:	prod_index_map_test
	$(LIBTOOL) --mode=execute valgrind \
	    --leak-check=full --show-reachable=yes ./prod_index_map_test
prod_index_queue_test_valgrind:	prod_index_queue_test
	$(LIBTOOL) --mode=execute valgrind \
	    --leak-check=full --show-reachable=yes ./prod_index_queue_test
endif

check_PROGRAMS			= $(opmock_check_progs) $(cunit_check_progs)
TESTS				= $(opmock_tests) $(cunit_tests)
