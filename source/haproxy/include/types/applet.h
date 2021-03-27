/*
 * include/types/applet.h
 * This file describes the applet struct and associated constants.
 *
 * Copyright (C) 2000-2015 Willy Tarreau - w@1wt.eu
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

#ifndef _TYPES_APPLET_H
#define _TYPES_APPLET_H

#include <types/hlua.h>
#include <types/obj_type.h>
#include <types/proxy.h>
#include <types/stream.h>
#include <common/buffer.h>
#include <common/chunk.h>
#include <common/config.h>
#include <common/xref.h>

struct appctx;

/* Applet descriptor */
struct applet {
	enum obj_type obj_type;            /* object type = OBJ_TYPE_APPLET */
	/* 3 unused bytes here */
	char *name;                        /* applet's name to report in logs */
	int (*init)(struct appctx *, struct proxy *px, struct stream *strm);   /* callback to init ressources, may be NULL.
	                                     expect 1 if ok, 0 if an error occurs, -1 if miss data. */
	void (*fct)(struct appctx *);      /* internal I/O handler, may never be NULL */
	void (*release)(struct appctx *);  /* callback to release resources, may be NULL */
	unsigned int timeout;              /* execution timeout. */
};

#define APPLET_SLEEPING     0x00  /* applet is currently sleeping or pending in active queue */
#define APPLET_RUNNING      0x01  /* applet is currently running */
#define APPLET_WOKEN_UP     0x02  /* applet was running and requested to woken up again */
#define APPLET_WANT_DIE     0x04  /* applet was running and requested to die */

/* Context of a running applet. */
struct appctx {
	struct list runq;          /* chaining in the applet run queue */
	enum obj_type obj_type;    /* OBJ_TYPE_APPCTX */
	/* 3 unused bytes here */
	unsigned short state;      /* Internal appctx state */
	unsigned int st0;          /* CLI state for stats, session state for peers */
	unsigned int st1;          /* prompt for stats, session error for peers */
	unsigned int st2;          /* output state for stats, unused by peers  */
	struct applet *applet;     /* applet this context refers to */
	void *owner;               /* pointer to upper layer's entity (eg: stream interface) */
	struct act_rule *rule;     /* rule associated with the applet. */
	int (*io_handler)(struct appctx *appctx);  /* used within the cli_io_handler when st0 = CLI_ST_CALLBACK */
	void (*io_release)(struct appctx *appctx);  /* used within the cli_io_handler when st0 = CLI_ST_CALLBACK,
	                                               if the command is terminated or the session released */
	int cli_severity_output;        /* used within the cli_io_handler to format severity output of informational feedback */
	struct buffer_wait buffer_wait; /* position in the list of objects waiting for a buffer */
	unsigned long thread_mask;      /* mask of thread IDs authorized to process the applet */

	union {
		struct {
			void *ptr;              /* current peer or NULL, do not use for something else */
		} peers;                        /* used by the peers applet */
		struct {
			int connected;
			struct xref xref; /* cross reference with the Lua object owner. */
			struct list wake_on_read;
			struct list wake_on_write;
			int die;
		} hlua_cosocket;                /* used by the Lua cosockets */
		struct {
			struct hlua *hlua;
			int flags;
			struct task *task;
		} hlua_apptcp;                  /* used by the Lua TCP services */
		struct {
			struct hlua *hlua;
			int left_bytes;         /* The max amount of bytes that we can read. */
			int flags;
			int status;
			const char *reason;
			struct task *task;
		} hlua_apphttp;                 /* used by the Lua HTTP services */
		struct {
			void *ptr;              /* private pointer for SPOE filter */
		} spoe;                         /* used by SPOE filter */
		struct {
			const char *msg;        /* pointer to a persistent message to be returned in CLI_ST_PRINT state */
			int severity;           /* severity of the message to be returned according to (syslog) rfc5424 */
			char *err;              /* pointer to a 'must free' message to be returned in CLI_ST_PRINT_FREE state */
			void *p0, *p1;          /* general purpose pointers and integers for registered commands, initialized */
			int i0, i1;             /* to 0 by the CLI before first invocation of the keyword parser. */
		} cli;                          /* context used by the CLI */
		struct {
			struct cache_entry *entry;
		} cache;
		/* all entries below are used by various CLI commands, please
		 * keep the grouped together and avoid adding new ones.
		 */
		struct {
			struct proxy *px;
			struct server *sv;
			void *l;
			int scope_str;		/* limit scope to a frontend/backend substring */
			int scope_len;		/* length of the string above in the buffer */
			int px_st;		/* STAT_PX_ST* */
			unsigned int flags;	/* STAT_* */
			int iid, type, sid;	/* proxy id, type and service id if bounding of stats is enabled */
			int st_code;		/* the status code returned by an action */
		} stats;
		struct {
			struct bref bref;	/* back-reference from the session being dumped */
			void *target;		/* session we want to dump, or NULL for all */
			unsigned int uid;	/* if non-null, the uniq_id of the session being dumped */
			int section;		/* section of the session being dumped */
			int pos;		/* last position of the current session's buffer */
		} sess;
		struct {
			int iid;		/* if >= 0, ID of the proxy to filter on */
			struct proxy *px;	/* current proxy being dumped, NULL = not started yet. */
			unsigned int flag;	/* bit0: buffer being dumped, 0 = req, 1 = resp ; bit1=skip req ; bit2=skip resp. */
			unsigned int sid;	/* session ID of error being dumped */
			int ptr;		/* <0: headers, >=0 : text pointer to restart from */
			int bol;		/* pointer to beginning of current line */
		} errors;
		struct {
			void *target;		/* table we want to dump, or NULL for all */
			struct proxy *proxy;	/* table being currently dumped (first if NULL) */
			struct stksess *entry;	/* last entry we were trying to dump (or first if NULL) */
			long long value;	/* value to compare against */
			signed char data_type;	/* type of data to compare, or -1 if none */
			signed char data_op;	/* operator (STD_OP_*) when data_type set */
			char action;            /* action on the table : one of STK_CLI_ACT_* */
		} table;
		struct {
			unsigned int display_flags;
			struct pat_ref *ref;
			struct bref bref;	/* back-reference from the pat_ref_elt being dumped */
			struct pattern_expr *expr;
			struct chunk chunk;
		} map;
		struct {
			struct hlua *hlua;
			struct task *task;
			struct hlua_function *fcn;
		} hlua_cli;
		/* NOTE: please add regular applet contexts (ie: not
		 * CLI-specific ones) above, before "cli".
		 */
	} ctx;					/* context-specific variables used by any applet */
};

#endif /* _TYPES_APPLET_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
