/*
 * include/types/connection.h
 * This file describes the connection struct and associated constants.
 *
 * Copyright (C) 2000-2014 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _TYPES_CONNECTION_H
#define _TYPES_CONNECTION_H

#include <stdlib.h>
#include <sys/socket.h>

#include <common/config.h>
#include <common/ist.h>

#include <types/listener.h>
#include <types/obj_type.h>
#include <types/port_range.h>
#include <types/protocol.h>

#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

/* referenced below */
struct connection;
struct conn_stream;
struct buffer;
struct server;
struct pipe;


/* A connection handle is how we differentiate two connections on the lower
 * layers. It usually is a file descriptor but can be a connection id.
 */
union conn_handle {
	int fd;                 /* file descriptor, for regular sockets */
};

/* conn_stream flags */
enum {
	CS_FL_NONE          = 0x00000000,  /* Just for initialization purposes */
	CS_FL_DATA_RD_ENA   = 0x00000001,  /* receiving data is allowed */
	CS_FL_DATA_WR_ENA   = 0x00000002,  /* sending data is desired */

	CS_FL_SHRD          = 0x00000010,  /* read shut, draining extra data */
	CS_FL_SHRR          = 0x00000020,  /* read shut, resetting extra data */
	CS_FL_SHR           = CS_FL_SHRD | CS_FL_SHRR, /* read shut status */

	CS_FL_SHWN          = 0x00000040,  /* write shut, verbose mode */
	CS_FL_SHWS          = 0x00000080,  /* write shut, silent mode */
	CS_FL_SHW           = CS_FL_SHWN | CS_FL_SHWS, /* write shut status */


	CS_FL_ERROR         = 0x00000100,  /* a fatal error was reported */
	CS_FL_RCV_MORE      = 0x00000200,  /* more bytes to receive but not enough room */
	CS_FL_EOS           = 0x00001000,  /* End of stream */
	CS_FL_KILL_CONN     = 0x00002000,  /* must kill the connection when the CS closes */
};

/* cs_shutr() modes */
enum cs_shr_mode {
	CS_SHR_DRAIN        = 0,           /* read shutdown, drain any extra stuff */
	CS_SHR_RESET        = 1,           /* read shutdown, reset any extra stuff */
};

/* cs_shutw() modes */
enum cs_shw_mode {
	CS_SHW_NORMAL       = 0,           /* regular write shutdown */
	CS_SHW_SILENT       = 1,           /* imminent close, don't notify peer */
};

/* For each direction, we have a CO_FL_{SOCK,DATA}_<DIR>_ENA flag, which
 * indicates if read or write is desired in that direction for the respective
 * layers. The current status corresponding to the current layer being used is
 * remembered in the CO_FL_CURR_<DIR>_ENA flag. The need to poll (ie receipt of
 * EAGAIN) is remembered at the file descriptor level so that even when the
 * activity is stopped and restarted, we still remember whether it was needed
 * to poll before attempting the I/O.
 *
 * The CO_FL_CURR_<DIR>_ENA flag is set from the FD status in
 * conn_refresh_polling_flags(). The FD state is updated according to these
 * flags in conn_cond_update_polling().
 */

/* flags for use in connection->flags */
enum {
	CO_FL_NONE          = 0x00000000,  /* Just for initialization purposes */

	/* Do not change these values without updating conn_*_poll_changes() ! */
	CO_FL_SOCK_RD_ENA   = 0x00000001,  /* receiving handshakes is allowed */
	CO_FL_XPRT_RD_ENA   = 0x00000002,  /* receiving data is allowed */
	CO_FL_CURR_RD_ENA   = 0x00000004,  /* receiving is currently allowed */
	/* unused : 0x00000008 */

	CO_FL_SOCK_WR_ENA   = 0x00000010,  /* sending handshakes is desired */
	CO_FL_XPRT_WR_ENA   = 0x00000020,  /* sending data is desired */
	CO_FL_CURR_WR_ENA   = 0x00000040,  /* sending is currently desired */
	/* unused : 0x00000080 */

	/* These flags indicate whether the Control and Transport layers are initialized */
	CO_FL_CTRL_READY    = 0x00000100, /* FD was registered, fd_delete() needed */
	CO_FL_XPRT_READY    = 0x00000200, /* xprt_init() done, xprt_close() needed */

