/*
 * queue.h - shared memory queues between the iokernel and the runtimes
 */

#pragma once

#include <base/stddef.h>

/* preamble to ingress network packets */
struct rx_net_hdr {
	unsigned long completion_data; /* a tag to help complete the request */
	unsigned int len;	/* the length of the payload */
	unsigned int rss_hash;	/* the HW RSS 5-tuple hash */
	unsigned int csum_type; /* the type of checksum */
	unsigned int csum;	/* 16-bit one's complement */
	char	     payload[];	/* packet data */
};

/* possible values for @csum_type above */
enum {
	/*
	 * Hardware did not provide checksum information.
	 */
	CHECKSUM_TYPE_NEEDED = 0,

	/*
	 * The checksum was verified by hardware and found to be valid.
	 */
	CHECKSUM_TYPE_UNNECESSARY,

	/* 
	 * Hardware provided a 16 bit one's complement sum from after the LL
	 * header to the end of the packet. VLAN tags (if present) are included
	 * in the sum. This is the most robust checksum type because it's useful
	 * even if the NIC can't parse the headers.
	 */
	CHECKSUM_TYPE_COMPLETE,
};


/*
 * RX queues: IOKERNEL -> RUNTIMES
 * These queues multiplex several different types of requests.
 */
enum {
	RX_NET_RECV = 0,	/* points to a struct rx_net_hdr */
	RX_NET_COMPLETE,	/* contains tx_net_hdr.completion_data */
	RX_JOIN,		/* immediate detach request for a kthread */
	RX_CALL_NR,		/* number of commands */
};


/*
 * TX command queues: RUNTIMES -> IOKERNEL
 * These queues handle a variety of commands, and typically they are handled
 * much faster by the IOKERNEL than packets, so no HOL blocking.
 */
enum {
	TXCMD_PARKED = 0,		/* hint to iokernel that kthread is parked */
	TXCMD_PARKED_LAST,	/* the last undetached kthread is parking */
	TXCMD_NR,		/* number of commands */
};
