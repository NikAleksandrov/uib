/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UIB_PROTOCOL_H
#define UIB_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct uib_context;
struct uib_qp;
struct uib_send_wr;

struct uib_rx_meta {
	uint64_t rx_time_ns;
	uint8_t ecn;
	uint8_t integrity_validated;
};

struct uib_wire_meta {
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t ip_id;
};

/*
 * Protocol operations separate RDMA packet semantics from the mechanism used
 * to carry packets. All ports in uib_wire_meta are in network byte order.
 */
struct uib_protocol_ops {
	const char *name;
	uint16_t default_port;
	size_t min_packet;
	/*
	 * Nonzero when a transport may reach one peer through several derived
	 * destination ports. Protocols whose destination port is fixed by
	 * their specification, such as RoCEv2 on 4791, must leave this clear
	 * so that a transport never moves their traffic off that port.
	 */
	uint8_t multipath_ports;
	/*
	 * Optional. Return nonzero when the protocol can carry this path MTU.
	 * A protocol that omits it accepts any value. RoCEv2 uses it to hold
	 * the architectural set, which also keeps the value inside the packet
	 * buffers it sizes from that maximum.
	 */
	int (*path_mtu_valid)(uint32_t path_mtu);

	int (*init)(struct uib_context *ctx);
	void (*fini)(struct uib_context *ctx);
	int (*qp_init)(struct uib_qp *qp);
	void (*qp_fini)(struct uib_qp *qp);
	void (*qp_modified)(struct uib_qp *qp, int attr_mask);

	int (*packet_qpn)(const void *packet, size_t len, uint32_t *qpn);
	int (*post_send)(struct uib_qp *qp, struct uib_send_wr *wr,
			 struct uib_send_wr **bad);
	int (*handle_packet)(struct uib_qp *qp,
			     const struct sockaddr_in *src,
			     const void *packet, size_t len,
			     const struct uib_rx_meta *meta);
	void (*retransmit_tick)(struct uib_qp *qp, uint64_t now_ns);

	int (*wire_encode)(const struct uib_wire_meta *meta,
			   void *packet, size_t len);
	int (*wire_decode)(const struct uib_wire_meta *meta,
			   void *packet, size_t len);
};

extern const struct uib_protocol_ops uib_rocev2_protocol_ops;
extern const struct uib_protocol_ops uib_dummy_protocol_ops;
extern const struct uib_protocol_ops uib_uet_protocol_ops;

const struct uib_protocol_ops *uib_protocol_find(const char *name);
const struct uib_protocol_ops *uib_protocol_from_env(void);
uint16_t uib_protocol_default_port(const char *name);

#endif