	CO_FL_WILL_UPDATE   = 0x00000400, /* the conn handler will take care of updating the polling */

	/* This flag is used by data layers to indicate they had to stop
	 * receiving data because a buffer was full. The connection handler
	 * clears it before first calling the I/O and data callbacks.
	 */
	CO_FL_WAIT_ROOM     = 0x00000800,  /* data sink is full */

	/* These flags are used to report whether the from/to addresses are set or not */
	CO_FL_ADDR_FROM_SET = 0x00001000,  /* addr.from is set */
	CO_FL_ADDR_TO_SET   = 0x00002000,  /* addr.to is set */

	CO_FL_EARLY_SSL_HS  = 0x00004000,  /* We have early data pending, don't start SSL handshake yet */
	CO_FL_EARLY_DATA    = 0x00008000,  /* At least some of the data are early data */
	/* unused : 0x00010000 */
	/* unused : 0x00020000 */

	/* flags used to remember what shutdown have been performed/reported */
	CO_FL_SOCK_RD_SH    = 0x00040000,  /* SOCK layer was notified about shutr/read0 */
	CO_FL_SOCK_WR_SH    = 0x00080000,  /* SOCK layer asked for shutw */

	/* flags used to report connection errors or other closing conditions */
	CO_FL_ERROR         = 0x00100000,  /* a fatal error was reported     */
	CO_FL_NOTIFY_DONE   = 0x001C0000,  /* any xprt shut/error flags above needs to be reported */
	CO_FL_NOTIFY_DATA   = 0x001C0000,  /* any shut/error flags above needs to be reported */

	/* flags used to report connection status updates */
	CO_FL_CONNECTED     = 0x00200000,  /* L4+L6 now ready ; extra handshakes may or may not exist */
	CO_FL_WAIT_L4_CONN  = 0x00400000,  /* waiting for L4 to be connected */
	CO_FL_WAIT_L6_CONN  = 0x00800000,  /* waiting for L6 to be connected (eg: SSL) */

	/*** All the flags below are used for connection handshakes. Any new
	 * handshake should be added after this point, and CO_FL_HANDSHAKE
	 * should be updated.
	 */
	CO_FL_SEND_PROXY    = 0x01000000,  /* send a valid PROXY protocol header */
	CO_FL_SSL_WAIT_HS   = 0x02000000,  /* wait for an SSL handshake to complete */
	CO_FL_ACCEPT_PROXY  = 0x04000000,  /* receive a valid PROXY protocol header */
	CO_FL_ACCEPT_CIP    = 0x08000000,  /* receive a valid NetScaler Client IP header */

	/* below we have all handshake flags grouped into one */
	CO_FL_HANDSHAKE     = CO_FL_SEND_PROXY | CO_FL_SSL_WAIT_HS | CO_FL_ACCEPT_PROXY | CO_FL_ACCEPT_CIP,

	/* when any of these flags is set, polling is defined by socket-layer
	 * operations, as opposed to data-layer. Transport is explicitly not
	 * mentionned here to avoid any confusion, since it can be the same
	 * as DATA or SOCK on some implementations.
	 */
	CO_FL_POLL_SOCK     = CO_FL_HANDSHAKE | CO_FL_WAIT_L4_CONN | CO_FL_WAIT_L6_CONN,

	/* This connection may not be shared between clients */
	CO_FL_PRIVATE       = 0x10000000,

	/* This flag is used to know that a PROXY protocol header was sent by the client */
	CO_FL_RCVD_PROXY    = 0x20000000,

	/* unused : 0x40000000 */

	/* This last flag indicates that the transport layer is used (for instance
	 * by logs) and must not be cleared yet. The last call to conn_xprt_close()
	 * must be done after clearing this flag.
	 */
	CO_FL_XPRT_TRACKED  = 0x80000000,
};


/* possible connection error codes */
enum {
	CO_ER_NONE,             /* no error */

	CO_ER_CONF_FDLIM,       /* reached process' configured FD limitation */
	CO_ER_PROC_FDLIM,       /* reached process' FD limitation */
	CO_ER_SYS_FDLIM,        /* reached system's FD limitation */
	CO_ER_SYS_MEMLIM,       /* reached system buffers limitation */
	CO_ER_NOPROTO,          /* protocol not supported */
	CO_ER_SOCK_ERR,         /* other socket error */

