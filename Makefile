RDMA_CORE_PATH = $(dir $(realpath $(firstword $(MAKEFILE_LIST))))/rdma-core
INC     = -I./inc -I$(RDMA_CORE_PATH)/build/include
ifneq ($(SPDK),)
SPDK_PATH = spdk
INC += -I$(SPDK_PATH)/include
endif
CFLAGS  = -g -Wall -std=gnu11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T base/base.ld -L $(RDMA_CORE_PATH)/build/lib -Wl,-rpath=$(RDMA_CORE_PATH)/build/lib/
LD	= gcc
CC	= gcc
AR	= ar
SPARSE	= sparse

CHECKFLAGS = -D__CHECKER__ -Waddress-space

ifneq ($(DEBUG),)
CFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
CFLAGS += -DNDEBUG -O3
endif

ifneq ($(TCP_RX_STATS),)
CFLAGS += -DTCP_RX_STATS
endif

RDMA_CORE_LIBS = -libverbs -lmlx5 -lmlx4

# handy for debugging
print-%  : ; @echo $* = $($*)

# libbase.a - the base library
base_src = $(wildcard base/*.c)
base_obj = $(base_src:.c=.o)

#libnet.a - a packet/networking utility library
net_src = $(wildcard net/*.c) $(wildcard net/ixgbe/*.c)
net_obj = $(net_src:.c=.o)

# iokernel - a soft-NIC service
iokernel_src = $(wildcard iokernel/*.c)
iokernel_obj = $(iokernel_src:.c=.o)
iokernel_noht_obj = $(iokernel_src:.c=-noht.o)

# runtime - a user-level threading and networking library
runtime_src = $(wildcard runtime/*.c) $(wildcard runtime/net/*.c) $(wildcard runtime/net/drivers/*.c) $(wildcard runtime/net/drivers/*/*.c)
runtime_asm = $(wildcard runtime/*.S)
runtime_obj = $(runtime_src:.c=.o) $(runtime_asm:.S=.o)

# test cases
test_src = $(wildcard tests/*.c)
test_obj = $(test_src:.c=.o)
test_targets = $(basename $(test_src))

ifneq ($(SPDK),)
SPDK_LIBS= -L$(SPDK_PATH)/build/lib -L$(SPDK_PATH)/dpdk/build/lib
SPDK_LIBS += -lspdk_nvme
SPDK_LIBS += -lspdk_util
SPDK_LIBS += -lspdk_env_dpdk
SPDK_LIBS += -lspdk_log
SPDK_LIBS += -lspdk_sock
SPDK_LIBS += -ldpdk
SPDK_LIBS += -lpthread
SPDK_LIBS += -lrt
SPDK_LIBS += -luuid
SPDK_LIBS += -lcrypto
SPDK_LIBS += -lnuma
SPDK_LIBS += -ldl
endif

# must be first
all: libbase.a libnet.a libruntime.a iokerneld iokerneld-noht $(test_targets)

libbase.a: $(base_obj)
	$(AR) rcs $@ $^

libnet.a: $(net_obj)
	$(AR) rcs $@ $^

libruntime.a: $(runtime_obj)
	$(AR) rcs $@ $^

iokerneld: $(iokernel_obj) libbase.a libnet.a base/base.ld
	$(LD) $(LDFLAGS) -o $@ $(iokernel_obj) libbase.a libnet.a \
	-lpthread -lnuma -ldl

iokerneld-noht: $(iokernel_noht_obj) libbase.a libnet.a base/base.ld
	$(LD) $(LDFLAGS) -o $@ $(iokernel_noht_obj) libbase.a libnet.a \
	 -lpthread -lnuma -ldl

$(test_targets): $(test_obj) libbase.a libruntime.a libnet.a base/base.ld
	$(LD) $(LDFLAGS) -o $@ $@.o libruntime.a libnet.a libbase.a -lpthread $(RDMA_CORE_LIBS) $(SPDK_LIBS)

# general build rules for all targets
src = $(base_src) $(net_src) $(runtime_src) $(iokernel_src) $(test_src)
asm = $(runtime_asm)
obj = $(src:.c=.o) $(asm:.S=.o) $(iokernel_src:.c=-noht.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%-noht.d %.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

%-noht.o: %.c
	$(CC) $(CFLAGS) -Wno-unused-variable -DCORES_NOHT -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
%.d: %.S
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

# prints sparse checker tool output
sparse: $(src)
	$(foreach f,$^,$(SPARSE) $(filter-out -std=gnu11, $(CFLAGS)) $(CHECKFLAGS) $(f);)

.PHONY: clean
clean:
	rm -f $(obj) $(dep) libbase.a libnet.a libruntime.a \
	iokerneld iokerneld-noht $(test_targets)
