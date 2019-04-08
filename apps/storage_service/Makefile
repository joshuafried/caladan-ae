BASEPATH = ../..
SPDK_PATH = $(BASEPATH)/spdk
INC     = -I../../inc
CFLAGS  = -g -Wall -std=gnu11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T../../base/base.ld -no-pie
LD	= gcc
CC	= gcc
AR	= ar

ifneq ($(DEBUG),)
CFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
CFLAGS += -DNDEBUG -O3
endif

SPDK_LIBS= -L$(SPDK_PATH)/build/lib -L$(SPDK_PATH)/dpdk/build/lib
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

librt_libs = $(BASEPATH)/libruntime.a $(BASEPATH)/libnet.a $(BASEPATH)/libbase.a

src = server.c test-shenango-tcp.c
obj = $(src:.c=.o)

all: test server

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
%.d: %.S
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

server: server.o
	$(LD) $(LDFLAGS) -o $@ $< $(librt_libs) $(SPDK_LIBS) -lpthread

test: test-shenango-tcp.o 
	$(LD) $(LDFLAGS) -o $@ $< $(librt_libs) $(SPDK_LIBS) -lpthread

clean:
	rm -f $(obj)