	CO_ER_PORT_RANGE,       /* source port range exhausted */
	CO_ER_CANT_BIND,        /* can't bind to source address */
	CO_ER_FREE_PORTS,       /* no more free ports on the system */
	CO_ER_ADDR_INUSE,       /* local address already in use */

	CO_ER_PRX_EMPTY,        /* nothing received in PROXY protocol header */
	CO_ER_PRX_ABORT,        /* client abort during PROXY protocol header */
	CO_ER_PRX_TIMEOUT,      /* timeout while waiting for a PROXY header */
	CO_ER_PRX_TRUNCATED,    /* truncated PROXY protocol header */
	CO_ER_PRX_NOT_HDR,      /* not a PROXY protocol header */
	CO_ER_PRX_BAD_HDR,      /* bad PROXY protocol header */
	CO_ER_PRX_BAD_PROTO,    /* unsupported protocol in PROXY header */

	CO_ER_CIP_EMPTY,        /* nothing received in NetScaler Client IP header */
	CO_ER_CIP_ABORT,        /* client abort during NetScaler Client IP header */
	CO_ER_CIP_TIMEOUT,      /* timeout while waiting for a NetScaler Client IP header */
	CO_ER_CIP_TRUNCATED,    /* truncated NetScaler Client IP header */
	CO_ER_CIP_BAD_MAGIC,    /* bad magic number in NetScaler Client IP header */
	CO_ER_CIP_BAD_PROTO,    /* unsupported protocol in NetScaler Client IP header */

	CO_ER_SSL_EMPTY,        /* client closed during SSL handshake */
	CO_ER_SSL_ABORT,        /* client abort during SSL handshake */
	CO_ER_SSL_TIMEOUT,      /* timeout during SSL handshake */
	CO_ER_SSL_TOO_MANY,     /* too many SSL connections */
	CO_ER_SSL_NO_MEM,       /* no more memory to allocate an SSL connection */
	CO_ER_SSL_RENEG,        /* forbidden client renegociation */
	CO_ER_SSL_CA_FAIL,      /* client cert verification failed in the CA chain */
	CO_ER_SSL_CRT_FAIL,     /* client cert verification failed on the certificate */
	CO_ER_SSL_MISMATCH,     /* Server presented an SSL certificate different from the configured one */
	CO_ER_SSL_MISMATCH_SNI, /* Server presented an SSL certificate different from the expected one */
	CO_ER_SSL_HANDSHAKE,    /* SSL error during handshake */
	CO_ER_SSL_HANDSHAKE_HB, /* SSL error during handshake with heartbeat present */
	CO_ER_SSL_KILLED_HB,    /* Stopped a TLSv1 heartbeat attack (CVE-2014-0160) */
	CO_ER_SSL_NO_TARGET,    /* unknown target (not client nor server) */
	CO_ER_SSL_EARLY_FAILED, /* Server refused early data */
};

/* source address settings for outgoing connections */
enum {
	/* Tproxy exclusive values from 0 to 7 */
	CO_SRC_TPROXY_ADDR = 0x0001,    /* bind to this non-local address when connecting */
	CO_SRC_TPROXY_CIP  = 0x0002,    /* bind to the client's IP address when connecting */
	CO_SRC_TPROXY_CLI  = 0x0003,    /* bind to the client's IP+port when connecting */
	CO_SRC_TPROXY_DYN  = 0x0004,    /* bind to a dynamically computed non-local address */
	CO_SRC_TPROXY_MASK = 0x0007,    /* bind to a non-local address when connecting */

	CO_SRC_BIND        = 0x0008,    /* bind to a specific source address when connecting */
};

/* flags that can be passed to xprt->snd_buf() */
enum {
	CO_SFL_MSG_MORE    = 0x0001,    /* More data to come afterwards */
	CO_SFL_STREAMER    = 0x0002,    /* Producer is continuously streaming data */
};

/* known transport layers (for ease of lookup) */
enum {
	XPRT_RAW = 0,
	XPRT_SSL = 1,
	XPRT_ENTRIES /* must be last one */
};

