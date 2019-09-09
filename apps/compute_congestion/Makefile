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

loadbalancer_src = loadbalancer.cc
loadbalancer_obj = $(loadbalancer_src:.cc=.o)

server_src = server.cc
server_obj = $(server_src:.cc=.o)

cfserver_src = cfserver.cc
cfserver_obj = $(cfserver_src:.cc=.o)

cfserver2_src = cfserver2.cc
cfserver2_obj = $(cfserver2_src:.cc=.o)

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

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma
CF_LIBS = -larmadillo -lmlpack -fopenmp

# must be first
all: netbench netbench2 netbench3 netbench4 netbench5 cfclient cfclient2 \
 	loadbalancer server cfserver cfserver2 fanout fanout2 fanout3 cffanout \
	cffanout2 cffanout3

$(cfserver_obj): $(cfserver_src)
	$(CXX) $(CXXFLAGS) $(INC) -c $(cfserver_src) -o $@ -fopenmp -lmlpack

$(cfserver2_obj): $(cfserver2_src)
	$(CXX) $(CXXFLAGS) $(INC) -c $(cfserver2_src) -o $@ -fopenmp -lmlpack

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

loadbalancer: $(lib_obj) $(loadbalancer_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(loadbalancer_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

server: $(lib_obj) $(server_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(server_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cfserver: $(lib_obj) $(cfserver_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cfserver_obj) \
	$(librt_libs) $(RUNTIME_LIBS) -larmadillo -fopenmp -lmlpack

cfserver2: $(lib_obj) $(cfserver2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cfserver2_obj) \
	$(librt_libs) $(RUNTIME_LIBS) -larmadillo -fopenmp -lmlpack

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

# general build rules for all targets
src = $(lib_src) $(netbench_src) $(netbench2_src) $(netbench3_src) $(netbench4_src) \
		  $(netbench5_src) $(cfclient_src) $(cfclient2_src) $(loadbalancer_src) $(server_src) \
		 	$(cfserver_src) $(cfserver2_src)	$(fanout_src) $(fanout2_src) $(fanout3_src) \
		 	$(cffanout_src) $(cffanout2_src) $(cffanout3_src)
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
	rm -f $(obj) $(dep) netbench netbench2 netbench3 netbench4 netbench5 cfclient cfclient2 \
		loadbalancer server cfserver cfserver2 fanout fanout2 fanout3 cffanout cffanout2 cffanout3
