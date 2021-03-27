#ifndef _TYPES_VARS_H
#define _TYPES_VARS_H

#include <common/mini-clist.h>
#include <common/hathreads.h>

#include <types/sample.h>

enum vars_scope {
	SCOPE_SESS = 0,
	SCOPE_TXN,
	SCOPE_REQ,
	SCOPE_RES,
	SCOPE_PROC,
};

struct vars {
	struct list head;
	enum vars_scope scope;
	unsigned int size;
	__decl_hathreads(HA_RWLOCK_T rwlock);
};

/* This struct describes a variable. */
struct var_desc {
	const char *name; /* Contains the normalized variable name. */
	enum vars_scope scope;
};

struct var {
	struct list l; /* Used for chaining vars. */
	const char *name; /* Contains the variable name. */
	struct sample_data data; /* data storage. */
};

#endif