/* MUX-specific flags */
enum {
	MX_FL_NONE        = 0x00000000,
	MX_FL_CLEAN_ABRT  = 0x00000001, /* abort is clearly reported as an error */
};

/* xprt_ops describes transport-layer operations for a connection. They
 * generally run over a socket-based control layer, but not always. Some
 * of them are used for data transfer with the upper layer (rcv_*, snd_*)
 * and the other ones are used to setup and release the transport layer.
 */
struct xprt_ops {
	int  (*rcv_buf)(struct connection *conn, struct buffer *buf, int count); /* recv callback */
	int  (*snd_buf)(struct connection *conn, struct buffer *buf, int flags); /* send callback */
	int  (*rcv_pipe)(struct connection *conn, struct pipe *pipe, unsigned int count); /* recv-to-pipe callback */
	int  (*snd_pipe)(struct connection *conn, struct pipe *pipe); /* send-to-pipe callback */
	void (*shutr)(struct connection *, int);    /* shutr function */
	void (*shutw)(struct connection *, int);    /* shutw function */
	void (*close)(struct connection *);         /* close the transport layer */
	int  (*init)(struct connection *conn);      /* initialize the transport layer */
	int  (*prepare_bind_conf)(struct bind_conf *conf); /* prepare a whole bind_conf */
	void (*destroy_bind_conf)(struct bind_conf *conf); /* destroy a whole bind_conf */
	int  (*prepare_srv)(struct server *srv);    /* prepare a server context */
	void (*destroy_srv)(struct server *srv);    /* destroy a server context */
	int  (*get_alpn)(const struct connection *conn, const char **str, int *len); /* get application layer name */
	char name[8];                               /* transport layer name, zero-terminated */
};

/* mux_ops describes the mux operations, which are to be performed at the
 * connection level after data are exchanged with the transport layer in order
 * to propagate them to streams. The <init> function will automatically be
 * called once the mux is instanciated by the connection's owner at the end
 * of a transport handshake, when it is about to transfer data and the data
 * layer is not ready yet.
 */
struct mux_ops {
	int  (*init)(struct connection *conn);        /* early initialization */
	void (*recv)(struct connection *conn);        /* mux-layer recv callback */
	void (*send)(struct connection *conn);        /* mux-layer send callback */
	int  (*wake)(struct connection *conn);        /* mux-layer callback to report activity, mandatory */
	void (*update_poll)(struct conn_stream *cs);  /* commit cs flags to mux/conn */
	int  (*rcv_buf)(struct conn_stream *cs, struct buffer *buf, int count); /* Called from the upper layer to get data */
	int  (*snd_buf)(struct conn_stream *cs, struct buffer *buf, int flags); /* Called from the upper layer to send data */
	int  (*rcv_pipe)(struct conn_stream *cs, struct pipe *pipe, unsigned int count); /* recv-to-pipe callback */
	int  (*snd_pipe)(struct conn_stream *cs, struct pipe *pipe); /* send-to-pipe callback */
	void (*shutr)(struct conn_stream *cs, enum cs_shr_mode);     /* shutr function */
	void (*shutw)(struct conn_stream *cs, enum cs_shw_mode);     /* shutw function */

	struct conn_stream *(*attach)(struct connection *); /* Create and attach a conn_stream to an outgoing connection */
	void (*detach)(struct conn_stream *); /* Detach a conn_stream from an outgoing connection, when the request is done */
	void (*show_fd)(struct chunk *, struct connection *); /* append some data about connection into chunk for "show fd" */
	unsigned int flags;                           /* some flags characterizing the mux's capabilities (MX_FL_*) */
	char name[8];                                 /* mux layer name, zero-terminated */
};

/* data_cb describes the data layer's recv and send callbacks which are called
 * when I/O activity was detected after the transport layer is ready. These
 * callbacks are supposed to make use of the xprt_ops above to exchange data
 * from/to buffers and pipes. The <wake> callback is used to report activity
 * at the transport layer, which can be a connection opening/close, or any
 * data movement. It may abort a connection by returning < 0.
 */
struct data_cb {
	void (*recv)(struct conn_stream *cs);  /* data-layer recv callback */
	void (*send)(struct conn_stream *cs);  /* data-layer send callback */
	int  (*wake)(struct conn_stream *cs);  /* data-layer callback to report activity */
	char name[8];                           /* data layer name, zero-terminated */
};

