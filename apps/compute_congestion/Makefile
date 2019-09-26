# Makefile for netbench
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

lib_src = synthetic_worker.cc distribution.cc util.cc
lib_obj = $(lib_src:.cc=.o)

netbench_src = netbench.cc
netbench_obj = $(netbench_src:.cc=.o)

netbench2_src = netbench2.cc
netbench2_obj = $(netbench2_src:.cc=.o)

netbench3_src = netbench3.cc
netbench3_obj = $(netbench3_src:.cc=.o)

netbench4_src = netbench4.cc
netbench4_obj = $(netbench4_src:.cc=.o)

netbench5_src = netbench5.cc
netbench5_obj = $(netbench5_src:.cc=.o)

cfclient_src = cfclient.cc
cfclient_obj = $(cfclient_src:.cc=.o)

cfclient2_src = cfclient2.cc
cfclient2_obj = $(cfclient2_src:.cc=.o)

cfclient3_src = cfclient3.cc
cfclient3_obj = $(cfclient3_src:.cc=.o)

loadbalancer_src = loadbalancer.cc
loadbalancer_obj = $(loadbalancer_src:.cc=.o)

server_src = server.cc
server_obj = $(server_src:.cc=.o)

sserver_src = sserver.cc
sserver_obj = $(sserver_src:.cc=.o)

sserver2_src = sserver2.cc
sserver2_obj = $(sserver2_src:.cc=.o)

fanout_src = fanout.cc
fanout_obj = $(fanout_src:.cc=.o)

fanout2_src = fanout2.cc
fanout2_obj = $(fanout2_src:.cc=.o)

fanout3_src = fanout3.cc
fanout3_obj = $(fanout3_src:.cc=.o)

cffanout_src = cffanout.cc
cffanout_obj = $(cffanout_src:.cc=.o)

cffanout2_src = cffanout2.cc
cffanout2_obj = $(cffanout2_src:.cc=.o)

cffanout3_src = cffanout3.cc
cffanout3_obj = $(cffanout3_src:.cc=.o)

cffanout4_src = cffanout4.cc
cffanout4_obj = $(cffanout4_src:.cc=.o)

cffanoutlb4_src = cffanoutlb4.cc
cffanoutlb4_obj = $(cffanoutlb4_src:.cc=.o)

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma

# must be first
all: netbench netbench2 netbench3 netbench4 netbench5 cfclient cfclient2 cfclient3 \
 	loadbalancer server sserver sserver2 fanout fanout2 fanout3 cffanout \
	cffanout2 cffanout3 cffanout4 cffanoutlb4

netbench: $(lib_obj) $(netbench_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench2: $(lib_obj) $(netbench2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench3: $(lib_obj) $(netbench3_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench3_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench4: $(lib_obj) $(netbench4_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench4_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

netbench5: $(lib_obj) $(netbench5_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench5_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cfclient: $(lib_obj) $(cfclient_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cfclient_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cfclient2: $(lib_obj) $(cfclient2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cfclient2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cfclient3: $(lib_obj) $(cfclient3_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cfclient3_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

loadbalancer: $(lib_obj) $(loadbalancer_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(loadbalancer_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

server: $(lib_obj) $(server_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(server_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

sserver: $(lib_obj) $(sserver_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(sserver_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

sserver2: $(lib_obj) $(sserver2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(sserver2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

fanout: $(lib_obj) $(fanout_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(fanout_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

fanout2: $(lib_obj) $(fanout2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(fanout2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

fanout3: $(lib_obj) $(fanout3_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(fanout3_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cffanout: $(lib_obj) $(cffanout_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cffanout_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cffanout2: $(lib_obj) $(cffanout2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cffanout2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cffanout3: $(lib_obj) $(cffanout3_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cffanout3_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cffanout4: $(lib_obj) $(cffanout4_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cffanout4_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cffanoutlb4: $(lib_obj) $(cffanoutlb4_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cffanoutlb4_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(lib_src) $(netbench_src) $(netbench2_src) $(netbench3_src) $(netbench4_src) \
		  $(netbench5_src) $(cfclient_src) $(cfclient2_src) $(cfclient3_src) $(loadbalancer_src) $(server_src) \
		 	$(sserver_src) $(sserver2_src)	$(fanout_src) $(fanout2_src) $(fanout3_src) \
		 	$(cffanout_src) $(cffanout2_src) $(cffanout3_src) $(cffanout4_src) $(cffanoutlb4_src)
obj = $(src:.cc=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.cc
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) netbench netbench2 netbench3 netbench4 netbench5 cfclient cfclient2 cfclient3 \
		loadbalancer server sserver sserver2 fanout fanout2 fanout3 cffanout cffanout2 cffanout3 \
		cffanout4 cffanoutlb4
