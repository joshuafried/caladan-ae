# Makefile for mcclient
ROOT_PATH=../..
include $(ROOT_PATH)/shared.mk

lib_src = distribution.cc util.cc
lib_obj = $(lib_src:.cc=.o)

mcclient_src = mcclient.cc
mcclient_obj = $(mcclient_src:.cc=.o)

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma

# must be first
all: mcclient

mcclient: $(lib_obj) $(mcclient_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(mcclient_obj) \
	$(librt_libs) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(lib_src) $(mcclient_src)
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
	rm -f $(obj) $(dep) mcclient