struct my_tcphdr {
	uint16_t source;
	uint16_t dest;
};

/* a connection source profile defines all the parameters needed to properly
 * bind an outgoing connection for a server or proxy.
 */

struct conn_src {
	unsigned int opts;                   /* CO_SRC_* */
	int iface_len;                       /* bind interface name length */
	char *iface_name;                    /* bind interface name or NULL */
	struct port_range *sport_range;      /* optional per-server TCP source ports */
	struct sockaddr_storage source_addr; /* the address to which we want to bind for connect() */
#if defined(CONFIG_HAP_TRANSPARENT)
	struct sockaddr_storage tproxy_addr; /* non-local address we want to bind to for connect() */
	char *bind_hdr_name;                 /* bind to this header name if defined */
	int bind_hdr_len;                    /* length of the name of the header above */
	int bind_hdr_occ;                    /* occurrence number of header above: >0 = from first, <0 = from end, 0=disabled */
#endif
};

/*
 * This structure describes the elements of a connection relevant to a stream
 */
struct conn_stream {
	enum obj_type obj_type;              /* differentiates connection from applet context */
	struct connection *conn;             /* xprt-level connection */
	unsigned int flags;                    /* CS_FL_* */
	void *data;                          /* pointer to upper layer's entity (eg: stream interface) */
	const struct data_cb *data_cb;       /* data layer callbacks. Must be set before xprt->init() */
	void *ctx;                           /* mux-specific context */
};

/* This structure describes a connection with its methods and data.
 * A connection may be performed to proxy or server via a local or remote
 * socket, and can also be made to an internal applet. It can support
 * several transport schemes (raw, ssl, ...). It can support several
 * connection control schemes, generally a protocol for socket-oriented
 * connections, but other methods for applets. The xprt_done_cb() callback
 * is called once the transport layer initialization is done (success or
 * failure). It may return < 0 to report an error and require an abort of the
 * connection being instanciated. It must be removed once done.
 */
struct connection {
	enum obj_type obj_type;       /* differentiates connection from applet context */
	unsigned char err_code;       /* CO_ER_* */
	signed short send_proxy_ofs;  /* <0 = offset to (re)send from the end, >0 = send all */
	unsigned int flags;           /* CO_FL_* */
	const struct protocol *ctrl;  /* operations at the socket layer */
	const struct xprt_ops *xprt;  /* operations at the transport layer */
	const struct mux_ops  *mux;   /* mux layer opreations. Must be set before xprt->init() */
	void *xprt_ctx;               /* general purpose pointer, initialized to NULL */
	void *mux_ctx;                /* mux-specific context, initialized to NULL */
	void *owner;                  /* pointer to the owner session for incoming connections, or NULL */
	int xprt_st;                  /* transport layer state, initialized to zero */
	int tmp_early_data;           /* 1st byte of early data, if any */
	int sent_early_data;          /* Amount of early data we sent so far */
	union conn_handle handle;     /* connection handle at the socket layer */
	enum obj_type *target;        /* the target to connect to (server, proxy, applet, ...) */
	struct list list;             /* attach point to various connection lists (idle, ...) */
	int (*xprt_done_cb)(struct connection *conn);  /* callback to notify of end of handshake */
	void (*destroy_cb)(struct connection *conn);  /* callback to notify of imminent death of the connection */
	const struct netns_entry *proxy_netns;
	struct {
		struct sockaddr_storage from;	/* client address, or address to spoof when connecting to the server */
		struct sockaddr_storage to;	/* address reached by the client, or address to connect to */
	} addr; /* addresses of the remote side, client for producer and server for consumer */
};

/* ALPN token registration */
enum alpn_proxy_mode {
	ALPN_MODE_NONE = 0,
	ALPN_MODE_TCP  = 1 << 0, // must not be changed!
	ALPN_MODE_HTTP = 1 << 1, // must not be changed!
	ALPN_MODE_ANY  = ALPN_MODE_TCP | ALPN_MODE_HTTP,
};

struct alpn_mux_list {
	const struct ist token;    /* token name and length. Empty is catch-all */
	enum alpn_proxy_mode mode;
	const struct mux_ops *mux;
	struct list list;
};

