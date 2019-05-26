BASEPATH = ../..
CXXPATH = $(BASEPATH)/bindings/cc
SPDK_PATH = $(BASEPATH)/spdk
RDMA_CORE_PATH = $(BASEPATH)/rdma-core
INC     = -I$(BASEPATH)/inc -I$(BASEPATH)/bindings/cc -I$(SPDK_PATH)/include
CXXFLAGS  = -g -Wall -std=gnu++11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T$(BASEPATH)/base/base.ld -no-pie -L $(RDMA_CORE_PATH)/build/lib -Wl,-rpath=$(RDMA_CORE_PATH)/build/lib/
LD	= g++
CC	= g++
AR	= ar

ifneq ($(DEBUG),)
CXXFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
CXXFLAGS += -DNDEBUG -O3
endif

SPDK_LIBS = -L$(SPDK_PATH)/build/lib -L$(SPDK_PATH)/dpdk/build/lib
SPDK_LIBS += -lspdk_nvme
SPDK_LIBS += -lspdk_util
SPDK_LIBS += -lspdk_env_dpdk
SPDK_LIBS += -lspdk_log
SPDK_LIBS += -lspdk_sock
SPDK_LIBS += -ldpdk
SPDK_LIBS += -lrte_bus_pci
SPDK_LIBS += -lrte_bus_vdev
SPDK_LIBS += -lrte_eal
SPDK_LIBS += -lrte_ethdev
SPDK_LIBS += -lrte_kvargs
SPDK_LIBS += -lrte_mbuf
SPDK_LIBS += -lrte_mempool
SPDK_LIBS += -lrte_mempool_bucket
SPDK_LIBS += -lrte_mempool_ring
SPDK_LIBS += -lrte_net
SPDK_LIBS += -lrte_pci
SPDK_LIBS += -lrte_ring
SPDK_LIBS += -lrt -luuid -lcrypto -lnuma -ldl

RDMA_CORE_LIBS = -libverbs -lmlx5 -lmlx4

librt_libs = $(CXXPATH)/librt++.a $(BASEPATH)/libruntime.a $(BASEPATH)/libnet.a $(BASEPATH)/libbase.a

src = server.cc
obj = $(src:.cc=.o)

all: server

%.o: %.cc
	$(CC) $(CXXFLAGS) -c $< -o $@
%.d: %.S
	@$(CC) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.S
	$(CC) $(CXXFLAGS) -c $< -o $@

server: server.o $(librt_libs)
	$(LD) $(LDFLAGS) -o $@ $< $(librt_libs) $(SPDK_LIBS) $(RDMA_CORE_LIBS) -lpthread

clean:
	rm -f $(obj) server
