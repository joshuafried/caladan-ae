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

loadbalancer_src = loadbalancer.cc
loadbalancer_obj = $(loadbalancer_src:.cc=.o)

server_src = server.cc
server_obj = $(server_src:.cc=.o)

cfserver_src = cfserver.cc
cfserver_obj = $(cfserver_src:.cc=.o)

fanout_src = fanout.cc
fanout_obj = $(fanout_src:.cc=.o)

fanout2_src = fanout2.cc
fanout2_obj = $(fanout2_src:.cc=.o)

fanout3_src = fanout3.cc
fanout3_obj = $(fanout3_src:.cc=.o)

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma
CF_LIBS = -larmadillo -lmlpack -fopenmp

# must be first
all: netbench netbench2 netbench3 netbench4 netbench5 loadbalancer server cfserver fanout fanout2 fanout3

$(cfserver_obj): $(cfserver_src)
	$(CXX) $(CXXFLAGS) $(INC) -c $(cfserver_src) -o $@ -fopenmp

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

loadbalancer: $(lib_obj) $(loadbalancer_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(loadbalancer_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

server: $(lib_obj) $(server_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(server_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

cfserver: $(lib_obj) $(cfserver_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(cfserver_obj) \
	$(librt_libs) /users/inhocho/mlpack-3.1.1/build/lib/libmlpack.so.3 \
 	$(RUNTIME_LIBS) -larmadillo -fopenmp

fanout: $(lib_obj) $(fanout_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(fanout_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

fanout2: $(lib_obj) $(fanout2_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(fanout2_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

fanout3: $(lib_obj) $(fanout3_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(fanout3_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(lib_src) $(netbench_src) $(netbench2_src) $(netbench3_src) $(netbench4_src) \
		  $(netbench5_src) $(loadbalancer_src) $(server_src) $(cfserver_src) $(fanout_src) \
		 	$(fanout2_src) $(fanout3_src)
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
	rm -f $(obj) $(dep) netbench netbench2 netbench3 netbench4 netbench5 loadbalancer \
	 	server cfserver fanout fanout2 fanout3