/* proxy protocol v2 definitions */
#define PP2_SIGNATURE        "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"
#define PP2_SIGNATURE_LEN    12
#define PP2_HEADER_LEN       16

/* ver_cmd byte */
#define PP2_CMD_LOCAL        0x00
#define PP2_CMD_PROXY        0x01
#define PP2_CMD_MASK         0x0F

#define PP2_VERSION          0x20
#define PP2_VERSION_MASK     0xF0

/* fam byte */
#define PP2_TRANS_UNSPEC     0x00
#define PP2_TRANS_STREAM     0x01
#define PP2_TRANS_DGRAM      0x02
#define PP2_TRANS_MASK       0x0F

#define PP2_FAM_UNSPEC       0x00
#define PP2_FAM_INET         0x10
#define PP2_FAM_INET6        0x20
#define PP2_FAM_UNIX         0x30
#define PP2_FAM_MASK         0xF0

#define PP2_ADDR_LEN_UNSPEC  (0)
#define PP2_ADDR_LEN_INET    (4 + 4 + 2 + 2)
#define PP2_ADDR_LEN_INET6   (16 + 16 + 2 + 2)
#define PP2_ADDR_LEN_UNIX    (108 + 108)

#define PP2_HDR_LEN_UNSPEC   (PP2_HEADER_LEN + PP2_ADDR_LEN_UNSPEC)
#define PP2_HDR_LEN_INET     (PP2_HEADER_LEN + PP2_ADDR_LEN_INET)
#define PP2_HDR_LEN_INET6    (PP2_HEADER_LEN + PP2_ADDR_LEN_INET6)
#define PP2_HDR_LEN_UNIX     (PP2_HEADER_LEN + PP2_ADDR_LEN_UNIX)

struct proxy_hdr_v2 {
	uint8_t sig[12];   /* hex 0D 0A 0D 0A 00 0D 0A 51 55 49 54 0A */
	uint8_t ver_cmd;   /* protocol version and command */
	uint8_t fam;       /* protocol family and transport */
	uint16_t len;      /* number of following bytes part of the header */
	union {
		struct {   /* for TCP/UDP over IPv4, len = 12 */
			uint32_t src_addr;
			uint32_t dst_addr;
			uint16_t src_port;
			uint16_t dst_port;
		} ip4;
		struct {   /* for TCP/UDP over IPv6, len = 36 */
			uint8_t  src_addr[16];
			uint8_t  dst_addr[16];
			uint16_t src_port;
			uint16_t dst_port;
		} ip6;
		struct {   /* for AF_UNIX sockets, len = 216 */
			uint8_t src_addr[108];
			uint8_t dst_addr[108];
		} unx;
	} addr;
};

#define PP2_TYPE_ALPN           0x01
#define PP2_TYPE_AUTHORITY      0x02
#define PP2_TYPE_CRC32C         0x03
#define PP2_TYPE_NOOP           0x04
#define PP2_TYPE_SSL            0x20
#define PP2_SUBTYPE_SSL_VERSION 0x21
#define PP2_SUBTYPE_SSL_CN      0x22
#define PP2_SUBTYPE_SSL_CIPHER  0x23
#define PP2_SUBTYPE_SSL_SIG_ALG 0x24
#define PP2_SUBTYPE_SSL_KEY_ALG 0x25
#define PP2_TYPE_NETNS          0x30

#define TLV_HEADER_SIZE      3
struct tlv {
	uint8_t type;
	uint8_t length_hi;
	uint8_t length_lo;
	uint8_t value[0];
}__attribute__((packed));

struct tlv_ssl {
	struct tlv tlv;
	uint8_t client;
	uint32_t verify;
	uint8_t sub_tlv[0];
}__attribute__((packed));

#define PP2_CLIENT_SSL           0x01
#define PP2_CLIENT_CERT_CONN     0x02
#define PP2_CLIENT_CERT_SESS     0x04


/*
 * Linux seems to be able to send 253 fds per sendmsg(), not sure
 * about the other OSes.
 */
/* Max number of file descriptors we send in one sendmsg() */
#define MAX_SEND_FD 253

#endif /* _TYPES_CONNECTION_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
