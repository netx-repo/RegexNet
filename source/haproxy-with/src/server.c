/*
 * Server management functions.
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 * Copyright 2007-2008 Krzysztof Piotr Oledzki <ole@ans.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>

#include <import/xxhash.h>

#include <common/cfgparse.h>
#include <common/config.h>
#include <common/errors.h>
#include <common/namespace.h>
#include <common/time.h>

#include <types/applet.h>
#include <types/cli.h>
#include <types/global.h>
#include <types/cli.h>
#include <types/dns.h>
#include <types/stats.h>

#include <proto/applet.h>
#include <proto/cli.h>
#include <proto/checks.h>
#include <proto/port_range.h>
#include <proto/protocol.h>
#include <proto/queue.h>
#include <proto/sample.h>
#include <proto/server.h>
#include <proto/stream.h>
#include <proto/stream_interface.h>
#include <proto/stats.h>
#include <proto/task.h>
#include <proto/dns.h>
#include <netinet/tcp.h>

struct list updated_servers = LIST_HEAD_INIT(updated_servers);
__decl_hathreads(HA_SPINLOCK_T updated_servers_lock);

static void srv_register_update(struct server *srv);
static void srv_update_state(struct server *srv, int version, char **params);
static int srv_apply_lastaddr(struct server *srv, int *err_code);
static int srv_set_fqdn(struct server *srv, const char *fqdn, int dns_locked);

/* List head of all known server keywords */
static struct srv_kw_list srv_keywords = {
	.list = LIST_HEAD_INIT(srv_keywords.list)
};

int srv_downtime(const struct server *s)
{
	if ((s->cur_state != SRV_ST_STOPPED) && s->last_change < now.tv_sec)		// ignore negative time
		return s->down_time;

	return now.tv_sec - s->last_change + s->down_time;
}

int srv_lastsession(const struct server *s)
{
	if (s->counters.last_sess)
		return now.tv_sec - s->counters.last_sess;

	return -1;
}

int srv_getinter(const struct check *check)
{
	const struct server *s = check->server;

	if ((check->state & CHK_ST_CONFIGURED) && (check->health == check->rise + check->fall - 1))
		return check->inter;

	if ((s->next_state == SRV_ST_STOPPED) && check->health == 0)
		return (check->downinter)?(check->downinter):(check->inter);

	return (check->fastinter)?(check->fastinter):(check->inter);
}

/*
 * Check that we did not get a hash collision.
 * Unlikely, but it can happen.
 */
static inline void srv_check_for_dup_dyncookie(struct server *s)
{
	struct proxy *p = s->proxy;
	struct server *tmpserv;

	for (tmpserv = p->srv; tmpserv != NULL;
	    tmpserv = tmpserv->next) {
		if (tmpserv == s)
			continue;
		if (tmpserv->next_admin & SRV_ADMF_FMAINT)
			continue;
		if (tmpserv->cookie &&
		    strcmp(tmpserv->cookie, s->cookie) == 0) {
			ha_warning("We generated two equal cookies for two different servers.\n"
				   "Please change the secret key for '%s'.\n",
				   s->proxy->id);
		}
	}

}

void srv_set_dyncookie(struct server *s)
{
	struct proxy *p = s->proxy;
	char *tmpbuf;
	unsigned long long hash_value;
	size_t key_len;
	size_t buffer_len;
	int addr_len;
	int port;

	if ((s->flags & SRV_F_COOKIESET) ||
	    !(s->proxy->ck_opts & PR_CK_DYNAMIC) ||
	    s->proxy->dyncookie_key == NULL)
		return;
	key_len = strlen(p->dyncookie_key);

	if (s->addr.ss_family != AF_INET &&
	    s->addr.ss_family != AF_INET6)
		return;
	/*
	 * Buffer to calculate the cookie value.
	 * The buffer contains the secret key + the server IP address
	 * + the TCP port.
	 */
	addr_len = (s->addr.ss_family == AF_INET) ? 4 : 16;
	/*
	 * The TCP port should use only 2 bytes, but is stored in
	 * an unsigned int in struct server, so let's use 4, to be
	 * on the safe side.
	 */
	buffer_len = key_len + addr_len + 4;
	tmpbuf = trash.str;
	memcpy(tmpbuf, p->dyncookie_key, key_len);
	memcpy(&(tmpbuf[key_len]),
	    s->addr.ss_family == AF_INET ?
	    (void *)&((struct sockaddr_in *)&s->addr)->sin_addr.s_addr :
	    (void *)&(((struct sockaddr_in6 *)&s->addr)->sin6_addr.s6_addr),
	    addr_len);
	/*
	 * Make sure it's the same across all the load balancers,
	 * no matter their endianness.
	 */
	port = htonl(s->svc_port);
	memcpy(&tmpbuf[key_len + addr_len], &port, 4);
	hash_value = XXH64(tmpbuf, buffer_len, 0);
	memprintf(&s->cookie, "%016llx", hash_value);
	if (!s->cookie)
		return;
	s->cklen = 16;

	/* Don't bother checking if the dyncookie is duplicated if
	 * the server is marked as "disabled", maybe it doesn't have
	 * its real IP yet, but just a place holder.
	 */
	if (!(s->next_admin & SRV_ADMF_FMAINT))
		srv_check_for_dup_dyncookie(s);
}

/*
 * Registers the server keyword list <kwl> as a list of valid keywords for next
 * parsing sessions.
 */
void srv_register_keywords(struct srv_kw_list *kwl)
{
	LIST_ADDQ(&srv_keywords.list, &kwl->list);
}

/* Return a pointer to the server keyword <kw>, or NULL if not found. If the
 * keyword is found with a NULL ->parse() function, then an attempt is made to
 * find one with a valid ->parse() function. This way it is possible to declare
 * platform-dependant, known keywords as NULL, then only declare them as valid
 * if some options are met. Note that if the requested keyword contains an
 * opening parenthesis, everything from this point is ignored.
 */
struct srv_kw *srv_find_kw(const char *kw)
{
	int index;
	const char *kwend;
	struct srv_kw_list *kwl;
	struct srv_kw *ret = NULL;

	kwend = strchr(kw, '(');
	if (!kwend)
		kwend = kw + strlen(kw);

	list_for_each_entry(kwl, &srv_keywords.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if ((strncmp(kwl->kw[index].kw, kw, kwend - kw) == 0) &&
			    kwl->kw[index].kw[kwend-kw] == 0) {
				if (kwl->kw[index].parse)
					return &kwl->kw[index]; /* found it !*/
				else
					ret = &kwl->kw[index];  /* may be OK */
			}
		}
	}
	return ret;
}

/* Dumps all registered "server" keywords to the <out> string pointer. The
 * unsupported keywords are only dumped if their supported form was not
 * found.
 */
void srv_dump_kws(char **out)
{
	struct srv_kw_list *kwl;
	int index;

	*out = NULL;
	list_for_each_entry(kwl, &srv_keywords.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if (kwl->kw[index].parse ||
			    srv_find_kw(kwl->kw[index].kw) == &kwl->kw[index]) {
				memprintf(out, "%s[%4s] %s%s%s%s\n", *out ? *out : "",
				          kwl->scope,
				          kwl->kw[index].kw,
				          kwl->kw[index].skip ? " <arg>" : "",
				          kwl->kw[index].default_ok ? " [dflt_ok]" : "",
				          kwl->kw[index].parse ? "" : " (not supported)");
			}
		}
	}
}

/* Parse the "addr" server keyword */
static int srv_parse_addr(char **args, int *cur_arg,
                          struct proxy *curproxy, struct server *newsrv, char **err)
{
	char *errmsg, *arg;
	struct sockaddr_storage *sk;
	int port1, port2;
	struct protocol *proto;

	errmsg = NULL;
	arg = args[*cur_arg + 1];

	if (!*arg) {
		memprintf(err, "'%s' expects <ipv4|ipv6> as argument.\n", args[*cur_arg]);
		goto err;
	}

	sk = str2sa_range(arg, NULL, &port1, &port2, &errmsg, NULL, NULL, 1);
	if (!sk) {
		memprintf(err, "'%s' : %s", args[*cur_arg], errmsg);
		goto err;
	}

	proto = protocol_by_family(sk->ss_family);
	if (!proto || !proto->connect) {
		memprintf(err, "'%s %s' : connect() not supported for this address family.\n",
		          args[*cur_arg], arg);
		goto err;
	}

	if (port1 != port2) {
		memprintf(err, "'%s' : port ranges and offsets are not allowed in '%s'\n",
		          args[*cur_arg], arg);
		goto err;
	}

	newsrv->check.addr = newsrv->agent.addr = *sk;
	newsrv->flags |= SRV_F_CHECKADDR;
	newsrv->flags |= SRV_F_AGENTADDR;

	return 0;

 err:
	free(errmsg);
	return ERR_ALERT | ERR_FATAL;
}

/* Parse the "agent-check" server keyword */
static int srv_parse_agent_check(char **args, int *cur_arg,
                                 struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->do_agent = 1;
	return 0;
}

/* Parse the "backup" server keyword */
static int srv_parse_backup(char **args, int *cur_arg,
                            struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->flags |= SRV_F_BACKUP;
	return 0;
}

/* Parse the "check" server keyword */
static int srv_parse_check(char **args, int *cur_arg,
                           struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->do_check = 1;
	return 0;
}

/* Parse the "check-send-proxy" server keyword */
static int srv_parse_check_send_proxy(char **args, int *cur_arg,
                                      struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->check.send_proxy = 1;
	return 0;
}

/* Parse the "cookie" server keyword */
static int srv_parse_cookie(char **args, int *cur_arg,
                            struct proxy *curproxy, struct server *newsrv, char **err)
{
	char *arg;

	arg = args[*cur_arg + 1];
	if (!*arg) {
		memprintf(err, "'%s' expects <value> as argument.\n", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	free(newsrv->cookie);
	newsrv->cookie = strdup(arg);
	newsrv->cklen = strlen(arg);
	newsrv->flags |= SRV_F_COOKIESET;
	return 0;
}

/* Parse the "disabled" server keyword */
static int srv_parse_disabled(char **args, int *cur_arg,
                              struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->next_admin |= SRV_ADMF_CMAINT | SRV_ADMF_FMAINT;
	newsrv->next_state = SRV_ST_STOPPED;
	newsrv->check.state |= CHK_ST_PAUSED;
	newsrv->check.health = 0;
	return 0;
}

/* Parse the "enabled" server keyword */
static int srv_parse_enabled(char **args, int *cur_arg,
                             struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->next_admin &= ~SRV_ADMF_CMAINT & ~SRV_ADMF_FMAINT;
	newsrv->next_state = SRV_ST_RUNNING;
	newsrv->check.state &= ~CHK_ST_PAUSED;
	newsrv->check.health = newsrv->check.rise;
	return 0;
}

/* parse the "id" server keyword */
static int srv_parse_id(char **args, int *cur_arg, struct proxy *curproxy, struct server *newsrv, char **err)
{
	struct eb32_node *node;

	if (!*args[*cur_arg + 1]) {
		memprintf(err, "'%s' : expects an integer argument", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	newsrv->puid = atol(args[*cur_arg + 1]);
	newsrv->conf.id.key = newsrv->puid;

	if (newsrv->puid <= 0) {
		memprintf(err, "'%s' : custom id has to be > 0", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	node = eb32_lookup(&curproxy->conf.used_server_id, newsrv->puid);
	if (node) {
		struct server *target = container_of(node, struct server, conf.id);
		memprintf(err, "'%s' : custom id %d already used at %s:%d ('server %s')",
		          args[*cur_arg], newsrv->puid, target->conf.file, target->conf.line,
		          target->id);
		return ERR_ALERT | ERR_FATAL;
	}

	eb32_insert(&curproxy->conf.used_server_id, &newsrv->conf.id);
	newsrv->flags |= SRV_F_FORCED_ID;
	return 0;
}

/* Parse the "namespace" server keyword */
static int srv_parse_namespace(char **args, int *cur_arg,
                               struct proxy *curproxy, struct server *newsrv, char **err)
{
#ifdef CONFIG_HAP_NS
	char *arg;

	arg = args[*cur_arg + 1];
	if (!*arg) {
		memprintf(err, "'%s' : expects <name> as argument", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (!strcmp(arg, "*")) {
		/* Use the namespace associated with the connection (if present). */
		newsrv->flags |= SRV_F_USE_NS_FROM_PP;
		return 0;
	}

	/*
	 * As this parser may be called several times for the same 'default-server'
	 * object, or for a new 'server' instance deriving from a 'default-server'
	 * one with SRV_F_USE_NS_FROM_PP flag enabled, let's reset it.
	 */
	newsrv->flags &= ~SRV_F_USE_NS_FROM_PP;

	newsrv->netns = netns_store_lookup(arg, strlen(arg));
	if (!newsrv->netns)
		newsrv->netns = netns_store_insert(arg);

	if (!newsrv->netns) {
		memprintf(err, "Cannot open namespace '%s'", arg);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
#else
	memprintf(err, "'%s': '%s' option not implemented", args[0], args[*cur_arg]);
	return ERR_ALERT | ERR_FATAL;
#endif
}

/* Parse the "no-agent-check" server keyword */
static int srv_parse_no_agent_check(char **args, int *cur_arg,
                                     struct proxy *curproxy, struct server *newsrv, char **err)
{
	free_check(&newsrv->agent);
	newsrv->agent.inter = 0;
	newsrv->agent.port = 0;
	newsrv->agent.state &= ~CHK_ST_CONFIGURED & ~CHK_ST_ENABLED & ~CHK_ST_AGENT;
	newsrv->do_agent = 0;
	return 0;
}

/* Parse the "no-backup" server keyword */
static int srv_parse_no_backup(char **args, int *cur_arg,
                               struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->flags &= ~SRV_F_BACKUP;
	return 0;
}

/* Parse the "no-check" server keyword */
static int srv_parse_no_check(char **args, int *cur_arg,
                              struct proxy *curproxy, struct server *newsrv, char **err)
{
	free_check(&newsrv->check);
	newsrv->check.state &= ~CHK_ST_CONFIGURED & ~CHK_ST_ENABLED;
	newsrv->do_check = 0;
	return 0;
}

/* Parse the "no-check-send-proxy" server keyword */
static int srv_parse_no_check_send_proxy(char **args, int *cur_arg,
                                         struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->check.send_proxy = 0;
	return 0;
}

/* Disable server PROXY protocol flags. */
static int inline srv_disable_pp_flags(struct server *srv, unsigned int flags)
{
	srv->pp_opts &= ~flags;
	return 0;
}

/* Parse the "no-send-proxy" server keyword */
static int srv_parse_no_send_proxy(char **args, int *cur_arg,
                                   struct proxy *curproxy, struct server *newsrv, char **err)
{
	return srv_disable_pp_flags(newsrv, SRV_PP_V1);
}

/* Parse the "no-send-proxy-v2" server keyword */
static int srv_parse_no_send_proxy_v2(char **args, int *cur_arg,
                                      struct proxy *curproxy, struct server *newsrv, char **err)
{
	return srv_disable_pp_flags(newsrv, SRV_PP_V2);
}

/* Parse the "non-stick" server keyword */
static int srv_parse_non_stick(char **args, int *cur_arg,
                               struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->flags |= SRV_F_NON_STICK;
	return 0;
}

/* Enable server PROXY protocol flags. */
static int inline srv_enable_pp_flags(struct server *srv, unsigned int flags)
{
	srv->pp_opts |= flags;
	return 0;
}

/* Parse the "observe" server keyword */
static int srv_parse_observe(char **args, int *cur_arg,
                             struct proxy *curproxy, struct server *newsrv, char **err)
{
	char *arg;

	arg = args[*cur_arg + 1];
	if (!*arg) {
		memprintf(err, "'%s' expects <mode> as argument.\n", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (!strcmp(arg, "none")) {
		newsrv->observe = HANA_OBS_NONE;
	}
	else if (!strcmp(arg, "layer4")) {
		newsrv->observe = HANA_OBS_LAYER4;
	}
	else if (!strcmp(arg, "layer7")) {
		if (curproxy->mode != PR_MODE_HTTP) {
			memprintf(err, "'%s' can only be used in http proxies.\n", arg);
			return ERR_ALERT;
		}
		newsrv->observe = HANA_OBS_LAYER7;
	}
	else {
		memprintf(err, "'%s' expects one of 'none', 'layer4', 'layer7' "
		               "but got '%s'\n", args[*cur_arg], arg);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

/* Parse the "redir" server keyword */
static int srv_parse_redir(char **args, int *cur_arg,
                           struct proxy *curproxy, struct server *newsrv, char **err)
{
	char *arg;

	arg = args[*cur_arg + 1];
	if (!*arg) {
		memprintf(err, "'%s' expects <prefix> as argument.\n", args[*cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	free(newsrv->rdr_pfx);
	newsrv->rdr_pfx = strdup(arg);
	newsrv->rdr_len = strlen(arg);

	return 0;
}

/* Parse the "send-proxy" server keyword */
static int srv_parse_send_proxy(char **args, int *cur_arg,
                                struct proxy *curproxy, struct server *newsrv, char **err)
{
	return srv_enable_pp_flags(newsrv, SRV_PP_V1);
}

/* Parse the "send-proxy-v2" server keyword */
static int srv_parse_send_proxy_v2(char **args, int *cur_arg,
                                   struct proxy *curproxy, struct server *newsrv, char **err)
{
	return srv_enable_pp_flags(newsrv, SRV_PP_V2);
}


/* Parse the "source" server keyword */
static int srv_parse_source(char **args, int *cur_arg,
                            struct proxy *curproxy, struct server *newsrv, char **err)
{
	char *errmsg;
	int port_low, port_high;
	struct sockaddr_storage *sk;
	struct protocol *proto;

	errmsg = NULL;

	if (!*args[*cur_arg + 1]) {
		memprintf(err, "'%s' expects <addr>[:<port>[-<port>]], and optionally '%s' <addr>, "
		               "and '%s' <name> as argument.\n", args[*cur_arg], "usesrc", "interface");
		goto err;
	}

	/* 'sk' is statically allocated (no need to be freed). */
	sk = str2sa_range(args[*cur_arg + 1], NULL, &port_low, &port_high, &errmsg, NULL, NULL, 1);
	if (!sk) {
		memprintf(err, "'%s %s' : %s\n", args[*cur_arg], args[*cur_arg + 1], errmsg);
		goto err;
	}

	proto = protocol_by_family(sk->ss_family);
	if (!proto || !proto->connect) {
		ha_alert("'%s %s' : connect() not supported for this address family.\n",
			 args[*cur_arg], args[*cur_arg + 1]);
		goto err;
	}

	newsrv->conn_src.opts |= CO_SRC_BIND;
	newsrv->conn_src.source_addr = *sk;

	if (port_low != port_high) {
		int i;

		if (!port_low || !port_high) {
			ha_alert("'%s' does not support port offsets (found '%s').\n",
				 args[*cur_arg], args[*cur_arg + 1]);
			goto err;
		}

		if (port_low  <= 0 || port_low  > 65535 ||
			port_high <= 0 || port_high > 65535 ||
			port_low > port_high) {
			ha_alert("'%s': invalid source port range %d-%d.\n", args[*cur_arg], port_low, port_high);
			goto err;
		}
		newsrv->conn_src.sport_range = port_range_alloc_range(port_high - port_low + 1);
		for (i = 0; i < newsrv->conn_src.sport_range->size; i++)
			newsrv->conn_src.sport_range->ports[i] = port_low + i;
	}

	*cur_arg += 2;
	while (*(args[*cur_arg])) {
		if (!strcmp(args[*cur_arg], "usesrc")) {  /* address to use outside */
#if defined(CONFIG_HAP_TRANSPARENT)
			if (!*args[*cur_arg + 1]) {
				ha_alert("'usesrc' expects <addr>[:<port>], 'client', 'clientip', "
					 "or 'hdr_ip(name,#)' as argument.\n");
				goto err;
			}
			if (!strcmp(args[*cur_arg + 1], "client")) {
				newsrv->conn_src.opts &= ~CO_SRC_TPROXY_MASK;
				newsrv->conn_src.opts |= CO_SRC_TPROXY_CLI;
			}
			else if (!strcmp(args[*cur_arg + 1], "clientip")) {
				newsrv->conn_src.opts &= ~CO_SRC_TPROXY_MASK;
				newsrv->conn_src.opts |= CO_SRC_TPROXY_CIP;
			}
			else if (!strncmp(args[*cur_arg + 1], "hdr_ip(", 7)) {
				char *name, *end;

				name = args[*cur_arg + 1] + 7;
				while (isspace(*name))
					name++;

				end = name;
				while (*end && !isspace(*end) && *end != ',' && *end != ')')
					end++;

				newsrv->conn_src.opts &= ~CO_SRC_TPROXY_MASK;
				newsrv->conn_src.opts |= CO_SRC_TPROXY_DYN;
				free(newsrv->conn_src.bind_hdr_name);
				newsrv->conn_src.bind_hdr_name = calloc(1, end - name + 1);
				newsrv->conn_src.bind_hdr_len = end - name;
				memcpy(newsrv->conn_src.bind_hdr_name, name, end - name);
				newsrv->conn_src.bind_hdr_name[end - name] = '\0';
				newsrv->conn_src.bind_hdr_occ = -1;

				/* now look for an occurrence number */
				while (isspace(*end))
					end++;
				if (*end == ',') {
					end++;
					name = end;
					if (*end == '-')
						end++;
					while (isdigit((int)*end))
						end++;
					newsrv->conn_src.bind_hdr_occ = strl2ic(name, end - name);
				}

				if (newsrv->conn_src.bind_hdr_occ < -MAX_HDR_HISTORY) {
					ha_alert("usesrc hdr_ip(name,num) does not support negative"
						 " occurrences values smaller than %d.\n", MAX_HDR_HISTORY);
					goto err;
				}
			}
			else {
				struct sockaddr_storage *sk;
				int port1, port2;

				/* 'sk' is statically allocated (no need to be freed). */
				sk = str2sa_range(args[*cur_arg + 1], NULL, &port1, &port2, &errmsg, NULL, NULL, 1);
				if (!sk) {
					ha_alert("'%s %s' : %s\n", args[*cur_arg], args[*cur_arg + 1], errmsg);
					goto err;
				}

				proto = protocol_by_family(sk->ss_family);
				if (!proto || !proto->connect) {
					ha_alert("'%s %s' : connect() not supported for this address family.\n",
						 args[*cur_arg], args[*cur_arg + 1]);
					goto err;
				}

				if (port1 != port2) {
					ha_alert("'%s' : port ranges and offsets are not allowed in '%s'\n",
						 args[*cur_arg], args[*cur_arg + 1]);
					goto err;
				}
				newsrv->conn_src.tproxy_addr = *sk;
				newsrv->conn_src.opts |= CO_SRC_TPROXY_ADDR;
			}
			global.last_checks |= LSTCHK_NETADM;
			*cur_arg += 2;
			continue;
#else	/* no TPROXY support */
			ha_alert("'usesrc' not allowed here because support for TPROXY was not compiled in.\n");
			goto err;
#endif /* defined(CONFIG_HAP_TRANSPARENT) */
		} /* "usesrc" */

		if (!strcmp(args[*cur_arg], "interface")) { /* specifically bind to this interface */
#ifdef SO_BINDTODEVICE
			if (!*args[*cur_arg + 1]) {
				ha_alert("'%s' : missing interface name.\n", args[0]);
				goto err;
			}
			free(newsrv->conn_src.iface_name);
			newsrv->conn_src.iface_name = strdup(args[*cur_arg + 1]);
			newsrv->conn_src.iface_len  = strlen(newsrv->conn_src.iface_name);
			global.last_checks |= LSTCHK_NETADM;
#else
			ha_alert("'%s' : '%s' option not implemented.\n", args[0], args[*cur_arg]);
			goto err;
#endif
			*cur_arg += 2;
			continue;
		}
		/* this keyword in not an option of "source" */
		break;
	} /* while */

	return 0;

 err:
	free(errmsg);
	return ERR_ALERT | ERR_FATAL;
}

/* Parse the "stick" server keyword */
static int srv_parse_stick(char **args, int *cur_arg,
                           struct proxy *curproxy, struct server *newsrv, char **err)
{
	newsrv->flags &= ~SRV_F_NON_STICK;
	return 0;
}

/* Parse the "track" server keyword */
static int srv_parse_track(char **args, int *cur_arg,
                           struct proxy *curproxy, struct server *newsrv, char **err)
{
	char *arg;

	arg = args[*cur_arg + 1];
	if (!*arg) {
		memprintf(err, "'track' expects [<proxy>/]<server> as argument.\n");
		return ERR_ALERT | ERR_FATAL;
	}

	free(newsrv->trackit);
	newsrv->trackit = strdup(arg);

	return 0;
}


/* Shutdown all connections of a server. The caller must pass a termination
 * code in <why>, which must be one of SF_ERR_* indicating the reason for the
 * shutdown.
 */
void srv_shutdown_streams(struct server *srv, int why)
{
	struct stream *stream, *stream_bck;

	list_for_each_entry_safe(stream, stream_bck, &srv->actconns, by_srv)
		if (stream->srv_conn == srv)
			stream_shutdown(stream, why);
}

/* Shutdown all connections of all backup servers of a proxy. The caller must
 * pass a termination code in <why>, which must be one of SF_ERR_* indicating
 * the reason for the shutdown.
 */
void srv_shutdown_backup_streams(struct proxy *px, int why)
{
	struct server *srv;

	for (srv = px->srv; srv != NULL; srv = srv->next)
		if (srv->flags & SRV_F_BACKUP)
			srv_shutdown_streams(srv, why);
}

/* Appends some information to a message string related to a server going UP or
 * DOWN.  If both <forced> and <reason> are null and the server tracks another
 * one, a "via" information will be provided to know where the status came from.
 * If <check> is non-null, an entire string describing the check result will be
 * appended after a comma and a space (eg: to report some information from the
 * check that changed the state). In the other case, the string will be built
 * using the check results stored into the struct server if present.
 * If <xferred> is non-negative, some information about requeued sessions are
 * provided.
 */
void srv_append_status(struct chunk *msg, struct server *s, struct check *check, int xferred, int forced)
{
	short status = s->op_st_chg.status;
	short code = s->op_st_chg.code;
	long duration = s->op_st_chg.duration;
	char *desc = s->op_st_chg.reason;

	if (check) {
		status = check->status;
		code = check->code;
		duration = check->duration;
		desc = check->desc;
	}

	if (status != -1) {
		chunk_appendf(msg, ", reason: %s", get_check_status_description(status));

		if (status >= HCHK_STATUS_L57DATA)
			chunk_appendf(msg, ", code: %d", code);

		if (desc && *desc) {
			struct chunk src;

			chunk_appendf(msg, ", info: \"");

			chunk_initlen(&src, desc, 0, strlen(desc));
			chunk_asciiencode(msg, &src, '"');

			chunk_appendf(msg, "\"");
		}

		if (duration >= 0)
			chunk_appendf(msg, ", check duration: %ldms", duration);
	}
        else if (desc && *desc) {
                chunk_appendf(msg, ", %s", desc);
        }
	else if (!forced && s->track) {
		chunk_appendf(msg, " via %s/%s", s->track->proxy->id, s->track->id);
	}

	if (xferred >= 0) {
		if (s->next_state == SRV_ST_STOPPED)
			chunk_appendf(msg, ". %d active and %d backup servers left.%s"
				" %d sessions active, %d requeued, %d remaining in queue",
				s->proxy->srv_act, s->proxy->srv_bck,
				(s->proxy->srv_bck && !s->proxy->srv_act) ? " Running on backup." : "",
				s->cur_sess, xferred, s->nbpend);
		else
			chunk_appendf(msg, ". %d active and %d backup servers online.%s"
				" %d sessions requeued, %d total in queue",
				s->proxy->srv_act, s->proxy->srv_bck,
				(s->proxy->srv_bck && !s->proxy->srv_act) ? " Running on backup." : "",
				xferred, s->nbpend);
	}
}

/* Marks server <s> down, regardless of its checks' statuses. The server is
 * registered in a list to postpone the counting of the remaining servers on
 * the proxy and transfers queued streams whenever possible to other servers at
 * a sync point. Maintenance servers are ignored. It stores the <reason> if
 * non-null as the reason for going down or the available data from the check
 * struct to recompute this reason later.
 */
void srv_set_stopped(struct server *s, const char *reason, struct check *check)
{
	struct server *srv;

	if ((s->cur_admin & SRV_ADMF_MAINT) || s->next_state == SRV_ST_STOPPED)
		return;

	s->next_state = SRV_ST_STOPPED;
	*s->op_st_chg.reason = 0;
	s->op_st_chg.status = -1;
	if (reason) {
		strlcpy2(s->op_st_chg.reason, reason, sizeof(s->op_st_chg.reason));
	}
	else if (check) {
		strlcpy2(s->op_st_chg.reason, check->desc, sizeof(s->op_st_chg.reason));
		s->op_st_chg.code = check->code;
		s->op_st_chg.status = check->status;
		s->op_st_chg.duration = check->duration;
	}

	srv_register_update(s);
	for (srv = s->trackers; srv; srv = srv->tracknext) {
		HA_SPIN_LOCK(SERVER_LOCK, &srv->lock);
		srv_set_stopped(srv, NULL, NULL);
		HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
	}
}

/* Marks server <s> up regardless of its checks' statuses and provided it isn't
 * in maintenance. The server is registered in a list to postpone the counting
 * of the remaining servers on the proxy and tries to grab requests from the
 * proxy at a sync point. Maintenance servers are ignored. It stores the
 * <reason> if non-null as the reason for going down or the available data
 * from the check struct to recompute this reason later.
 */
void srv_set_running(struct server *s, const char *reason, struct check *check)
{
	struct server *srv;

	if (s->cur_admin & SRV_ADMF_MAINT)
		return;

	if (s->next_state == SRV_ST_STARTING || s->next_state == SRV_ST_RUNNING)
		return;

	s->next_state = SRV_ST_STARTING;
	*s->op_st_chg.reason = 0;
	s->op_st_chg.status = -1;
	if (reason) {
		strlcpy2(s->op_st_chg.reason, reason, sizeof(s->op_st_chg.reason));
	}
	else if (check) {
		strlcpy2(s->op_st_chg.reason, check->desc, sizeof(s->op_st_chg.reason));
		s->op_st_chg.code = check->code;
		s->op_st_chg.status = check->status;
		s->op_st_chg.duration = check->duration;
	}

	if (s->slowstart <= 0)
		s->next_state = SRV_ST_RUNNING;

	srv_register_update(s);
	for (srv = s->trackers; srv; srv = srv->tracknext) {
		HA_SPIN_LOCK(SERVER_LOCK, &srv->lock);
		srv_set_running(srv, NULL, NULL);
		HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
	}
}

/* Marks server <s> stopping regardless of its checks' statuses and provided it
 * isn't in maintenance. The server is registered in a list to postpone the
 * counting of the remaining servers on the proxy and tries to grab requests
 * from the proxy. Maintenance servers are ignored. It stores the
 * <reason> if non-null as the reason for going down or the available data
 * from the check struct to recompute this reason later.
 * up. Note that it makes use of the trash to build the log strings, so <reason>
 * must not be placed there.
 */
void srv_set_stopping(struct server *s, const char *reason, struct check *check)
{
	struct server *srv;

	if (s->cur_admin & SRV_ADMF_MAINT)
		return;

	if (s->next_state == SRV_ST_STOPPING)
		return;

	s->next_state = SRV_ST_STOPPING;
	*s->op_st_chg.reason = 0;
	s->op_st_chg.status = -1;
	if (reason) {
		strlcpy2(s->op_st_chg.reason, reason, sizeof(s->op_st_chg.reason));
	}
	else if (check) {
		strlcpy2(s->op_st_chg.reason, check->desc, sizeof(s->op_st_chg.reason));
		s->op_st_chg.code = check->code;
		s->op_st_chg.status = check->status;
		s->op_st_chg.duration = check->duration;
	}

	srv_register_update(s);
	for (srv = s->trackers; srv; srv = srv->tracknext) {
		HA_SPIN_LOCK(SERVER_LOCK, &srv->lock);
		srv_set_stopping(srv, NULL, NULL);
		HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
	}
}

/* Enables admin flag <mode> (among SRV_ADMF_*) on server <s>. This is used to
 * enforce either maint mode or drain mode. It is not allowed to set more than
 * one flag at once. The equivalent "inherited" flag is propagated to all
 * tracking servers. Maintenance mode disables health checks (but not agent
 * checks). When either the flag is already set or no flag is passed, nothing
 * is done. If <cause> is non-null, it will be displayed at the end of the log
 * lines to justify the state change.
 */
void srv_set_admin_flag(struct server *s, enum srv_admin mode, const char *cause)
{
	struct server *srv;

	if (!mode)
		return;

	/* stop going down as soon as we meet a server already in the same state */
	if (s->next_admin & mode)
		return;

	s->next_admin |= mode;
	if (cause)
		strlcpy2(s->adm_st_chg_cause, cause, sizeof(s->adm_st_chg_cause));

	srv_register_update(s);

	/* stop going down if the equivalent flag was already present (forced or inherited) */
	if (((mode & SRV_ADMF_MAINT) && (s->next_admin & ~mode & SRV_ADMF_MAINT)) ||
	    ((mode & SRV_ADMF_DRAIN) && (s->next_admin & ~mode & SRV_ADMF_DRAIN)))
		return;

	/* compute the inherited flag to propagate */
	if (mode & SRV_ADMF_MAINT)
		mode = SRV_ADMF_IMAINT;
	else if (mode & SRV_ADMF_DRAIN)
		mode = SRV_ADMF_IDRAIN;

	for (srv = s->trackers; srv; srv = srv->tracknext) {
		HA_SPIN_LOCK(SERVER_LOCK, &srv->lock);
		srv_set_admin_flag(srv, mode, cause);
		HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
	}
}

/* Disables admin flag <mode> (among SRV_ADMF_*) on server <s>. This is used to
 * stop enforcing either maint mode or drain mode. It is not allowed to set more
 * than one flag at once. The equivalent "inherited" flag is propagated to all
 * tracking servers. Leaving maintenance mode re-enables health checks. When
 * either the flag is already cleared or no flag is passed, nothing is done.
 */
void srv_clr_admin_flag(struct server *s, enum srv_admin mode)
{
	struct server *srv;

	if (!mode)
		return;

	/* stop going down as soon as we see the flag is not there anymore */
	if (!(s->next_admin & mode))
		return;

	s->next_admin &= ~mode;

	srv_register_update(s);

	/* stop going down if the equivalent flag is still present (forced or inherited) */
	if (((mode & SRV_ADMF_MAINT) && (s->next_admin & SRV_ADMF_MAINT)) ||
	    ((mode & SRV_ADMF_DRAIN) && (s->next_admin & SRV_ADMF_DRAIN)))
		return;

	if (mode & SRV_ADMF_MAINT)
		mode = SRV_ADMF_IMAINT;
	else if (mode & SRV_ADMF_DRAIN)
		mode = SRV_ADMF_IDRAIN;

	for (srv = s->trackers; srv; srv = srv->tracknext) {
		HA_SPIN_LOCK(SERVER_LOCK, &srv->lock);
		srv_clr_admin_flag(srv, mode);
		HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
	}
}

/* principle: propagate maint and drain to tracking servers. This is useful
 * upon startup so that inherited states are correct.
 */
static void srv_propagate_admin_state(struct server *srv)
{
	struct server *srv2;

	if (!srv->trackers)
		return;

	for (srv2 = srv->trackers; srv2; srv2 = srv2->tracknext) {
		HA_SPIN_LOCK(SERVER_LOCK, &srv2->lock);
		if (srv->next_admin & (SRV_ADMF_MAINT | SRV_ADMF_CMAINT))
			srv_set_admin_flag(srv2, SRV_ADMF_IMAINT, NULL);

		if (srv->next_admin & SRV_ADMF_DRAIN)
			srv_set_admin_flag(srv2, SRV_ADMF_IDRAIN, NULL);
		HA_SPIN_UNLOCK(SERVER_LOCK, &srv2->lock);
	}
}

/* Compute and propagate the admin states for all servers in proxy <px>.
 * Only servers *not* tracking another one are considered, because other
 * ones will be handled when the server they track is visited.
 */
void srv_compute_all_admin_states(struct proxy *px)
{
	struct server *srv;

	for (srv = px->srv; srv; srv = srv->next) {
		if (srv->track)
			continue;
		srv_propagate_admin_state(srv);
	}
}

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted, doing so helps
 * all code contributors.
 * Optional keywords are also declared with a NULL ->parse() function so that
 * the config parser can report an appropriate error when a known keyword was
 * not enabled.
 * Note: -1 as ->skip value means that the number of arguments are variable.
 */
static struct srv_kw_list srv_kws = { "ALL", { }, {
	{ "addr",                srv_parse_addr,                1,  1 }, /* IP address to send health to or to probe from agent-check */
	{ "agent-check",         srv_parse_agent_check,         0,  1 }, /* Enable an auxiliary agent check */
	{ "backup",              srv_parse_backup,              0,  1 }, /* Flag as backup server */
	{ "check",               srv_parse_check,               0,  1 }, /* enable health checks */
	{ "check-send-proxy",    srv_parse_check_send_proxy,    0,  1 }, /* enable PROXY protocol for health checks */
	{ "cookie",              srv_parse_cookie,              1,  1 }, /* Assign a cookie to the server */
	{ "disabled",            srv_parse_disabled,            0,  1 }, /* Start the server in 'disabled' state */
	{ "enabled",             srv_parse_enabled,             0,  1 }, /* Start the server in 'enabled' state */
	{ "id",                  srv_parse_id,                  1,  0 }, /* set id# of server */
	{ "namespace",           srv_parse_namespace,           1,  1 }, /* Namespace the server socket belongs to (if supported) */
	{ "no-agent-check",      srv_parse_no_agent_check,      0,  1 }, /* Do not enable any auxiliary agent check */
	{ "no-backup",           srv_parse_no_backup,           0,  1 }, /* Flag as non-backup server */
	{ "no-check",            srv_parse_no_check,            0,  1 }, /* disable health checks */
	{ "no-check-send-proxy", srv_parse_no_check_send_proxy, 0,  1 }, /* disable PROXY protol for health checks */
	{ "no-send-proxy",       srv_parse_no_send_proxy,       0,  1 }, /* Disable use of PROXY V1 protocol */
	{ "no-send-proxy-v2",    srv_parse_no_send_proxy_v2,    0,  1 }, /* Disable use of PROXY V2 protocol */
	{ "non-stick",           srv_parse_non_stick,           0,  1 }, /* Disable stick-table persistence */
	{ "observe",             srv_parse_observe,             1,  1 }, /* Enables health adjusting based on observing communication with the server */
	{ "redir",               srv_parse_redir,               1,  1 }, /* Enable redirection mode */
	{ "send-proxy",          srv_parse_send_proxy,          0,  1 }, /* Enforce use of PROXY V1 protocol */
	{ "send-proxy-v2",       srv_parse_send_proxy_v2,       0,  1 }, /* Enforce use of PROXY V2 protocol */
	{ "source",              srv_parse_source,             -1,  1 }, /* Set the source address to be used to connect to the server */
	{ "stick",               srv_parse_stick,               0,  1 }, /* Enable stick-table persistence */
	{ "track",               srv_parse_track,               1,  1 }, /* Set the current state of the server, tracking another one */
	{ NULL, NULL, 0 },
}};

__attribute__((constructor))
static void __listener_init(void)
{
	srv_register_keywords(&srv_kws);
}

/* Recomputes the server's eweight based on its state, uweight, the current time,
 * and the proxy's algorihtm. To be used after updating sv->uweight. The warmup
 * state is automatically disabled if the time is elapsed.
 */
void server_recalc_eweight(struct server *sv)
{
	struct proxy *px = sv->proxy;
	unsigned w;

	if (now.tv_sec < sv->last_change || now.tv_sec >= sv->last_change + sv->slowstart) {
		/* go to full throttle if the slowstart interval is reached */
		if (sv->next_state == SRV_ST_STARTING)
			sv->next_state = SRV_ST_RUNNING;
	}

	/* We must take care of not pushing the server to full throttle during slow starts.
	 * It must also start immediately, at least at the minimal step when leaving maintenance.
	 */
	if ((sv->next_state == SRV_ST_STARTING) && (px->lbprm.algo & BE_LB_PROP_DYN))
		w = (px->lbprm.wdiv * (now.tv_sec - sv->last_change) + sv->slowstart) / sv->slowstart;
	else
		w = px->lbprm.wdiv;

	sv->next_eweight = (sv->uweight * w + px->lbprm.wmult - 1) / px->lbprm.wmult;

	srv_register_update(sv);
}

/*
 * Parses weight_str and configures sv accordingly.
 * Returns NULL on success, error message string otherwise.
 */
const char *server_parse_weight_change_request(struct server *sv,
					       const char *weight_str)
{
	struct proxy *px;
	long int w;
	char *end;

	px = sv->proxy;

	/* if the weight is terminated with '%', it is set relative to
	 * the initial weight, otherwise it is absolute.
	 */
	if (!*weight_str)
		return "Require <weight> or <weight%>.\n";

	w = strtol(weight_str, &end, 10);
	if (end == weight_str)
		return "Empty weight string empty or preceded by garbage";
	else if (end[0] == '%' && end[1] == '\0') {
		if (w < 0)
			return "Relative weight must be positive.\n";
		/* Avoid integer overflow */
		if (w > 25600)
			w = 25600;
		w = sv->iweight * w / 100;
		if (w > 256)
			w = 256;
	}
	else if (w < 0 || w > 256)
		return "Absolute weight can only be between 0 and 256 inclusive.\n";
	else if (end[0] != '\0')
		return "Trailing garbage in weight string";

	if (w && w != sv->iweight && !(px->lbprm.algo & BE_LB_PROP_DYN))
		return "Backend is using a static LB algorithm and only accepts weights '0%' and '100%'.\n";

	sv->uweight = w;
	server_recalc_eweight(sv);

	return NULL;
}

/*
 * Parses <addr_str> and configures <sv> accordingly. <from> precise
 * the source of the change in the associated message log.
 * Returns:
 *  - error string on error
 *  - NULL on success
 */
const char *server_parse_addr_change_request(struct server *sv,
                                             const char *addr_str, const char *updater)
{
	unsigned char ip[INET6_ADDRSTRLEN];

	if (inet_pton(AF_INET6, addr_str, ip)) {
		update_server_addr(sv, ip, AF_INET6, updater);
		return NULL;
	}
	if (inet_pton(AF_INET, addr_str, ip)) {
		update_server_addr(sv, ip, AF_INET, updater);
		return NULL;
	}

	return "Could not understand IP address format.\n";
}

const char *server_parse_maxconn_change_request(struct server *sv,
                                                const char *maxconn_str)
{
	long int v;
	char *end;

	if (!*maxconn_str)
		return "Require <maxconn>.\n";

	v = strtol(maxconn_str, &end, 10);
	if (end == maxconn_str)
		return "maxconn string empty or preceded by garbage";
	else if (end[0] != '\0')
		return "Trailing garbage in maxconn string";

	if (sv->maxconn == sv->minconn) { // static maxconn
		sv->maxconn = sv->minconn = v;
	} else { // dynamic maxconn
		sv->maxconn = v;
	}

	if (may_dequeue_tasks(sv, sv->proxy))
		process_srv_queue(sv);

	return NULL;
}

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
static struct sample_expr *srv_sni_sample_parse_expr(struct server *srv, struct proxy *px,
                                                     const char *file, int linenum, char **err)
{
	int idx;
	const char *args[] = {
		srv->sni_expr,
		NULL,
	};

	idx = 0;
	px->conf.args.ctx = ARGC_SRV;

	return sample_parse_expr((char **)args, &idx, file, linenum, err, &px->conf.args);
}

static int server_parse_sni_expr(struct server *newsrv, struct proxy *px, char **err)
{
	struct sample_expr *expr;

	expr = srv_sni_sample_parse_expr(newsrv, px, px->conf.file, px->conf.line, err);
	if (!expr) {
		memprintf(err, "error detected while parsing sni expression : %s", *err);
		return ERR_ALERT | ERR_FATAL;
	}

	if (!(expr->fetch->val & SMP_VAL_BE_SRV_CON)) {
		memprintf(err, "error detected while parsing sni expression : "
		          " fetch method '%s' extracts information from '%s', "
		          "none of which is available here.\n",
		          newsrv->sni_expr, sample_src_names(expr->fetch->use));
		return ERR_ALERT | ERR_FATAL;
	}

	px->http_needed |= !!(expr->fetch->use & SMP_USE_HTTP_ANY);
	release_sample_expr(newsrv->ssl_ctx.sni);
	newsrv->ssl_ctx.sni = expr;

	return 0;
}
#endif

static void display_parser_err(const char *file, int linenum, char **args, int cur_arg, char **err)
{
	if (err && *err) {
		indent_msg(err, 2);
		ha_alert("parsing [%s:%d] : '%s %s' : %s\n", file, linenum, args[0], args[1], *err);
	}
	else
		ha_alert("parsing [%s:%d] : '%s %s' : error encountered while processing '%s'.\n",
			 file, linenum, args[0], args[1], args[cur_arg]);
}

static void srv_conn_src_sport_range_cpy(struct server *srv,
                                            struct server *src)
{
	int range_sz;

	range_sz = src->conn_src.sport_range->size;
	if (range_sz > 0) {
		srv->conn_src.sport_range = port_range_alloc_range(range_sz);
		if (srv->conn_src.sport_range != NULL) {
			int i;

			for (i = 0; i < range_sz; i++) {
				srv->conn_src.sport_range->ports[i] =
					src->conn_src.sport_range->ports[i];
			}
		}
	}
}

/*
 * Copy <src> server connection source settings to <srv> server everything needed.
 */
static void srv_conn_src_cpy(struct server *srv, struct server *src)
{
	srv->conn_src.opts = src->conn_src.opts;
	srv->conn_src.source_addr = src->conn_src.source_addr;

	/* Source port range copy. */
	if (src->conn_src.sport_range != NULL)
		srv_conn_src_sport_range_cpy(srv, src);

#ifdef CONFIG_HAP_TRANSPARENT
	if (src->conn_src.bind_hdr_name != NULL) {
		srv->conn_src.bind_hdr_name = strdup(src->conn_src.bind_hdr_name);
		srv->conn_src.bind_hdr_len = strlen(src->conn_src.bind_hdr_name);
	}
	srv->conn_src.bind_hdr_occ = src->conn_src.bind_hdr_occ;
	srv->conn_src.tproxy_addr  = src->conn_src.tproxy_addr;
#endif
	if (src->conn_src.iface_name != NULL)
		srv->conn_src.iface_name = strdup(src->conn_src.iface_name);
}

/*
 * Copy <src> server SSL settings to <srv> server allocating
 * everything needed.
 */
#if defined(USE_OPENSSL)
static void srv_ssl_settings_cpy(struct server *srv, struct server *src)
{
	if (src->ssl_ctx.ca_file != NULL)
		srv->ssl_ctx.ca_file = strdup(src->ssl_ctx.ca_file);
	if (src->ssl_ctx.crl_file != NULL)
		srv->ssl_ctx.crl_file = strdup(src->ssl_ctx.crl_file);
	if (src->ssl_ctx.client_crt != NULL)
		srv->ssl_ctx.client_crt = strdup(src->ssl_ctx.client_crt);

	srv->ssl_ctx.verify = src->ssl_ctx.verify;

	if (src->ssl_ctx.verify_host != NULL)
		srv->ssl_ctx.verify_host = strdup(src->ssl_ctx.verify_host);
	if (src->ssl_ctx.ciphers != NULL)
		srv->ssl_ctx.ciphers = strdup(src->ssl_ctx.ciphers);
#if (OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined OPENSSL_IS_BORINGSSL && !defined LIBRESSL_VERSION_NUMBER)
	if (src->ssl_ctx.ciphersuites != NULL)
		srv->ssl_ctx.ciphersuites = strdup(src->ssl_ctx.ciphersuites);
#endif
	if (src->sni_expr != NULL)
		srv->sni_expr = strdup(src->sni_expr);
}
#endif

/*
 * Prepare <srv> for hostname resolution.
 * May be safely called with a default server as <src> argument (without hostname).
 * Returns -1 in case of any allocation failure, 0 if not.
 */
static int srv_prepare_for_resolution(struct server *srv, const char *hostname)
{
	char *hostname_dn;
	int   hostname_len, hostname_dn_len;

	if (!hostname)
		return 0;

	hostname_len    = strlen(hostname);
	hostname_dn     = trash.str;
	hostname_dn_len = dns_str_to_dn_label(hostname, hostname_len + 1,
					      hostname_dn, trash.size);
	if (hostname_dn_len == -1)
		goto err;


	free(srv->hostname);
	free(srv->hostname_dn);
	srv->hostname        = strdup(hostname);
	srv->hostname_dn     = strdup(hostname_dn);
	srv->hostname_dn_len = hostname_dn_len;
	if (!srv->hostname || !srv->hostname_dn)
		goto err;

	return 0;

 err:
	free(srv->hostname);    srv->hostname    = NULL;
	free(srv->hostname_dn); srv->hostname_dn = NULL;
	return -1;
}

/*
 * Copy <src> server settings to <srv> server allocating
 * everything needed.
 * This function is not supposed to be called at any time, but only
 * during server settings parsing or during server allocations from
 * a server template, and just after having calloc()'ed a new server.
 * So, <src> may only be a default server (when parsing server settings)
 * or a server template (during server allocations from a server template).
 * <srv_tmpl> distinguishes these two cases (must be 1 if <srv> is a template,
 * 0 if not).
 */
static void srv_settings_cpy(struct server *srv, struct server *src, int srv_tmpl)
{
	/* Connection source settings copy */
	srv_conn_src_cpy(srv, src);

	if (srv_tmpl) {
		srv->addr = src->addr;
		srv->svc_port = src->svc_port;
	}

	srv->pp_opts = src->pp_opts;
	if (src->rdr_pfx != NULL) {
		srv->rdr_pfx = strdup(src->rdr_pfx);
		srv->rdr_len = src->rdr_len;
	}
	if (src->cookie != NULL) {
		srv->cookie = strdup(src->cookie);
		srv->cklen  = src->cklen;
	}
	srv->use_ssl                  = src->use_ssl;
	srv->check.addr = srv->agent.addr = src->check.addr;
	srv->check.use_ssl            = src->check.use_ssl;
	srv->check.port               = src->check.port;
	srv->check.sni                = src->check.sni;
	/* Note: 'flags' field has potentially been already initialized. */
	srv->flags                   |= src->flags;
	srv->do_check                 = src->do_check;
	srv->do_agent                 = src->do_agent;
	if (srv->check.port)
		srv->flags |= SRV_F_CHECKPORT;
	srv->check.inter              = src->check.inter;
	srv->check.fastinter          = src->check.fastinter;
	srv->check.downinter          = src->check.downinter;
	srv->agent.use_ssl            = src->agent.use_ssl;
	srv->agent.port               = src->agent.port;
	if (src->agent.send_string != NULL)
		srv->agent.send_string = strdup(src->agent.send_string);
	srv->agent.send_string_len    = src->agent.send_string_len;
	srv->agent.inter              = src->agent.inter;
	srv->agent.fastinter          = src->agent.fastinter;
	srv->agent.downinter          = src->agent.downinter;
	srv->maxqueue                 = src->maxqueue;
	srv->minconn                  = src->minconn;
	srv->maxconn                  = src->maxconn;
	srv->slowstart                = src->slowstart;
	srv->observe                  = src->observe;
	srv->onerror                  = src->onerror;
	srv->onmarkeddown             = src->onmarkeddown;
	srv->onmarkedup               = src->onmarkedup;
	if (src->trackit != NULL)
		srv->trackit = strdup(src->trackit);
	srv->consecutive_errors_limit = src->consecutive_errors_limit;
	srv->uweight = srv->iweight   = src->iweight;

	srv->check.send_proxy         = src->check.send_proxy;
	/* health: up, but will fall down at first failure */
	srv->check.rise = srv->check.health = src->check.rise;
	srv->check.fall               = src->check.fall;

	/* Here we check if 'disabled' is the default server state */
	if (src->next_admin & (SRV_ADMF_CMAINT | SRV_ADMF_FMAINT)) {
		srv->next_admin |= SRV_ADMF_CMAINT | SRV_ADMF_FMAINT;
		srv->next_state        = SRV_ST_STOPPED;
		srv->check.state |= CHK_ST_PAUSED;
		srv->check.health = 0;
	}

	/* health: up but will fall down at first failure */
	srv->agent.rise	= srv->agent.health = src->agent.rise;
	srv->agent.fall	              = src->agent.fall;

	if (src->resolvers_id != NULL)
		srv->resolvers_id = strdup(src->resolvers_id);
	srv->dns_opts.family_prio = src->dns_opts.family_prio;
	srv->dns_opts.accept_duplicate_ip = src->dns_opts.accept_duplicate_ip;
	if (srv->dns_opts.family_prio == AF_UNSPEC)
		srv->dns_opts.family_prio = AF_INET6;
	memcpy(srv->dns_opts.pref_net,
	       src->dns_opts.pref_net,
	       sizeof srv->dns_opts.pref_net);
	srv->dns_opts.pref_net_nb     = src->dns_opts.pref_net_nb;

	srv->init_addr_methods        = src->init_addr_methods;
	srv->init_addr                = src->init_addr;
#if defined(USE_OPENSSL)
	srv_ssl_settings_cpy(srv, src);
#endif
#ifdef TCP_USER_TIMEOUT
	srv->tcp_ut = src->tcp_ut;
#endif
	if (srv_tmpl)
		srv->srvrq = src->srvrq;
}

static struct server *new_server(struct proxy *proxy)
{
	struct server *srv;

	srv = calloc(1, sizeof *srv);
	if (!srv)
		return NULL;

	srv->obj_type = OBJ_TYPE_SERVER;
	srv->proxy = proxy;
	LIST_INIT(&srv->actconns);
	LIST_INIT(&srv->pendconns);

	srv->next_state = SRV_ST_RUNNING; /* early server setup */
	srv->last_change = now.tv_sec;

	srv->check.status = HCHK_STATUS_INI;
	srv->check.server = srv;
	srv->check.tcpcheck_rules = &proxy->tcpcheck_rules;

	srv->agent.status = HCHK_STATUS_INI;
	srv->agent.server = srv;
	srv->xprt  = srv->check.xprt = srv->agent.xprt = xprt_get(XPRT_RAW);

	return srv;
}

/*
 * Validate <srv> server health-check settings.
 * Returns 0 if everything is OK, -1 if not.
 */
static int server_healthcheck_validate(const char *file, int linenum, struct server *srv)
{
	struct tcpcheck_rule *r = NULL;
	struct list *l;

	/*
	 * We need at least a service port, a check port or the first tcp-check rule must
	 * be a 'connect' one when checking an IPv4/IPv6 server.
	 */
	if ((srv_check_healthcheck_port(&srv->check) != 0) ||
	    (!is_inet_addr(&srv->check.addr) && (is_addr(&srv->check.addr) || !is_inet_addr(&srv->addr))))
		return 0;

	r = (struct tcpcheck_rule *)srv->proxy->tcpcheck_rules.n;
	if (!r) {
		ha_alert("parsing [%s:%d] : server %s has neither service port nor check port. "
			 "Check has been disabled.\n",
			 file, linenum, srv->id);
		return -1;
	}

	/* search the first action (connect / send / expect) in the list */
	l = &srv->proxy->tcpcheck_rules;
	list_for_each_entry(r, l, list) {
		if (r->action != TCPCHK_ACT_COMMENT)
			break;
	}

	if ((r->action != TCPCHK_ACT_CONNECT) || !r->port) {
		ha_alert("parsing [%s:%d] : server %s has neither service port nor check port "
			 "nor tcp_check rule 'connect' with port information. Check has been disabled.\n",
			 file, linenum, srv->id);
		return -1;
	}

	/* scan the tcp-check ruleset to ensure a port has been configured */
	l = &srv->proxy->tcpcheck_rules;
	list_for_each_entry(r, l, list) {
		if ((r->action == TCPCHK_ACT_CONNECT) && (!r->port)) {
			ha_alert("parsing [%s:%d] : server %s has neither service port nor check port, "
				 "and a tcp_check rule 'connect' with no port information. Check has been disabled.\n",
				 file, linenum, srv->id);
			return -1;
		}
	}

	return 0;
}

/*
 * Initialize <srv> health-check structure.
 * Returns the error string in case of memory allocation failure, NULL if not.
 */
static const char *do_health_check_init(struct server *srv, int check_type, int state)
{
	const char *ret;

	if (!srv->do_check)
		return NULL;

	ret = init_check(&srv->check, check_type);
	if (ret)
		return ret;

	srv->check.state |= state;
	global.maxsock++;

	return NULL;
}

static int server_health_check_init(const char *file, int linenum,
                                    struct server *srv, struct proxy *curproxy)
{
	const char *ret;

	if (!srv->do_check)
		return 0;

	if (srv->trackit) {
		ha_alert("parsing [%s:%d]: unable to enable checks and tracking at the same time!\n",
			 file, linenum);
		return ERR_ALERT | ERR_FATAL;
	}

	if (server_healthcheck_validate(file, linenum, srv) < 0)
		return ERR_ALERT | ERR_ABORT;

	/* note: check type will be set during the config review phase */
	ret = do_health_check_init(srv, 0, CHK_ST_CONFIGURED | CHK_ST_ENABLED);
	if (ret) {
		ha_alert("parsing [%s:%d] : %s.\n", file, linenum, ret);
		return ERR_ALERT | ERR_ABORT;
	}

	return 0;
}

/*
 * Initialize <srv> agent check structure.
 * Returns the error string in case of memory allocation failure, NULL if not.
 */
static const char *do_server_agent_check_init(struct server *srv, int state)
{
	const char *ret;

	if (!srv->do_agent)
		return NULL;

	ret = init_check(&srv->agent, PR_O2_LB_AGENT_CHK);
	if (ret)
		return ret;

	if (!srv->agent.inter)
		srv->agent.inter = srv->check.inter;

	srv->agent.state |= state;
	global.maxsock++;

	return NULL;
}

static int server_agent_check_init(const char *file, int linenum,
                                   struct server *srv, struct proxy *curproxy)
{
	const char *ret;

	if (!srv->do_agent)
		return 0;

	if (!srv->agent.port) {
		ha_alert("parsing [%s:%d] : server %s does not have agent port. Agent check has been disabled.\n",
			  file, linenum, srv->id);
		return ERR_ALERT | ERR_FATAL;
	}

	ret = do_server_agent_check_init(srv, CHK_ST_CONFIGURED | CHK_ST_ENABLED | CHK_ST_AGENT);
	if (ret) {
		ha_alert("parsing [%s:%d] : %s.\n", file, linenum, ret);
		return ERR_ALERT | ERR_ABORT;
	}

	return 0;
}

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
static int server_sni_expr_init(const char *file, int linenum, char **args, int cur_arg,
                                struct server *srv, struct proxy *proxy)
{
	int ret;
	char *err = NULL;

	if (!srv->sni_expr)
		return 0;

	ret = server_parse_sni_expr(srv, proxy, &err);
	if (!ret)
	    return 0;

	display_parser_err(file, linenum, args, cur_arg, &err);
	free(err);

	return ret;
}
#endif

/*
 * Server initializations finalization.
 * Initialize health check, agent check and SNI expression if enabled.
 * Must not be called for a default server instance.
 */
static int server_finalize_init(const char *file, int linenum, char **args, int cur_arg,
                                struct server *srv, struct proxy *px)
{
	int ret;

	if ((ret = server_health_check_init(file, linenum, srv, px)) != 0 ||
	    (ret = server_agent_check_init(file, linenum, srv, px)) != 0) {
		return ret;
	}

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
	if ((ret = server_sni_expr_init(file, linenum, args, cur_arg, srv, px)) != 0)
		return ret;
#endif

	if (srv->flags & SRV_F_BACKUP)
		px->srv_bck++;
	else
		px->srv_act++;
	srv_lb_commit_status(srv);

	return 0;
}

/*
 * Parse as much as possible such a range string argument: low[-high]
 * Set <nb_low> and <nb_high> values so that they may be reused by this loop
 * for(int i = nb_low; i <= nb_high; i++)... with nb_low >= 1.
 * Fails if 'low' < 0 or 'high' is present and not higher than 'low'.
 * Returns 0 if succeeded, -1 if not.
 */
static int srv_tmpl_parse_range(struct server *srv, const char *arg, int *nb_low, int *nb_high)
{
	char *nb_high_arg;

	*nb_high = 0;
	chunk_printf(&trash, "%s", arg);
	*nb_low = atoi(trash.str);

	if ((nb_high_arg = strchr(trash.str, '-'))) {
		*nb_high_arg++ = '\0';
		*nb_high = atoi(nb_high_arg);
	}
	else {
		*nb_high += *nb_low;
		*nb_low = 1;
	}

	if (*nb_low < 0 || *nb_high < *nb_low)
		return -1;

	return 0;
}

static inline void srv_set_id_from_prefix(struct server *srv, const char *prefix, int nb)
{
	chunk_printf(&trash, "%s%d", prefix, nb);
	free(srv->id);
	srv->id = strdup(trash.str);
}

/*
 * Initialize as much as possible servers from <srv> server template.
 * Note that a server template is a special server with
 * a few different parameters than a server which has
 * been parsed mostly the same way as a server.
 * Returns the number of servers succesfully allocated,
 * 'srv' template included.
 */
static int server_template_init(struct server *srv, struct proxy *px)
{
	int i;
	struct server *newsrv;

	for (i = srv->tmpl_info.nb_low + 1; i <= srv->tmpl_info.nb_high; i++) {
		int check_init_state;
		int agent_init_state;

		newsrv = new_server(px);
		if (!newsrv)
			goto err;

		srv_settings_cpy(newsrv, srv, 1);
		srv_prepare_for_resolution(newsrv, srv->hostname);
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
		if (newsrv->sni_expr) {
			newsrv->ssl_ctx.sni = srv_sni_sample_parse_expr(newsrv, px, NULL, 0, NULL);
			if (!newsrv->ssl_ctx.sni)
				goto err;
		}
#endif
		/* Set this new server ID. */
		srv_set_id_from_prefix(newsrv, srv->tmpl_info.prefix, i);

		/* Initial checks states. */
		check_init_state = CHK_ST_CONFIGURED | CHK_ST_ENABLED;
		agent_init_state = CHK_ST_CONFIGURED | CHK_ST_ENABLED | CHK_ST_AGENT;

		if (do_health_check_init(newsrv, px->options2 & PR_O2_CHK_ANY, check_init_state) ||
		    do_server_agent_check_init(newsrv, agent_init_state))
			goto err;

		/* Linked backwards first. This will be restablished after parsing. */
		newsrv->next = px->srv;
		px->srv = newsrv;
	}
	srv_set_id_from_prefix(srv, srv->tmpl_info.prefix, srv->tmpl_info.nb_low);

	return i - srv->tmpl_info.nb_low;

 err:
	srv_set_id_from_prefix(srv, srv->tmpl_info.prefix, srv->tmpl_info.nb_low);
	if (newsrv)  {
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
		release_sample_expr(newsrv->ssl_ctx.sni);
#endif
		free_check(&newsrv->agent);
		free_check(&newsrv->check);
	}
	free(newsrv);
	return i - srv->tmpl_info.nb_low;
}

int parse_server(const char *file, int linenum, char **args, struct proxy *curproxy, struct proxy *defproxy)
{
	struct server *newsrv = NULL;
	const char *err = NULL;
	char *errmsg = NULL;
	int err_code = 0;
	unsigned val;
	char *fqdn = NULL;

	if (!strcmp(args[0], "server")         ||
	    !strcmp(args[0], "default-server") ||
	    !strcmp(args[0], "server-template")) {
		int cur_arg;
		int defsrv = (*args[0] == 'd');
		int srv = !defsrv && !strcmp(args[0], "server");
		int srv_tmpl = !defsrv && !srv;
		int tmpl_range_low = 0, tmpl_range_high = 0;

		if (!defsrv && curproxy == defproxy) {
			ha_alert("parsing [%s:%d] : '%s' not allowed in 'defaults' section.\n", file, linenum, args[0]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
		else if (warnifnotcap(curproxy, PR_CAP_BE, file, linenum, args[0], NULL))
			err_code |= ERR_WARN;

		/* There is no mandatory first arguments for default server. */
		if (srv) {
			if (!*args[2]) {
				/* 'server' line number of argument check. */
				ha_alert("parsing [%s:%d] : '%s' expects <name> and <addr>[:<port>] as arguments.\n",
					  file, linenum, args[0]);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}

			err = invalid_char(args[1]);
		}
		else if (srv_tmpl) {
			if (!*args[3]) {
				/* 'server-template' line number of argument check. */
				ha_alert("parsing [%s:%d] : '%s' expects <prefix> <nb | range> <addr>[:<port>] as arguments.\n",
					  file, linenum, args[0]);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}

			err = invalid_prefix_char(args[1]);
		}

		if (err) {
			ha_alert("parsing [%s:%d] : character '%c' is not permitted in %s %s '%s'.\n",
			      file, linenum, *err, args[0], srv ? "name" : "prefix", args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}

		cur_arg = 2;
		if (srv_tmpl) {
			/* Parse server-template <nb | range> arg. */
			if (srv_tmpl_parse_range(newsrv, args[cur_arg], &tmpl_range_low, &tmpl_range_high) < 0) {
				ha_alert("parsing [%s:%d] : Wrong %s number or range arg '%s'.\n",
					  file, linenum, args[0], args[cur_arg]);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}
			cur_arg++;
		}

		if (!defsrv) {
			struct sockaddr_storage *sk;
			int port1, port2, port;
			struct protocol *proto;

			newsrv = new_server(curproxy);
			if (!newsrv) {
				ha_alert("parsing [%s:%d] : out of memory.\n", file, linenum);
				err_code |= ERR_ALERT | ERR_ABORT;
				goto out;
			}

			if (srv_tmpl) {
				newsrv->tmpl_info.nb_low = tmpl_range_low;
				newsrv->tmpl_info.nb_high = tmpl_range_high;
			}

			/* the servers are linked backwards first */
			newsrv->next = curproxy->srv;
			curproxy->srv = newsrv;
			newsrv->conf.file = strdup(file);
			newsrv->conf.line = linenum;
			/* Note: for a server template, its id is its prefix.
			 * This is a temporary id which will be used for server allocations to come
			 * after parsing.
			 */
			if (srv)
				newsrv->id = strdup(args[1]);
			else
				newsrv->tmpl_info.prefix = strdup(args[1]);

			/* several ways to check the port component :
			 *  - IP    => port=+0, relative (IPv4 only)
			 *  - IP:   => port=+0, relative
			 *  - IP:N  => port=N, absolute
			 *  - IP:+N => port=+N, relative
			 *  - IP:-N => port=-N, relative
			 */
			sk = str2sa_range(args[cur_arg], &port, &port1, &port2, &errmsg, NULL, &fqdn, 0);
			if (!sk) {
				ha_alert("parsing [%s:%d] : '%s %s' : %s\n", file, linenum, args[0], args[1], errmsg);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}

			proto = protocol_by_family(sk->ss_family);
			if (!fqdn && (!proto || !proto->connect)) {
				ha_alert("parsing [%s:%d] : '%s %s' : connect() not supported for this address family.\n",
				      file, linenum, args[0], args[1]);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}

			if (!port1 || !port2) {
				/* no port specified, +offset, -offset */
				newsrv->flags |= SRV_F_MAPPORTS;
			}
			else if (port1 != port2) {
				/* port range */
				ha_alert("parsing [%s:%d] : '%s %s' : port ranges are not allowed in '%s'\n",
				      file, linenum, args[0], args[1], args[2]);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}

			/* save hostname and create associated name resolution */
			if (fqdn) {
				if (fqdn[0] == '_') { /* SRV record */
					/* Check if a SRV request already exists, and if not, create it */
					if ((newsrv->srvrq = find_srvrq_by_name(fqdn, curproxy)) == NULL)
						newsrv->srvrq = new_dns_srvrq(newsrv, fqdn);
					if (newsrv->srvrq == NULL) {
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}
				}
				else if (srv_prepare_for_resolution(newsrv, fqdn) == -1) {
					ha_alert("parsing [%s:%d] : Can't create DNS resolution for server '%s'\n",
					      file, linenum, newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
			}

			newsrv->addr = *sk;
			newsrv->svc_port = port;

			if (!newsrv->srvrq && !newsrv->hostname && !protocol_by_family(newsrv->addr.ss_family)) {
				ha_alert("parsing [%s:%d] : Unknown protocol family %d '%s'\n",
				      file, linenum, newsrv->addr.ss_family, args[cur_arg]);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}

			/* Copy default server settings to new server settings. */
			srv_settings_cpy(newsrv, &curproxy->defsrv, 0);
			HA_SPIN_INIT(&newsrv->lock);
			cur_arg++;
		} else {
			newsrv = &curproxy->defsrv;
			cur_arg = 1;
			newsrv->dns_opts.family_prio = AF_INET6;
			newsrv->dns_opts.accept_duplicate_ip = 0;
		}

		while (*args[cur_arg]) {
			if (!strcmp(args[cur_arg], "agent-inter")) {
				const char *err = parse_time_err(args[cur_arg + 1], &val, TIME_UNIT_MS);
				if (err) {
					ha_alert("parsing [%s:%d] : unexpected character '%c' in 'agent-inter' argument of server %s.\n",
					      file, linenum, *err, newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				if (val <= 0) {
					ha_alert("parsing [%s:%d]: invalid value %d for argument '%s' of server %s.\n",
					      file, linenum, val, args[cur_arg], newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				newsrv->agent.inter = val;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "agent-addr")) {
				if(str2ip(args[cur_arg + 1], &newsrv->agent.addr) == NULL) {
					ha_alert("parsing agent-addr failed. Check if %s is correct address.\n", args[cur_arg + 1]);
					goto out;
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "agent-port")) {
				global.maxsock++;
				newsrv->agent.port = atol(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "agent-send")) {
				global.maxsock++;
				free(newsrv->agent.send_string);
				newsrv->agent.send_string_len = strlen(args[cur_arg + 1]);
				newsrv->agent.send_string = calloc(1, newsrv->agent.send_string_len + 1);
				memcpy(newsrv->agent.send_string, args[cur_arg + 1], newsrv->agent.send_string_len);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "init-addr")) {
				char *p, *end;
				int done;
				struct sockaddr_storage sa;

				newsrv->init_addr_methods = 0;
				memset(&newsrv->init_addr, 0, sizeof(newsrv->init_addr));

				for (p = args[cur_arg + 1]; *p; p = end) {
					/* cut on next comma */
					for (end = p; *end && *end != ','; end++);
					if (*end)
						*(end++) = 0;

					memset(&sa, 0, sizeof(sa));
					if (!strcmp(p, "libc")) {
						done = srv_append_initaddr(&newsrv->init_addr_methods, SRV_IADDR_LIBC);
					}
					else if (!strcmp(p, "last")) {
						done = srv_append_initaddr(&newsrv->init_addr_methods, SRV_IADDR_LAST);
					}
					else if (!strcmp(p, "none")) {
						done = srv_append_initaddr(&newsrv->init_addr_methods, SRV_IADDR_NONE);
					}
					else if (str2ip2(p, &sa, 0)) {
						if (is_addr(&newsrv->init_addr)) {
							ha_alert("parsing [%s:%d]: '%s' : initial address already specified, cannot add '%s'.\n",
							      file, linenum, args[cur_arg], p);
							err_code |= ERR_ALERT | ERR_FATAL;
							goto out;
						}
						newsrv->init_addr = sa;
						done = srv_append_initaddr(&newsrv->init_addr_methods, SRV_IADDR_IP);
					}
					else {
						ha_alert("parsing [%s:%d]: '%s' : unknown init-addr method '%s', supported methods are 'libc', 'last', 'none'.\n",
							file, linenum, args[cur_arg], p);
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}
					if (!done) {
						ha_alert("parsing [%s:%d]: '%s' : too many init-addr methods when trying to add '%s'\n",
							file, linenum, args[cur_arg], p);
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}
				}
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "resolvers")) {
				free(newsrv->resolvers_id);
				newsrv->resolvers_id = strdup(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "resolve-opts")) {
				char *p, *end;

				for (p = args[cur_arg + 1]; *p; p = end) {
					/* cut on next comma */
					for (end = p; *end && *end != ','; end++);
					if (*end)
						*(end++) = 0;

					if (!strcmp(p, "allow-dup-ip")) {
						newsrv->dns_opts.accept_duplicate_ip = 1;
					}
					else if (!strcmp(p, "prevent-dup-ip")) {
						newsrv->dns_opts.accept_duplicate_ip = 0;
					}
					else {
						ha_alert("parsing [%s:%d]: '%s' : unknown resolve-opts option '%s', supported methods are 'allow-dup-ip' and 'prevent-dup-ip'.\n",
								file, linenum, args[cur_arg], p);
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "resolve-prefer")) {
				if (!strcmp(args[cur_arg + 1], "ipv4"))
					newsrv->dns_opts.family_prio = AF_INET;
				else if (!strcmp(args[cur_arg + 1], "ipv6"))
					newsrv->dns_opts.family_prio = AF_INET6;
				else {
					ha_alert("parsing [%s:%d]: '%s' expects either ipv4 or ipv6 as argument.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "resolve-net")) {
				char *p, *e;
				unsigned char mask;
				struct dns_options *opt;

				if (!args[cur_arg + 1] || args[cur_arg + 1][0] == '\0') {
					ha_alert("parsing [%s:%d]: '%s' expects a list of networks.\n",
					      file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				opt = &newsrv->dns_opts;

				/* Split arguments by comma, and convert it from ipv4 or ipv6
				 * string network in in_addr or in6_addr.
				 */
				p = args[cur_arg + 1];
				e = p;
				while (*p != '\0') {
					/* If no room avalaible, return error. */
					if (opt->pref_net_nb >= SRV_MAX_PREF_NET) {
						ha_alert("parsing [%s:%d]: '%s' exceed %d networks.\n",
						      file, linenum, args[cur_arg], SRV_MAX_PREF_NET);
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}
					/* look for end or comma. */
					while (*e != ',' && *e != '\0')
						e++;
					if (*e == ',') {
						*e = '\0';
						e++;
					}
					if (str2net(p, 0, &opt->pref_net[opt->pref_net_nb].addr.in4,
					                  &opt->pref_net[opt->pref_net_nb].mask.in4)) {
						/* Try to convert input string from ipv4 or ipv6 network. */
						opt->pref_net[opt->pref_net_nb].family = AF_INET;
					} else if (str62net(p, &opt->pref_net[opt->pref_net_nb].addr.in6,
					                     &mask)) {
						/* Try to convert input string from ipv6 network. */
						len2mask6(mask, &opt->pref_net[opt->pref_net_nb].mask.in6);
						opt->pref_net[opt->pref_net_nb].family = AF_INET6;
					} else {
						/* All network conversions fail, retrun error. */
						ha_alert("parsing [%s:%d]: '%s': invalid network '%s'.\n",
						      file, linenum, args[cur_arg], p);
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}
					opt->pref_net_nb++;
					p = e;
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "rise")) {
				if (!*args[cur_arg + 1]) {
					ha_alert("parsing [%s:%d]: '%s' expects an integer argument.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				newsrv->check.rise = atol(args[cur_arg + 1]);
				if (newsrv->check.rise <= 0) {
					ha_alert("parsing [%s:%d]: '%s' has to be > 0.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				if (newsrv->check.health)
					newsrv->check.health = newsrv->check.rise;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "fall")) {
				newsrv->check.fall = atol(args[cur_arg + 1]);

				if (!*args[cur_arg + 1]) {
					ha_alert("parsing [%s:%d]: '%s' expects an integer argument.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				if (newsrv->check.fall <= 0) {
					ha_alert("parsing [%s:%d]: '%s' has to be > 0.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "inter")) {
				const char *err = parse_time_err(args[cur_arg + 1], &val, TIME_UNIT_MS);
				if (err) {
					ha_alert("parsing [%s:%d] : unexpected character '%c' in 'inter' argument of server %s.\n",
					      file, linenum, *err, newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				if (val <= 0) {
					ha_alert("parsing [%s:%d]: invalid value %d for argument '%s' of server %s.\n",
					      file, linenum, val, args[cur_arg], newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				newsrv->check.inter = val;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "fastinter")) {
				const char *err = parse_time_err(args[cur_arg + 1], &val, TIME_UNIT_MS);
				if (err) {
					ha_alert("parsing [%s:%d]: unexpected character '%c' in 'fastinter' argument of server %s.\n",
					      file, linenum, *err, newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				if (val <= 0) {
					ha_alert("parsing [%s:%d]: invalid value %d for argument '%s' of server %s.\n",
					      file, linenum, val, args[cur_arg], newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				newsrv->check.fastinter = val;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "downinter")) {
				const char *err = parse_time_err(args[cur_arg + 1], &val, TIME_UNIT_MS);
				if (err) {
					ha_alert("parsing [%s:%d]: unexpected character '%c' in 'downinter' argument of server %s.\n",
					      file, linenum, *err, newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				if (val <= 0) {
					ha_alert("parsing [%s:%d]: invalid value %d for argument '%s' of server %s.\n",
					      file, linenum, val, args[cur_arg], newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				newsrv->check.downinter = val;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "port")) {
				newsrv->check.port = atol(args[cur_arg + 1]);
				newsrv->flags |= SRV_F_CHECKPORT;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "weight")) {
				int w;
				w = atol(args[cur_arg + 1]);
				if (w < 0 || w > SRV_UWGHT_MAX) {
					ha_alert("parsing [%s:%d] : weight of server %s is not within 0 and %d (%d).\n",
					      file, linenum, newsrv->id, SRV_UWGHT_MAX, w);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				newsrv->uweight = newsrv->iweight = w;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "minconn")) {
				newsrv->minconn = atol(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "maxconn")) {
				newsrv->maxconn = atol(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "maxqueue")) {
				newsrv->maxqueue = atol(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "slowstart")) {
				/* slowstart is stored in seconds */
				const char *err = parse_time_err(args[cur_arg + 1], &val, TIME_UNIT_MS);
				if (err) {
					ha_alert("parsing [%s:%d] : unexpected character '%c' in 'slowstart' argument of server %s.\n",
					      file, linenum, *err, newsrv->id);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				newsrv->slowstart = (val + 999) / 1000;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "on-error")) {
				if (!strcmp(args[cur_arg + 1], "fastinter"))
					newsrv->onerror = HANA_ONERR_FASTINTER;
				else if (!strcmp(args[cur_arg + 1], "fail-check"))
					newsrv->onerror = HANA_ONERR_FAILCHK;
				else if (!strcmp(args[cur_arg + 1], "sudden-death"))
					newsrv->onerror = HANA_ONERR_SUDDTH;
				else if (!strcmp(args[cur_arg + 1], "mark-down"))
					newsrv->onerror = HANA_ONERR_MARKDWN;
				else {
					ha_alert("parsing [%s:%d]: '%s' expects one of 'fastinter', "
						"'fail-check', 'sudden-death' or 'mark-down' but got '%s'\n",
						file, linenum, args[cur_arg], args[cur_arg + 1]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "on-marked-down")) {
				if (!strcmp(args[cur_arg + 1], "shutdown-sessions"))
					newsrv->onmarkeddown = HANA_ONMARKEDDOWN_SHUTDOWNSESSIONS;
				else {
					ha_alert("parsing [%s:%d]: '%s' expects 'shutdown-sessions' but got '%s'\n",
						file, linenum, args[cur_arg], args[cur_arg + 1]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "on-marked-up")) {
				if (!strcmp(args[cur_arg + 1], "shutdown-backup-sessions"))
					newsrv->onmarkedup = HANA_ONMARKEDUP_SHUTDOWNBACKUPSESSIONS;
				else {
					ha_alert("parsing [%s:%d]: '%s' expects 'shutdown-backup-sessions' but got '%s'\n",
						file, linenum, args[cur_arg], args[cur_arg + 1]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "error-limit")) {
				if (!*args[cur_arg + 1]) {
					ha_alert("parsing [%s:%d]: '%s' expects an integer argument.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}

				newsrv->consecutive_errors_limit = atoi(args[cur_arg + 1]);

				if (newsrv->consecutive_errors_limit <= 0) {
					ha_alert("parsing [%s:%d]: %s has to be > 0.\n",
						file, linenum, args[cur_arg]);
					err_code |= ERR_ALERT | ERR_FATAL;
					goto out;
				}
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "usesrc")) {  /* address to use outside: needs "source" first */
				ha_alert("parsing [%s:%d] : '%s' only allowed after a '%s' statement.\n",
				      file, linenum, "usesrc", "source");
				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}
			else {
				static int srv_dumped;
				struct srv_kw *kw;
				char *err;

				kw = srv_find_kw(args[cur_arg]);
				if (kw) {
					char *err = NULL;
					int code;

					if (!kw->parse) {
						ha_alert("parsing [%s:%d] : '%s %s' : '%s' option is not implemented in this version (check build options).\n",
						      file, linenum, args[0], args[1], args[cur_arg]);
						if (kw->skip != -1)
							cur_arg += 1 + kw->skip ;
						err_code |= ERR_ALERT | ERR_FATAL;
						goto out;
					}

					if (defsrv && !kw->default_ok) {
						ha_alert("parsing [%s:%d] : '%s %s' : '%s' option is not accepted in default-server sections.\n",
						      file, linenum, args[0], args[1], args[cur_arg]);
						if (kw->skip != -1)
							cur_arg += 1 + kw->skip ;
						err_code |= ERR_ALERT;
						continue;
					}

					code = kw->parse(args, &cur_arg, curproxy, newsrv, &err);
					err_code |= code;

					if (code) {
						display_parser_err(file, linenum, args, cur_arg, &err);
						if (code & ERR_FATAL) {
							free(err);
							if (kw->skip != -1)
								cur_arg += 1 + kw->skip;
							goto out;
						}
					}
					free(err);
					if (kw->skip != -1)
						cur_arg += 1 + kw->skip;
					continue;
				}

				err = NULL;
				if (!srv_dumped) {
					srv_dump_kws(&err);
					indent_msg(&err, 4);
					srv_dumped = 1;
				}

				ha_alert("parsing [%s:%d] : '%s %s' unknown keyword '%s'.%s%s\n",
				      file, linenum, args[0], args[1], args[cur_arg],
				      err ? " Registered keywords :" : "", err ? err : "");
				free(err);

				err_code |= ERR_ALERT | ERR_FATAL;
				goto out;
			}
		}

		if (!defsrv)
			err_code |= server_finalize_init(file, linenum, args, cur_arg, newsrv, curproxy);
		if (err_code & ERR_FATAL)
			goto out;
		if (srv_tmpl)
			server_template_init(newsrv, curproxy);
	}
	free(fqdn);
	return 0;

 out:
	free(fqdn);
	free(errmsg);
	return err_code;
}

/* Returns a pointer to the first server matching either id <id>.
 * NULL is returned if no match is found.
 * the lookup is performed in the backend <bk>
 */
struct server *server_find_by_id(struct proxy *bk, int id)
{
	struct eb32_node *eb32;
	struct server *curserver;

	if (!bk || (id ==0))
		return NULL;

	/* <bk> has no backend capabilities, so it can't have a server */
	if (!(bk->cap & PR_CAP_BE))
		return NULL;

	curserver = NULL;

	eb32 = eb32_lookup(&bk->conf.used_server_id, id);
	if (eb32)
		curserver = container_of(eb32, struct server, conf.id);

	return curserver;
}

/* Returns a pointer to the first server matching either name <name>, or id
 * if <name> starts with a '#'. NULL is returned if no match is found.
 * the lookup is performed in the backend <bk>
 */
struct server *server_find_by_name(struct proxy *bk, const char *name)
{
	struct server *curserver;

	if (!bk || !name)
		return NULL;

	/* <bk> has no backend capabilities, so it can't have a server */
	if (!(bk->cap & PR_CAP_BE))
		return NULL;

	curserver = NULL;
	if (*name == '#') {
		curserver = server_find_by_id(bk, atoi(name + 1));
		if (curserver)
			return curserver;
	}
	else {
		curserver = bk->srv;

		while (curserver && (strcmp(curserver->id, name) != 0))
			curserver = curserver->next;

		if (curserver)
			return curserver;
	}

	return NULL;
}

struct server *server_find_best_match(struct proxy *bk, char *name, int id, int *diff)
{
	struct server *byname;
	struct server *byid;

	if (!name && !id)
		return NULL;

	if (diff)
		*diff = 0;

	byname = byid = NULL;

	if (name) {
		byname = server_find_by_name(bk, name);
		if (byname && (!id || byname->puid == id))
			return byname;
	}

	/* remaining possibilities :
	 *  - name not set
	 *  - name set but not found
	 *  - name found but ID doesn't match
	 */
	if (id) {
		byid = server_find_by_id(bk, id);
		if (byid) {
			if (byname) {
				/* use id only if forced by configuration */
				if (byid->flags & SRV_F_FORCED_ID) {
					if (diff)
						*diff |= 2;
					return byid;
				}
				else {
					if (diff)
						*diff |= 1;
					return byname;
				}
			}

			/* remaining possibilities:
			 *   - name not set
			 *   - name set but not found
			 */
			if (name && diff)
				*diff |= 2;
			return byid;
		}

		/* id bot found */
		if (byname) {
			if (diff)
				*diff |= 1;
			return byname;
		}
	}

	return NULL;
}

/* Registers changes to be applied asynchronously */
static void srv_register_update(struct server *srv)
{
	if (LIST_ISEMPTY(&srv->update_status)) {
		THREAD_WANT_SYNC();
		HA_SPIN_LOCK(UPDATED_SERVERS_LOCK, &updated_servers_lock);
		if (LIST_ISEMPTY(&srv->update_status))
			LIST_ADDQ(&updated_servers, &srv->update_status);
		HA_SPIN_UNLOCK(UPDATED_SERVERS_LOCK, &updated_servers_lock);
	}
}

/* Update a server state using the parameters available in the params list */
static void srv_update_state(struct server *srv, int version, char **params)
{
	char *p;
	struct chunk *msg;

	/* fields since version 1
	 * and common to all other upcoming versions
	 */
	enum srv_state srv_op_state;
	enum srv_admin srv_admin_state;
	unsigned srv_uweight, srv_iweight;
	unsigned long srv_last_time_change;
	short srv_check_status;
	enum chk_result srv_check_result;
	int srv_check_health;
	int srv_check_state, srv_agent_state;
	int bk_f_forced_id;
	int srv_f_forced_id;
	int fqdn_set_by_cli;
	const char *fqdn;
	const char *port_str;
	unsigned int port;
	char *srvrecord;

	fqdn = NULL;
	port = 0;
	msg = get_trash_chunk();
	switch (version) {
		case 1:
			/*
			 * now we can proceed with server's state update:
			 * srv_addr:             params[0]
			 * srv_op_state:         params[1]
			 * srv_admin_state:      params[2]
			 * srv_uweight:          params[3]
			 * srv_iweight:          params[4]
			 * srv_last_time_change: params[5]
			 * srv_check_status:     params[6]
			 * srv_check_result:     params[7]
			 * srv_check_health:     params[8]
			 * srv_check_state:      params[9]
			 * srv_agent_state:      params[10]
			 * bk_f_forced_id:       params[11]
			 * srv_f_forced_id:      params[12]
			 * srv_fqdn:             params[13]
			 * srv_port:             params[14]
			 * srvrecord:            params[15]
			 */

			/* validating srv_op_state */
			p = NULL;
			errno = 0;
			srv_op_state = strtol(params[1], &p, 10);
			if ((p == params[1]) || errno == EINVAL || errno == ERANGE ||
			    (srv_op_state != SRV_ST_STOPPED &&
			     srv_op_state != SRV_ST_STARTING &&
			     srv_op_state != SRV_ST_RUNNING &&
			     srv_op_state != SRV_ST_STOPPING)) {
				chunk_appendf(msg, ", invalid srv_op_state value '%s'", params[1]);
			}

			/* validating srv_admin_state */
			p = NULL;
			errno = 0;
			srv_admin_state = strtol(params[2], &p, 10);
			fqdn_set_by_cli = !!(srv_admin_state & SRV_ADMF_HMAINT);

			/* inherited statuses will be recomputed later.
			 * Also disable SRV_ADMF_HMAINT flag (set from stats socket fqdn).
			 */
			srv_admin_state &= ~SRV_ADMF_IDRAIN & ~SRV_ADMF_IMAINT & ~SRV_ADMF_HMAINT;

			if ((p == params[2]) || errno == EINVAL || errno == ERANGE ||
			    (srv_admin_state != 0 &&
			     srv_admin_state != SRV_ADMF_FMAINT &&
			     srv_admin_state != SRV_ADMF_CMAINT &&
			     srv_admin_state != (SRV_ADMF_CMAINT | SRV_ADMF_FMAINT) &&
			     srv_admin_state != (SRV_ADMF_CMAINT | SRV_ADMF_FDRAIN) &&
			     srv_admin_state != SRV_ADMF_FDRAIN)) {
				chunk_appendf(msg, ", invalid srv_admin_state value '%s'", params[2]);
			}

			/* validating srv_uweight */
			p = NULL;
			errno = 0;
			srv_uweight = strtol(params[3], &p, 10);
			if ((p == params[3]) || errno == EINVAL || errno == ERANGE || (srv_uweight > SRV_UWGHT_MAX))
				chunk_appendf(msg, ", invalid srv_uweight value '%s'", params[3]);

			/* validating srv_iweight */
			p = NULL;
			errno = 0;
			srv_iweight = strtol(params[4], &p, 10);
			if ((p == params[4]) || errno == EINVAL || errno == ERANGE || (srv_iweight > SRV_UWGHT_MAX))
				chunk_appendf(msg, ", invalid srv_iweight value '%s'", params[4]);

			/* validating srv_last_time_change */
			p = NULL;
			errno = 0;
			srv_last_time_change = strtol(params[5], &p, 10);
			if ((p == params[5]) || errno == EINVAL || errno == ERANGE)
				chunk_appendf(msg, ", invalid srv_last_time_change value '%s'", params[5]);

			/* validating srv_check_status */
			p = NULL;
			errno = 0;
			srv_check_status = strtol(params[6], &p, 10);
			if (p == params[6] || errno == EINVAL || errno == ERANGE ||
			    (srv_check_status >= HCHK_STATUS_SIZE))
				chunk_appendf(msg, ", invalid srv_check_status value '%s'", params[6]);

			/* validating srv_check_result */
			p = NULL;
			errno = 0;
			srv_check_result = strtol(params[7], &p, 10);
			if ((p == params[7]) || errno == EINVAL || errno == ERANGE ||
			    (srv_check_result != CHK_RES_UNKNOWN &&
			     srv_check_result != CHK_RES_NEUTRAL &&
			     srv_check_result != CHK_RES_FAILED &&
			     srv_check_result != CHK_RES_PASSED &&
			     srv_check_result != CHK_RES_CONDPASS)) {
				chunk_appendf(msg, ", invalid srv_check_result value '%s'", params[7]);
			}

			/* validating srv_check_health */
			p = NULL;
			errno = 0;
			srv_check_health = strtol(params[8], &p, 10);
			if (p == params[8] || errno == EINVAL || errno == ERANGE)
				chunk_appendf(msg, ", invalid srv_check_health value '%s'", params[8]);

			/* validating srv_check_state */
			p = NULL;
			errno = 0;
			srv_check_state = strtol(params[9], &p, 10);
			if (p == params[9] || errno == EINVAL || errno == ERANGE ||
			    (srv_check_state & ~(CHK_ST_INPROGRESS | CHK_ST_CONFIGURED | CHK_ST_ENABLED | CHK_ST_PAUSED | CHK_ST_AGENT)))
				chunk_appendf(msg, ", invalid srv_check_state value '%s'", params[9]);

			/* validating srv_agent_state */
			p = NULL;
			errno = 0;
			srv_agent_state = strtol(params[10], &p, 10);
			if (p == params[10] || errno == EINVAL || errno == ERANGE ||
			    (srv_agent_state & ~(CHK_ST_INPROGRESS | CHK_ST_CONFIGURED | CHK_ST_ENABLED | CHK_ST_PAUSED | CHK_ST_AGENT)))
				chunk_appendf(msg, ", invalid srv_agent_state value '%s'", params[10]);

			/* validating bk_f_forced_id */
			p = NULL;
			errno = 0;
			bk_f_forced_id = strtol(params[11], &p, 10);
			if (p == params[11] || errno == EINVAL || errno == ERANGE || !((bk_f_forced_id == 0) || (bk_f_forced_id == 1)))
				chunk_appendf(msg, ", invalid bk_f_forced_id value '%s'", params[11]);

			/* validating srv_f_forced_id */
			p = NULL;
			errno = 0;
			srv_f_forced_id = strtol(params[12], &p, 10);
			if (p == params[12] || errno == EINVAL || errno == ERANGE || !((srv_f_forced_id == 0) || (srv_f_forced_id == 1)))
				chunk_appendf(msg, ", invalid srv_f_forced_id value '%s'", params[12]);

			/* validating srv_fqdn */
			fqdn = params[13];
			if (fqdn && *fqdn == '-')
				fqdn = NULL;
			if (fqdn && (strlen(fqdn) > DNS_MAX_NAME_SIZE || invalid_domainchar(fqdn))) {
				chunk_appendf(msg, ", invalid srv_fqdn value '%s'", params[13]);
				fqdn = NULL;
			}

			port_str = params[14];
			if (port_str) {
				port = strl2uic(port_str, strlen(port_str));
				if (port > USHRT_MAX) {
					chunk_appendf(msg, ", invalid srv_port value '%s'", port_str);
					port_str = NULL;
				}
			}

			/* SRV record
			 * NOTE: in HAProxy, SRV records must start with an underscore '_'
			 */
			srvrecord = params[15];
			if (srvrecord && *srvrecord != '_')
				srvrecord = NULL;

			/* don't apply anything if one error has been detected */
			if (msg->len)
				goto out;

			HA_SPIN_LOCK(SERVER_LOCK, &srv->lock);
			/* recover operational state and apply it to this server
			 * and all servers tracking this one */
			srv->check.health = srv_check_health;
			switch (srv_op_state) {
				case SRV_ST_STOPPED:
					srv->check.health = 0;
					srv_set_stopped(srv, "changed from server-state after a reload", NULL);
					break;
				case SRV_ST_STARTING:
					/* If rise == 1 there is no STARTING state, let's switch to
					 * RUNNING
					 */
					if (srv->check.rise == 1) {
						srv->check.health = srv->check.rise + srv->check.fall - 1;
						srv_set_running(srv, "", NULL);
						break;
					}
					if (srv->check.health < 1 || srv->check.health >= srv->check.rise)
						srv->check.health = srv->check.rise - 1;
					srv->next_state = srv_op_state;
					break;
				case SRV_ST_STOPPING:
					/* If fall == 1 there is no STOPPING state, let's switch to
					 * STOPPED
					 */
					if (srv->check.fall == 1) {
						srv->check.health = 0;
						srv_set_stopped(srv, "changed from server-state after a reload", NULL);
						break;
					}
					if (srv->check.health < srv->check.rise ||
					    srv->check.health > srv->check.rise + srv->check.fall - 2)
						srv->check.health = srv->check.rise;
					srv_set_stopping(srv, "changed from server-state after a reload", NULL);
					break;
				case SRV_ST_RUNNING:
					srv->check.health = srv->check.rise + srv->check.fall - 1;
					srv_set_running(srv, "", NULL);
					break;
			}

			/* When applying server state, the following rules apply:
			 * - in case of a configuration change, we apply the setting from the new
			 *   configuration, regardless of old running state
			 * - if no configuration change, we apply old running state only if old running
			 *   state is different from new configuration state
			 */
			/* configuration has changed */
			if ((srv_admin_state & SRV_ADMF_CMAINT) != (srv->next_admin & SRV_ADMF_CMAINT)) {
				if (srv->next_admin & SRV_ADMF_CMAINT)
					srv_adm_set_maint(srv);
				else
					srv_adm_set_ready(srv);
			}
			/* configuration is the same, let's compate old running state and new conf state */
			else {
				if (srv_admin_state & SRV_ADMF_FMAINT && !(srv->next_admin & SRV_ADMF_CMAINT))
					srv_adm_set_maint(srv);
				else if (!(srv_admin_state & SRV_ADMF_FMAINT) && (srv->next_admin & SRV_ADMF_CMAINT))
					srv_adm_set_ready(srv);
			}
			/* apply drain mode if server is currently enabled */
			if (!(srv->next_admin & SRV_ADMF_FMAINT) && (srv_admin_state & SRV_ADMF_FDRAIN)) {
				/* The SRV_ADMF_FDRAIN flag is inherited when srv->iweight is 0
				 * (srv->iweight is the weight set up in configuration).
				 * There are two possible reasons for FDRAIN to have been present :
				 *   - previous config weight was zero
				 *   - "set server b/s drain" was sent to the CLI
				 *
				 * In the first case, we simply want to drop this drain state
				 * if the new weight is not zero anymore, meaning the administrator
				 * has intentionally turned the weight back to a positive value to
				 * enable the server again after an operation. In the second case,
				 * the drain state was forced on the CLI regardless of the config's
				 * weight so we don't want a change to the config weight to lose this
				 * status. What this means is :
				 *   - if previous weight was 0 and new one is >0, drop the DRAIN state.
				 *   - if the previous weight was >0, keep it.
				 */
				if (srv_iweight > 0 || srv->iweight == 0)
					srv_adm_set_drain(srv);
			}

			srv->last_change = date.tv_sec - srv_last_time_change;
			srv->check.status = srv_check_status;
			srv->check.result = srv_check_result;

			/* Only case we want to apply is removing ENABLED flag which could have been
			 * done by the "disable health" command over the stats socket
			 */
			if ((srv->check.state & CHK_ST_CONFIGURED) &&
			    (srv_check_state & CHK_ST_CONFIGURED) &&
			    !(srv_check_state & CHK_ST_ENABLED))
				srv->check.state &= ~CHK_ST_ENABLED;

			/* Only case we want to apply is removing ENABLED flag which could have been
			 * done by the "disable agent" command over the stats socket
			 */
			if ((srv->agent.state & CHK_ST_CONFIGURED) &&
			    (srv_agent_state & CHK_ST_CONFIGURED) &&
			    !(srv_agent_state & CHK_ST_ENABLED))
				srv->agent.state &= ~CHK_ST_ENABLED;

			/* We want to apply the previous 'running' weight (srv_uweight) only if there
			 * was no change in the configuration: both previous and new iweight are equals
			 *
			 * It means that a configuration file change has precedence over a unix socket change
			 * for server's weight
			 *
			 * by default, HAProxy applies the following weight when parsing the configuration
			 *    srv->uweight = srv->iweight
			 */
			if (srv_iweight == srv->iweight) {
				srv->uweight = srv_uweight;
			}
			server_recalc_eweight(srv);

			/* load server IP address */
			if (strcmp(params[0], "-"))
				srv->lastaddr = strdup(params[0]);

			if (fqdn && srv->hostname) {
				if (!strcmp(srv->hostname, fqdn)) {
					/* Here we reset the 'set from stats socket FQDN' flag
					 * to support such transitions:
					 * Let's say initial FQDN value is foo1 (in configuration file).
					 * - FQDN changed from stats socket, from foo1 to foo2 value,
					 * - FQDN changed again from file configuration (with the same previous value
					     set from stats socket, from foo1 to foo2 value),
					 * - reload for any other reason than a FQDN modification,
					 * the configuration file FQDN matches the fqdn server state file value.
					 * So we must reset the 'set from stats socket FQDN' flag to be consistent with
					 * any futher FQDN modification.
					 */
					srv->next_admin &= ~SRV_ADMF_HMAINT;
				}
				else {
					/* If the FDQN has been changed from stats socket,
					 * apply fqdn state file value (which is the value set
					 * from stats socket).
					 */
					if (fqdn_set_by_cli) {
						srv_set_fqdn(srv, fqdn, 0);
						srv->next_admin |= SRV_ADMF_HMAINT;
					}
				}
			}
			/* If all the conditions below are validated, this means
			 * we're evaluating a server managed by SRV resolution
			 */
			else if (fqdn && !srv->hostname && srvrecord) {
				int res;

				/* we can't apply previous state if SRV record has changed */
				if (srv->srvrq && strcmp(srv->srvrq->name, srvrecord) != 0) {
					chunk_appendf(msg, ", SRV record mismatch between configuration ('%s') and state file ('%s) for server '%s'. Previous state not applied", srv->srvrq->name, srvrecord, srv->id);
					HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
					goto out;
				}

				/* create or find a SRV resolution for this srv record */
				if (srv->srvrq == NULL && (srv->srvrq = find_srvrq_by_name(srvrecord, srv->proxy)) == NULL)
					srv->srvrq = new_dns_srvrq(srv, srvrecord);
				if (srv->srvrq == NULL) {
					chunk_appendf(msg, ", can't create or find SRV resolution '%s' for server '%s'", srvrecord, srv->id);
					HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
					goto out;
				}

				/* prepare DNS resolution for this server */
				res = srv_prepare_for_resolution(srv, fqdn);
				if (res == -1) {
					chunk_appendf(msg, ", can't allocate memory for DNS resolution for server '%s'", srv->id);
					HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);
					goto out;
				}

				/* configure check.port accordingly */
				if ((srv->check.state & CHK_ST_CONFIGURED) &&
				    !(srv->flags & SRV_F_CHECKPORT))
					srv->check.port = port;

				/* Unset SRV_F_MAPPORTS for SRV records.
				 * SRV_F_MAPPORTS is unfortunately set by parse_server()
				 * because no ports are provided in the configuration file.
				 * This is because HAProxy will use the port found into the SRV record.
				 */
				srv->flags &= ~SRV_F_MAPPORTS;
			}


			if (port_str)
				srv->svc_port = port;
			HA_SPIN_UNLOCK(SERVER_LOCK, &srv->lock);

			break;
		default:
			chunk_appendf(msg, ", version '%d' not supported", version);
	}

 out:
	if (msg->len) {
		chunk_appendf(msg, "\n");
		ha_warning("server-state application failed for server '%s/%s'%s",
			   srv->proxy->id, srv->id, msg->str);
	}
}

/* This function parses all the proxies and only take care of the backends (since we're looking for server)
 * For each proxy, it does the following:
 *  - opens its server state file (either one or local one)
 *  - read whole file, line by line
 *  - analyse each line to check if it matches our current backend:
 *    - backend name matches
 *    - backend id matches if id is forced and name doesn't match
 *  - if the server pointed by the line is found, then state is applied
 *
 * If the running backend uuid or id differs from the state file, then HAProxy reports
 * a warning.
 */
void apply_server_state(void)
{
	char *cur, *end;
	char mybuf[SRV_STATE_LINE_MAXLEN];
	int mybuflen;
	char *params[SRV_STATE_FILE_MAX_FIELDS] = {0};
	char *srv_params[SRV_STATE_FILE_MAX_FIELDS] = {0};
	int arg, srv_arg, version, diff;
	FILE *f;
	char *filepath;
	char globalfilepath[MAXPATHLEN + 1];
	char localfilepath[MAXPATHLEN + 1];
	int len, fileopenerr, globalfilepathlen, localfilepathlen;
	struct proxy *curproxy, *bk;
	struct server *srv;

	globalfilepathlen = 0;
	/* create the globalfilepath variable */
	if (global.server_state_file) {
		/* absolute path or no base directory provided */
	        if ((global.server_state_file[0] == '/') || (!global.server_state_base)) {
			len = strlen(global.server_state_file);
			if (len > MAXPATHLEN) {
				globalfilepathlen = 0;
				goto globalfileerror;
			}
			memcpy(globalfilepath, global.server_state_file, len);
			globalfilepath[len] = '\0';
			globalfilepathlen = len;
		}
		else if (global.server_state_base) {
			len = strlen(global.server_state_base);
			globalfilepathlen += len;

			if (globalfilepathlen > MAXPATHLEN) {
				globalfilepathlen = 0;
				goto globalfileerror;
			}
			memcpy(globalfilepath, global.server_state_base, len);
			globalfilepath[globalfilepathlen] = 0;

			/* append a slash if needed */
			if (!globalfilepathlen || globalfilepath[globalfilepathlen - 1] != '/') {
				if (globalfilepathlen + 1 > MAXPATHLEN) {
					globalfilepathlen = 0;
					goto globalfileerror;
				}
				globalfilepath[globalfilepathlen++] = '/';
			}

			len = strlen(global.server_state_file);
			if (globalfilepathlen + len > MAXPATHLEN) {
				globalfilepathlen = 0;
				goto globalfileerror;
			}
			memcpy(globalfilepath + globalfilepathlen, global.server_state_file, len);
			globalfilepathlen += len;
			globalfilepath[globalfilepathlen++] = 0;
		}
	}
 globalfileerror:
	if (globalfilepathlen == 0)
		globalfilepath[0] = '\0';

	/* read servers state from local file */
	for (curproxy = proxies_list; curproxy != NULL; curproxy = curproxy->next) {
		/* servers are only in backends */
		if (!(curproxy->cap & PR_CAP_BE))
			continue;
		fileopenerr = 0;
		filepath = NULL;

		/* search server state file path and name */
		switch (curproxy->load_server_state_from_file) {
			/* read servers state from global file */
			case PR_SRV_STATE_FILE_GLOBAL:
				/* there was an error while generating global server state file path */
				if (globalfilepathlen == 0)
					continue;
				filepath = globalfilepath;
				fileopenerr = 1;
				break;
			/* this backend has its own file */
			case PR_SRV_STATE_FILE_LOCAL:
				localfilepathlen = 0;
				localfilepath[0] = '\0';
				len = 0;
				/* create the localfilepath variable */
				/* absolute path or no base directory provided */
				if ((curproxy->server_state_file_name[0] == '/') || (!global.server_state_base)) {
					len = strlen(curproxy->server_state_file_name);
					if (len > MAXPATHLEN) {
						localfilepathlen = 0;
						goto localfileerror;
					}
					memcpy(localfilepath, curproxy->server_state_file_name, len);
					localfilepath[len] = '\0';
					localfilepathlen = len;
				}
				else if (global.server_state_base) {
					len = strlen(global.server_state_base);
					localfilepathlen += len;

					if (localfilepathlen > MAXPATHLEN) {
						localfilepathlen = 0;
						goto localfileerror;
					}
					memcpy(localfilepath, global.server_state_base, len);
					localfilepath[localfilepathlen] = 0;

					/* append a slash if needed */
					if (!localfilepathlen || localfilepath[localfilepathlen - 1] != '/') {
						if (localfilepathlen + 1 > MAXPATHLEN) {
							localfilepathlen = 0;
							goto localfileerror;
						}
						localfilepath[localfilepathlen++] = '/';
					}

					len = strlen(curproxy->server_state_file_name);
					if (localfilepathlen + len > MAXPATHLEN) {
						localfilepathlen = 0;
						goto localfileerror;
					}
					memcpy(localfilepath + localfilepathlen, curproxy->server_state_file_name, len);
					localfilepathlen += len;
					localfilepath[localfilepathlen++] = 0;
				}
				filepath = localfilepath;
 localfileerror:
				if (localfilepathlen == 0)
					localfilepath[0] = '\0';

				break;
			case PR_SRV_STATE_FILE_NONE:
			default:
				continue;
		}

		/* preload global state file */
		errno = 0;
		f = fopen(filepath, "r");
		if (errno && fileopenerr)
			ha_warning("Can't open server state file '%s': %s\n", filepath, strerror(errno));
		if (!f)
			continue;

		mybuf[0] = '\0';
		mybuflen = 0;
		version = 0;

		/* first character of first line of the file must contain the version of the export */
		if (fgets(mybuf, SRV_STATE_LINE_MAXLEN, f) == NULL) {
			ha_warning("Can't read first line of the server state file '%s'\n", filepath);
			goto fileclose;
		}

		cur = mybuf;
		version = atoi(cur);
		if ((version < SRV_STATE_FILE_VERSION_MIN) ||
		    (version > SRV_STATE_FILE_VERSION_MAX))
			goto fileclose;

		while (fgets(mybuf, SRV_STATE_LINE_MAXLEN, f)) {
			int bk_f_forced_id = 0;
			int check_id = 0;
			int check_name = 0;

			mybuflen = strlen(mybuf);
			cur = mybuf;
			end = cur + mybuflen;

			bk = NULL;
			srv = NULL;

			/* we need at least one character */
			if (mybuflen == 0)
				continue;

			/* ignore blank characters at the beginning of the line */
			while (isspace(*cur))
				++cur;

			/* Ignore empty or commented lines */
			if (cur == end || *cur == '#')
				continue;

			/* truncated lines */
			if (mybuf[mybuflen - 1] != '\n') {
				ha_warning("server-state file '%s': truncated line\n", filepath);
				continue;
			}

			/* Removes trailing '\n' */
			mybuf[mybuflen - 1] = '\0';

			/* we're now ready to move the line into *srv_params[] */
			params[0] = cur;
			arg = 1;
			srv_arg = 0;
			while (*cur && arg < SRV_STATE_FILE_MAX_FIELDS) {
				if (isspace(*cur)) {
					*cur = '\0';
					++cur;
					while (isspace(*cur))
						++cur;
					switch (version) {
						case 1:
							/*
							 * srv_addr:             params[4]  => srv_params[0]
							 * srv_op_state:         params[5]  => srv_params[1]
							 * srv_admin_state:      params[6]  => srv_params[2]
							 * srv_uweight:          params[7]  => srv_params[3]
							 * srv_iweight:          params[8]  => srv_params[4]
							 * srv_last_time_change: params[9]  => srv_params[5]
							 * srv_check_status:     params[10] => srv_params[6]
							 * srv_check_result:     params[11] => srv_params[7]
							 * srv_check_health:     params[12] => srv_params[8]
							 * srv_check_state:      params[13] => srv_params[9]
							 * srv_agent_state:      params[14] => srv_params[10]
							 * bk_f_forced_id:       params[15] => srv_params[11]
							 * srv_f_forced_id:      params[16] => srv_params[12]
							 * srv_fqdn:             params[17] => srv_params[13]
							 * srv_port:             params[18] => srv_params[14]
							 * srvrecord:            params[19] => srv_params[15]
							 */
							if (arg >= 4) {
								srv_params[srv_arg] = cur;
								++srv_arg;
							}
							break;
					}

					params[arg] = cur;
					++arg;
				}
				else {
					++cur;
				}
			}

			/* if line is incomplete line, then ignore it.
			 * otherwise, update useful flags */
			switch (version) {
				case 1:
					if (arg < SRV_STATE_FILE_NB_FIELDS_VERSION_1)
						continue;
					bk_f_forced_id = (atoi(params[15]) & PR_O_FORCED_ID);
					check_id = (atoi(params[0]) == curproxy->uuid);
					check_name = (strcmp(curproxy->id, params[1]) == 0);
					break;
			}

			diff = 0;
			bk = curproxy;

			/* if backend can't be found, let's continue */
			if (!check_id && !check_name)
				continue;
			else if (!check_id && check_name) {
				ha_warning("backend ID mismatch: from server state file: '%s', from running config '%d'\n", params[0], bk->uuid);
				send_log(bk, LOG_NOTICE, "backend ID mismatch: from server state file: '%s', from running config '%d'\n", params[0], bk->uuid);
			}
			else if (check_id && !check_name) {
				ha_warning("backend name mismatch: from server state file: '%s', from running config '%s'\n", params[1], bk->id);
				send_log(bk, LOG_NOTICE, "backend name mismatch: from server state file: '%s', from running config '%s'\n", params[1], bk->id);
				/* if name doesn't match, we still want to update curproxy if the backend id
				 * was forced in previous the previous configuration */
				if (!bk_f_forced_id)
					continue;
			}

			/* look for the server by its id: param[2] */
			/* else look for the server by its name: param[3] */
			diff = 0;
			srv = server_find_best_match(bk, params[3], atoi(params[2]), &diff);

			if (!srv) {
				/* if no server found, then warning and continue with next line */
				ha_warning("can't find server '%s' with id '%s' in backend with id '%s' or name '%s'\n",
					   params[3], params[2], params[0], params[1]);
				send_log(bk, LOG_NOTICE, "can't find server '%s' with id '%s' in backend with id '%s' or name '%s'\n",
					 params[3], params[2], params[0], params[1]);
				continue;
			}
			else if (diff & PR_FBM_MISMATCH_ID) {
				ha_warning("In backend '%s' (id: '%d'): server ID mismatch: from server state file: '%s', from running config %d\n", bk->id, bk->uuid, params[2], srv->puid);
				send_log(bk, LOG_NOTICE, "In backend '%s' (id: %d): server ID mismatch: from server state file: '%s', from running config %d\n", bk->id, bk->uuid, params[2], srv->puid);
				continue;
			}
			else if (diff & PR_FBM_MISMATCH_NAME) {
				ha_warning("In backend '%s' (id: %d): server name mismatch: from server state file: '%s', from running config '%s'\n", bk->id, bk->uuid, params[3], srv->id);
				send_log(bk, LOG_NOTICE, "In backend '%s' (id: %d): server name mismatch: from server state file: '%s', from running config '%s'\n", bk->id, bk->uuid, params[3], srv->id);
				continue;
			}

			/* now we can proceed with server's state update */
			srv_update_state(srv, version, srv_params);
		}
fileclose:
		fclose(f);
	}
}

/*
 * update a server's current IP address.
 * ip is a pointer to the new IP address, whose address family is ip_sin_family.
 * ip is in network format.
 * updater is a string which contains an information about the requester of the update.
 * updater is used if not NULL.
 *
 * A log line and a stderr warning message is generated based on server's backend options.
 */
int update_server_addr(struct server *s, void *ip, int ip_sin_family, const char *updater)
{
	/* generates a log line and a warning on stderr */
	if (1) {
		/* book enough space for both IPv4 and IPv6 */
		char oldip[INET6_ADDRSTRLEN];
		char newip[INET6_ADDRSTRLEN];

		memset(oldip, '\0', INET6_ADDRSTRLEN);
		memset(newip, '\0', INET6_ADDRSTRLEN);

		/* copy old IP address in a string */
		switch (s->addr.ss_family) {
		case AF_INET:
			inet_ntop(s->addr.ss_family, &((struct sockaddr_in *)&s->addr)->sin_addr, oldip, INET_ADDRSTRLEN);
			break;
		case AF_INET6:
			inet_ntop(s->addr.ss_family, &((struct sockaddr_in6 *)&s->addr)->sin6_addr, oldip, INET6_ADDRSTRLEN);
			break;
		};

		/* copy new IP address in a string */
		switch (ip_sin_family) {
		case AF_INET:
			inet_ntop(ip_sin_family, ip, newip, INET_ADDRSTRLEN);
			break;
		case AF_INET6:
			inet_ntop(ip_sin_family, ip, newip, INET6_ADDRSTRLEN);
			break;
		};

		/* save log line into a buffer */
		chunk_printf(&trash, "%s/%s changed its IP from %s to %s by %s",
				s->proxy->id, s->id, oldip, newip, updater);

		/* write the buffer on stderr */
		ha_warning("%s.\n", trash.str);

		/* send a log */
		send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);
	}

	/* save the new IP family */
	s->addr.ss_family = ip_sin_family;
	/* save the new IP address */
	switch (ip_sin_family) {
	case AF_INET:
		memcpy(&((struct sockaddr_in *)&s->addr)->sin_addr.s_addr, ip, 4);
		break;
	case AF_INET6:
		memcpy(((struct sockaddr_in6 *)&s->addr)->sin6_addr.s6_addr, ip, 16);
		break;
	};
	srv_set_dyncookie(s);

	return 0;
}

/*
 * This function update a server's addr and port only for AF_INET and AF_INET6 families.
 *
 * Caller can pass its name through <updater> to get it integrated in the response message
 * returned by the function.
 *
 * The function first does the following, in that order:
 * - validates the new addr and/or port
 * - checks if an update is required (new IP or port is different than current ones)
 * - checks the update is allowed:
 *   - don't switch from/to a family other than AF_INET4 and AF_INET6
 *   - allow all changes if no CHECKS are configured
 *   - if CHECK is configured:
 *     - if switch to port map (SRV_F_MAPPORTS), ensure health check have their own ports
 * - applies required changes to both ADDR and PORT if both 'required' and 'allowed'
 *   conditions are met
 */
const char *update_server_addr_port(struct server *s, const char *addr, const char *port, char *updater)
{
	struct sockaddr_storage sa;
	int ret, port_change_required;
	char current_addr[INET6_ADDRSTRLEN];
	uint16_t current_port, new_port;
	struct chunk *msg;
	int changed = 0;

	msg = get_trash_chunk();
	chunk_reset(msg);

	if (addr) {
		memset(&sa, 0, sizeof(struct sockaddr_storage));
		if (str2ip2(addr, &sa, 0) == NULL) {
			chunk_printf(msg, "Invalid addr '%s'", addr);
			goto out;
		}

		/* changes are allowed on AF_INET* families only */
		if ((sa.ss_family != AF_INET) && (sa.ss_family != AF_INET6)) {
			chunk_printf(msg, "Update to families other than AF_INET and AF_INET6 supported only through configuration file");
			goto out;
		}

		/* collecting data currently setup */
		memset(current_addr, '\0', sizeof(current_addr));
		ret = addr_to_str(&s->addr, current_addr, sizeof(current_addr));
		/* changes are allowed on AF_INET* families only */
		if ((ret != AF_INET) && (ret != AF_INET6)) {
			chunk_printf(msg, "Update for the current server address family is only supported through configuration file");
			goto out;
		}

		/* applying ADDR changes if required and allowed
		 * ipcmp returns 0 when both ADDR are the same
		 */
		if (ipcmp(&s->addr, &sa) == 0) {
			chunk_appendf(msg, "no need to change the addr");
			goto port;
		}
		ipcpy(&sa, &s->addr);
		changed = 1;

		/* we also need to update check's ADDR only if it uses the server's one */
		if ((s->check.state & CHK_ST_CONFIGURED) && (s->flags & SRV_F_CHECKADDR)) {
			ipcpy(&sa, &s->check.addr);
		}

		/* we also need to update agent ADDR only if it use the server's one */
		if ((s->agent.state & CHK_ST_CONFIGURED) && (s->flags & SRV_F_AGENTADDR)) {
			ipcpy(&sa, &s->agent.addr);
		}

		/* update report for caller */
		chunk_printf(msg, "IP changed from '%s' to '%s'", current_addr, addr);
	}

 port:
	if (port) {
		char sign = '\0';
		char *endptr;

		if (addr)
			chunk_appendf(msg, ", ");

		/* collecting data currently setup */
		current_port = s->svc_port;

		/* check if PORT change is required */
		port_change_required = 0;

		sign = *port;
		errno = 0;
		new_port = strtol(port, &endptr, 10);
		if ((errno != 0) || (port == endptr)) {
			chunk_appendf(msg, "problem converting port '%s' to an int", port);
			goto out;
		}

		/* check if caller triggers a port mapped or offset */
		if (sign == '-' || (sign == '+')) {
			/* check if server currently uses port map */
			if (!(s->flags & SRV_F_MAPPORTS)) {
				/* switch from fixed port to port map mandatorily triggers
				 * a port change */
				port_change_required = 1;
				/* check is configured
				 * we're switching from a fixed port to a SRV_F_MAPPORTS (mapped) port
				 * prevent PORT change if check doesn't have it's dedicated port while switching
				 * to port mapping */
				if ((s->check.state & CHK_ST_CONFIGURED) && !(s->flags & SRV_F_CHECKPORT)) {
					chunk_appendf(msg, "can't change <port> to port map because it is incompatible with current health check port configuration (use 'port' statement from the 'server' directive.");
					goto out;
				}
			}
			/* we're already using port maps */
			else {
				port_change_required = current_port != new_port;
			}
		}
		/* fixed port */
		else {
			port_change_required = current_port != new_port;
		}

		/* applying PORT changes if required and update response message */
		if (port_change_required) {
			/* apply new port */
			s->svc_port = new_port;
			changed = 1;

			/* prepare message */
			chunk_appendf(msg, "port changed from '");
			if (s->flags & SRV_F_MAPPORTS)
				chunk_appendf(msg, "+");
			chunk_appendf(msg, "%d' to '", current_port);

			if (sign == '-') {
				s->flags |= SRV_F_MAPPORTS;
				chunk_appendf(msg, "%c", sign);
				/* just use for result output */
				new_port = -new_port;
			}
			else if (sign == '+') {
				s->flags |= SRV_F_MAPPORTS;
				chunk_appendf(msg, "%c", sign);
			}
			else {
				s->flags &= ~SRV_F_MAPPORTS;
			}

			chunk_appendf(msg, "%d'", new_port);

			/* we also need to update health checks port only if it uses server's realport */
			if ((s->check.state & CHK_ST_CONFIGURED) && !(s->flags & SRV_F_CHECKPORT)) {
				s->check.port = new_port;
			}
		}
		else {
			chunk_appendf(msg, "no need to change the port");
		}
	}

out:
	if (changed)
		srv_set_dyncookie(s);
	if (updater)
		chunk_appendf(msg, " by '%s'", updater);
	chunk_appendf(msg, "\n");
	return msg->str;
}


/*
 * update server status based on result of name resolution
 * returns:
 *  0 if server status is updated
 *  1 if server status has not changed
 */
int snr_update_srv_status(struct server *s, int has_no_ip)
{
	struct dns_resolvers  *resolvers  = s->resolvers;
	struct dns_resolution *resolution = s->dns_requester->resolution;
	int exp;

	switch (resolution->status) {
		case RSLV_STATUS_NONE:
			/* status when HAProxy has just (re)started.
			 * Nothing to do, since the task is already automatically started */
			break;

		case RSLV_STATUS_VALID:
			/*
			 * resume health checks
			 * server will be turned back on if health check is safe
			 */
			if (has_no_ip) {
				if (s->next_admin & SRV_ADMF_RMAINT)
					return 1;
				srv_set_admin_flag(s, SRV_ADMF_RMAINT,
				    "No IP for server ");
				return 0;
			}

			if (!(s->next_admin & SRV_ADMF_RMAINT))
				return 1;
			srv_clr_admin_flag(s, SRV_ADMF_RMAINT);
			chunk_printf(&trash, "Server %s/%s administratively READY thanks to valid DNS answer",
			             s->proxy->id, s->id);

			ha_warning("%s.\n", trash.str);
			send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);
			return 0;

		case RSLV_STATUS_NX:
			/* stop server if resolution is NX for a long enough period */
			exp = tick_add(resolution->last_valid, resolvers->hold.nx);
			if (!tick_is_expired(exp, now_ms))
				break;

			if (s->next_admin & SRV_ADMF_RMAINT)
				return 1;
			srv_set_admin_flag(s, SRV_ADMF_RMAINT, "DNS NX status");
			return 0;

		case RSLV_STATUS_TIMEOUT:
			/* stop server if resolution is TIMEOUT for a long enough period */
			exp = tick_add(resolution->last_valid, resolvers->hold.timeout);
			if (!tick_is_expired(exp, now_ms))
				break;

			if (s->next_admin & SRV_ADMF_RMAINT)
				return 1;
			srv_set_admin_flag(s, SRV_ADMF_RMAINT, "DNS timeout status");
			return 0;

		case RSLV_STATUS_REFUSED:
			/* stop server if resolution is REFUSED for a long enough period */
			exp = tick_add(resolution->last_valid, resolvers->hold.refused);
			if (!tick_is_expired(exp, now_ms))
				break;

			if (s->next_admin & SRV_ADMF_RMAINT)
				return 1;
			srv_set_admin_flag(s, SRV_ADMF_RMAINT, "DNS refused status");
			return 0;

		default:
			/* stop server if resolution failed for a long enough period */
			exp = tick_add(resolution->last_valid, resolvers->hold.other);
			if (!tick_is_expired(exp, now_ms))
				break;

			if (s->next_admin & SRV_ADMF_RMAINT)
				return 1;
			srv_set_admin_flag(s, SRV_ADMF_RMAINT, "unspecified DNS error");
			return 0;
	}

	return 1;
}

/*
 * Server Name Resolution valid response callback
 * It expects:
 *  - <nameserver>: the name server which answered the valid response
 *  - <response>: buffer containing a valid DNS response
 *  - <response_len>: size of <response>
 * It performs the following actions:
 *  - ignore response if current ip found and server family not met
 *  - update with first new ip found if family is met and current IP is not found
 * returns:
 *  0 on error
 *  1 when no error or safe ignore
 *
 * Must be called with server lock held
 */
int snr_resolution_cb(struct dns_requester *requester, struct dns_nameserver *nameserver)
{
	struct server *s = NULL;
	struct dns_resolution *resolution = NULL;
	void *serverip, *firstip;
	short server_sin_family, firstip_sin_family;
	int ret;
	struct chunk *chk = get_trash_chunk();
	int has_no_ip = 0;

	s = objt_server(requester->owner);
	if (!s)
		return 1;

	resolution = s->dns_requester->resolution;

	/* initializing variables */
	firstip = NULL;		/* pointer to the first valid response found */
				/* it will be used as the new IP if a change is required */
	firstip_sin_family = AF_UNSPEC;
	serverip = NULL;	/* current server IP address */

	/* initializing server IP pointer */
	server_sin_family = s->addr.ss_family;
	switch (server_sin_family) {
		case AF_INET:
			serverip = &((struct sockaddr_in *)&s->addr)->sin_addr.s_addr;
			break;

		case AF_INET6:
			serverip = &((struct sockaddr_in6 *)&s->addr)->sin6_addr.s6_addr;
			break;

		case AF_UNSPEC:
			break;

		default:
			goto invalid;
	}

	ret = dns_get_ip_from_response(&resolution->response, &s->dns_opts,
	                               serverip, server_sin_family, &firstip,
	                               &firstip_sin_family, s);

	switch (ret) {
		case DNS_UPD_NO:
			goto update_status;

		case DNS_UPD_SRVIP_NOT_FOUND:
			goto save_ip;

		case DNS_UPD_CNAME:
			goto invalid;

		case DNS_UPD_NO_IP_FOUND:
			has_no_ip = 1;
			goto update_status;

		case DNS_UPD_NAME_ERROR:
			/* update resolution status to OTHER error type */
			resolution->status = RSLV_STATUS_OTHER;
			goto update_status;

		default:
			goto invalid;

	}

 save_ip:
	if (nameserver) {
		nameserver->counters.update++;
		/* save the first ip we found */
		chunk_printf(chk, "%s/%s", nameserver->resolvers->id, nameserver->id);
	}
	else
		chunk_printf(chk, "DNS cache");
	update_server_addr(s, firstip, firstip_sin_family, (char *)chk->str);

 update_status:
	snr_update_srv_status(s, has_no_ip);
	return 1;

 invalid:
	if (nameserver) {
		nameserver->counters.invalid++;
		goto update_status;
	}
	snr_update_srv_status(s, has_no_ip);
	return 0;
}

/*
 * Server Name Resolution error management callback
 * returns:
 *  0 on error
 *  1 when no error or safe ignore
 */
int snr_resolution_error_cb(struct dns_requester *requester, int error_code)
{
	struct server *s;

	s = objt_server(requester->owner);
	if (!s)
		return 1;
	HA_SPIN_LOCK(SERVER_LOCK, &s->lock);
	snr_update_srv_status(s, 0);
	HA_SPIN_UNLOCK(SERVER_LOCK, &s->lock);
	return 1;
}

/*
 * Function to check if <ip> is already affected to a server in the backend
 * which owns <srv> and is up.
 * It returns a pointer to the first server found or NULL if <ip> is not yet
 * assigned.
 *
 * Must be called with server lock held
 */
struct server *snr_check_ip_callback(struct server *srv, void *ip, unsigned char *ip_family)
{
	struct server *tmpsrv;
	struct proxy *be;

	if (!srv)
		return NULL;

	be = srv->proxy;
	for (tmpsrv = be->srv; tmpsrv; tmpsrv = tmpsrv->next) {
		/* we found the current server is the same, ignore it */
		if (srv == tmpsrv)
			continue;

		/* We want to compare the IP in the record with the IP of the servers in the
		 * same backend, only if:
		 *   * DNS resolution is enabled on the server
		 *   * the hostname used for the resolution by our server is the same than the
		 *     one used for the server found in the backend
		 *   * the server found in the backend is not our current server
		 */
		HA_SPIN_LOCK(SERVER_LOCK, &tmpsrv->lock);
		if ((tmpsrv->hostname_dn == NULL) ||
		    (srv->hostname_dn_len != tmpsrv->hostname_dn_len) ||
		    (strcmp(srv->hostname_dn, tmpsrv->hostname_dn) != 0) ||
		    (srv->puid == tmpsrv->puid)) {
			HA_SPIN_UNLOCK(SERVER_LOCK, &tmpsrv->lock);
			continue;
		}

		/* If the server has been taken down, don't consider it */
		if (tmpsrv->next_admin & SRV_ADMF_RMAINT) {
			HA_SPIN_UNLOCK(SERVER_LOCK, &tmpsrv->lock);
			continue;
		}

		/* At this point, we have 2 different servers using the same DNS hostname
		 * for their respective resolution.
		 */
		if (*ip_family == tmpsrv->addr.ss_family &&
		    ((tmpsrv->addr.ss_family == AF_INET &&
		      memcmp(ip, &((struct sockaddr_in *)&tmpsrv->addr)->sin_addr, 4) == 0) ||
		     (tmpsrv->addr.ss_family == AF_INET6 &&
		      memcmp(ip, &((struct sockaddr_in6 *)&tmpsrv->addr)->sin6_addr, 16) == 0))) {
			HA_SPIN_UNLOCK(SERVER_LOCK, &tmpsrv->lock);
			return tmpsrv;
		}
		HA_SPIN_UNLOCK(SERVER_LOCK, &tmpsrv->lock);
	}


	return NULL;
}

/* Sets the server's address (srv->addr) from srv->hostname using the libc's
 * resolver. This is suited for initial address configuration. Returns 0 on
 * success otherwise a non-zero error code. In case of error, *err_code, if
 * not NULL, is filled up.
 */
int srv_set_addr_via_libc(struct server *srv, int *err_code)
{
	if (str2ip2(srv->hostname, &srv->addr, 1) == NULL) {
		if (err_code)
			*err_code |= ERR_WARN;
		return 1;
	}
	return 0;
}

/* Set the server's FDQN (->hostname) from <hostname>.
 * Returns -1 if failed, 0 if not.
 */
int srv_set_fqdn(struct server *srv, const char *hostname, int dns_locked)
{
	struct dns_resolution *resolution;
	char                  *hostname_dn;
	int                    hostname_len, hostname_dn_len;

	/* Note that the server lock is already held. */
	if (!srv->resolvers)
		return -1;

	if (!dns_locked)
		HA_SPIN_LOCK(DNS_LOCK, &srv->resolvers->lock);
	/* run time DNS resolution was not active for this server
	 * and we can't enable it at run time for now.
	 */
	if (!srv->dns_requester)
		goto err;

	chunk_reset(&trash);
	hostname_len    = strlen(hostname);
	hostname_dn     = trash.str;
	hostname_dn_len = dns_str_to_dn_label(hostname, hostname_len + 1,
					      hostname_dn, trash.size);
	if (hostname_dn_len == -1)
		goto err;

	resolution = srv->dns_requester->resolution;
	if (resolution &&
	    resolution->hostname_dn &&
	    !strcmp(resolution->hostname_dn, hostname_dn))
		goto end;

	dns_unlink_resolution(srv->dns_requester);

	free(srv->hostname);
	free(srv->hostname_dn);
	srv->hostname        = strdup(hostname);
	srv->hostname_dn     = strdup(hostname_dn);
	srv->hostname_dn_len = hostname_dn_len;
	if (!srv->hostname || !srv->hostname_dn)
		goto err;

	if (dns_link_resolution(srv, OBJ_TYPE_SERVER, 1) == -1)
		goto err;

  end:
	if (!dns_locked)
		HA_SPIN_UNLOCK(DNS_LOCK, &srv->resolvers->lock);
	return 0;

  err:
	if (!dns_locked)
		HA_SPIN_UNLOCK(DNS_LOCK, &srv->resolvers->lock);
	return -1;
}

/* Sets the server's address (srv->addr) from srv->lastaddr which was filled
 * from the state file. This is suited for initial address configuration.
 * Returns 0 on success otherwise a non-zero error code. In case of error,
 * *err_code, if not NULL, is filled up.
 */
static int srv_apply_lastaddr(struct server *srv, int *err_code)
{
	if (!str2ip2(srv->lastaddr, &srv->addr, 0)) {
		if (err_code)
			*err_code |= ERR_WARN;
		return 1;
	}
	return 0;
}

/* returns 0 if no error, otherwise a combination of ERR_* flags */
static int srv_iterate_initaddr(struct server *srv)
{
	int return_code = 0;
	int err_code;
	unsigned int methods;

	methods = srv->init_addr_methods;
	if (!methods) { // default to "last,libc"
		srv_append_initaddr(&methods, SRV_IADDR_LAST);
		srv_append_initaddr(&methods, SRV_IADDR_LIBC);
	}

	/* "-dr" : always append "none" so that server addresses resolution
	 * failures are silently ignored, this is convenient to validate some
	 * configs out of their environment.
	 */
	if (global.tune.options & GTUNE_RESOLVE_DONTFAIL)
		srv_append_initaddr(&methods, SRV_IADDR_NONE);

	while (methods) {
		err_code = 0;
		switch (srv_get_next_initaddr(&methods)) {
		case SRV_IADDR_LAST:
			if (!srv->lastaddr)
				continue;
			if (srv_apply_lastaddr(srv, &err_code) == 0)
				goto out;
			return_code |= err_code;
			break;

		case SRV_IADDR_LIBC:
			if (!srv->hostname)
				continue;
			if (srv_set_addr_via_libc(srv, &err_code) == 0)
				goto out;
			return_code |= err_code;
			break;

		case SRV_IADDR_NONE:
			srv_set_admin_flag(srv, SRV_ADMF_RMAINT, NULL);
			if (return_code) {
				ha_warning("parsing [%s:%d] : 'server %s' : could not resolve address '%s', disabling server.\n",
					   srv->conf.file, srv->conf.line, srv->id, srv->hostname);
			}
			return return_code;

		case SRV_IADDR_IP:
			ipcpy(&srv->init_addr, &srv->addr);
			if (return_code) {
				ha_warning("parsing [%s:%d] : 'server %s' : could not resolve address '%s', falling back to configured address.\n",
					   srv->conf.file, srv->conf.line, srv->id, srv->hostname);
			}
			goto out;

		default: /* unhandled method */
			break;
		}
	}

	if (!return_code) {
		ha_alert("parsing [%s:%d] : 'server %s' : no method found to resolve address '%s'\n",
		      srv->conf.file, srv->conf.line, srv->id, srv->hostname);
	}
	else {
		ha_alert("parsing [%s:%d] : 'server %s' : could not resolve address '%s'.\n",
		      srv->conf.file, srv->conf.line, srv->id, srv->hostname);
	}

	return_code |= ERR_ALERT | ERR_FATAL;
	return return_code;
out:
	srv_set_dyncookie(srv);
	return return_code;
}

/*
 * This function parses all backends and all servers within each backend
 * and performs servers' addr resolution based on information provided by:
 *   - configuration file
 *   - server-state file (states provided by an 'old' haproxy process)
 *
 * Returns 0 if no error, otherwise, a combination of ERR_ flags.
 */
int srv_init_addr(void)
{
	struct proxy *curproxy;
	int return_code = 0;

	curproxy = proxies_list;
	while (curproxy) {
		struct server *srv;

		/* servers are in backend only */
		if (!(curproxy->cap & PR_CAP_BE))
			goto srv_init_addr_next;

		for (srv = curproxy->srv; srv; srv = srv->next)
			if (srv->hostname)
				return_code |= srv_iterate_initaddr(srv);

 srv_init_addr_next:
		curproxy = curproxy->next;
	}

	return return_code;
}

const char *update_server_fqdn(struct server *server, const char *fqdn, const char *updater, int dns_locked)
{

	struct chunk *msg;

	msg = get_trash_chunk();
	chunk_reset(msg);

	if (server->hostname && !strcmp(fqdn, server->hostname)) {
		chunk_appendf(msg, "no need to change the FDQN");
		goto out;
	}

	if (strlen(fqdn) > DNS_MAX_NAME_SIZE || invalid_domainchar(fqdn)) {
		chunk_appendf(msg, "invalid fqdn '%s'", fqdn);
		goto out;
	}

	chunk_appendf(msg, "%s/%s changed its FQDN from %s to %s",
	              server->proxy->id, server->id, server->hostname, fqdn);

	if (srv_set_fqdn(server, fqdn, dns_locked) < 0) {
		chunk_reset(msg);
		chunk_appendf(msg, "could not update %s/%s FQDN",
		              server->proxy->id, server->id);
		goto out;
	}

	/* Flag as FQDN set from stats socket. */
	server->next_admin |= SRV_ADMF_HMAINT;

 out:
	if (updater)
		chunk_appendf(msg, " by '%s'", updater);
	chunk_appendf(msg, "\n");

	return msg->str;
}


/* Expects to find a backend and a server in <arg> under the form <backend>/<server>,
 * and returns the pointer to the server. Otherwise, display adequate error messages
 * on the CLI, sets the CLI's state to CLI_ST_PRINT and returns NULL. This is only
 * used for CLI commands requiring a server name.
 * Important: the <arg> is modified to remove the '/'.
 */
struct server *cli_find_server(struct appctx *appctx, char *arg)
{
	struct proxy *px;
	struct server *sv;
	char *line;

	/* split "backend/server" and make <line> point to server */
	for (line = arg; *line; line++)
		if (*line == '/') {
			*line++ = '\0';
			break;
		}

	if (!*line || !*arg) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Require 'backend/server'.\n";
		appctx->st0 = CLI_ST_PRINT;
		return NULL;
	}

	if (!get_backend_server(arg, line, &px, &sv)) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = px ? "No such server.\n" : "No such backend.\n";
		appctx->st0 = CLI_ST_PRINT;
		return NULL;
	}

	if (px->state == PR_STSTOPPED) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Proxy is disabled.\n";
		appctx->st0 = CLI_ST_PRINT;
		return NULL;
	}

	return sv;
}


static int cli_parse_set_server(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;
	const char *warning;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);

	if (strcmp(args[3], "weight") == 0) {
		warning = server_parse_weight_change_request(sv, args[4]);
		if (warning) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = warning;
			appctx->st0 = CLI_ST_PRINT;
		}
	}
	else if (strcmp(args[3], "state") == 0) {
		if (strcmp(args[4], "ready") == 0)
			srv_adm_set_ready(sv);
		else if (strcmp(args[4], "drain") == 0)
			srv_adm_set_drain(sv);
		else if (strcmp(args[4], "maint") == 0)
			srv_adm_set_maint(sv);
		else {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "'set server <srv> state' expects 'ready', 'drain' and 'maint'.\n";
			appctx->st0 = CLI_ST_PRINT;
		}
	}
	else if (strcmp(args[3], "health") == 0) {
		if (sv->track) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "cannot change health on a tracking server.\n";
			appctx->st0 = CLI_ST_PRINT;
		}
		else if (strcmp(args[4], "up") == 0) {
			sv->check.health = sv->check.rise + sv->check.fall - 1;
			srv_set_running(sv, "changed from CLI", NULL);
		}
		else if (strcmp(args[4], "stopping") == 0) {
			sv->check.health = sv->check.rise + sv->check.fall - 1;
			srv_set_stopping(sv, "changed from CLI", NULL);
		}
		else if (strcmp(args[4], "down") == 0) {
			sv->check.health = 0;
			srv_set_stopped(sv, "changed from CLI", NULL);
		}
		else {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "'set server <srv> health' expects 'up', 'stopping', or 'down'.\n";
			appctx->st0 = CLI_ST_PRINT;
		}
	}
	else if (strcmp(args[3], "agent") == 0) {
		if (!(sv->agent.state & CHK_ST_ENABLED)) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "agent checks are not enabled on this server.\n";
			appctx->st0 = CLI_ST_PRINT;
		}
		else if (strcmp(args[4], "up") == 0) {
			sv->agent.health = sv->agent.rise + sv->agent.fall - 1;
			srv_set_running(sv, "changed from CLI", NULL);
		}
		else if (strcmp(args[4], "down") == 0) {
			sv->agent.health = 0;
			srv_set_stopped(sv, "changed from CLI", NULL);
		}
		else {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "'set server <srv> agent' expects 'up' or 'down'.\n";
			appctx->st0 = CLI_ST_PRINT;
		}
	}
	else if (strcmp(args[3], "agent-addr") == 0) {
		if (!(sv->agent.state & CHK_ST_ENABLED)) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "agent checks are not enabled on this server.\n";
			appctx->st0 = CLI_ST_PRINT;
		} else {
			if (str2ip(args[4], &sv->agent.addr) == NULL) {
				appctx->ctx.cli.severity = LOG_ERR;
				appctx->ctx.cli.msg = "incorrect addr address given for agent.\n";
				appctx->st0 = CLI_ST_PRINT;
			}
		}
	}
	else if (strcmp(args[3], "agent-send") == 0) {
		if (!(sv->agent.state & CHK_ST_ENABLED)) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "agent checks are not enabled on this server.\n";
			appctx->st0 = CLI_ST_PRINT;
		} else {
			char *nss = strdup(args[4]);
			if (!nss) {
				appctx->ctx.cli.severity = LOG_ERR;
				appctx->ctx.cli.msg = "cannot allocate memory for new string.\n";
				appctx->st0 = CLI_ST_PRINT;
			} else {
				free(sv->agent.send_string);
				sv->agent.send_string = nss;
				sv->agent.send_string_len = strlen(args[4]);
			}
		}
	}
	else if (strcmp(args[3], "check-port") == 0) {
		int i = 0;
		if (strl2irc(args[4], strlen(args[4]), &i) != 0) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "'set server <srv> check-port' expects an integer as argument.\n";
			appctx->st0 = CLI_ST_PRINT;
			goto out_unlock;
		}
		if ((i < 0) || (i > 65535)) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "provided port is not valid.\n";
			appctx->st0 = CLI_ST_PRINT;
			goto out_unlock;
		}
		/* prevent the update of port to 0 if MAPPORTS are in use */
		if ((sv->flags & SRV_F_MAPPORTS) && (i == 0)) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "can't unset 'port' since MAPPORTS is in use.\n";
			appctx->st0 = CLI_ST_PRINT;
			goto out_unlock;
		}
		sv->check.port = i;
		appctx->ctx.cli.severity = LOG_NOTICE;
		appctx->ctx.cli.msg = "health check port updated.\n";
		appctx->st0 = CLI_ST_PRINT;
	}
	else if (strcmp(args[3], "addr") == 0) {
		char *addr = NULL;
		char *port = NULL;
		if (strlen(args[4]) == 0) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "set server <b>/<s> addr requires an address and optionally a port.\n";
			appctx->st0 = CLI_ST_PRINT;
			goto out_unlock;
		}
		else {
			addr = args[4];
		}
		if (strcmp(args[5], "port") == 0) {
			port = args[6];
		}
		warning = update_server_addr_port(sv, addr, port, "stats socket command");
		if (warning) {
			appctx->ctx.cli.severity = LOG_WARNING;
			appctx->ctx.cli.msg = warning;
			appctx->st0 = CLI_ST_PRINT;
		}
		srv_clr_admin_flag(sv, SRV_ADMF_RMAINT);
	}
	else if (strcmp(args[3], "fqdn") == 0) {
		if (!*args[4]) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "set server <b>/<s> fqdn requires a FQDN.\n";
			appctx->st0 = CLI_ST_PRINT;
			goto out_unlock;
		}
		warning = update_server_fqdn(sv, args[4], "stats socket command", 0);
		if (warning) {
			appctx->ctx.cli.severity = LOG_WARNING;
			appctx->ctx.cli.msg = warning;
			appctx->st0 = CLI_ST_PRINT;
		}
	}
	else {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "'set server <srv>' only supports 'agent', 'health', 'state', 'weight', 'addr', 'fqdn' and 'check-port'.\n";
		appctx->st0 = CLI_ST_PRINT;
	}
 out_unlock:
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

static int cli_parse_get_weight(char **args, struct appctx *appctx, void *private)
{
	struct stream_interface *si = appctx->owner;
	struct proxy *px;
	struct server *sv;
	char *line;


	/* split "backend/server" and make <line> point to server */
	for (line = args[2]; *line; line++)
		if (*line == '/') {
			*line++ = '\0';
			break;
		}

	if (!*line) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Require 'backend/server'.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	if (!get_backend_server(args[2], line, &px, &sv)) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = px ? "No such server.\n" : "No such backend.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	/* return server's effective weight at the moment */
	snprintf(trash.str, trash.size, "%d (initial %d)\n", sv->uweight, sv->iweight);
	if (ci_putstr(si_ic(si), trash.str) == -1) {
		si_applet_cant_put(si);
		return 0;
	}
	return 1;
}

/* Parse a "set weight" command.
 *
 * Grabs the server lock.
 */
static int cli_parse_set_weight(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;
	const char *warning;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);

	warning = server_parse_weight_change_request(sv, args[3]);
	if (warning) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = warning;
		appctx->st0 = CLI_ST_PRINT;
	}

	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);

	return 1;
}

/* parse a "set maxconn server" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_set_maxconn_server(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;
	const char *warning;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[3]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);

	warning = server_parse_maxconn_change_request(sv, args[4]);
	if (warning) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = warning;
		appctx->st0 = CLI_ST_PRINT;
	}

	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);

	return 1;
}

/* parse a "disable agent" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_disable_agent(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	sv->agent.state &= ~CHK_ST_ENABLED;
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* parse a "disable health" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_disable_health(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	sv->check.state &= ~CHK_ST_ENABLED;
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* parse a "disable server" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_disable_server(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	srv_adm_set_maint(sv);
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* parse a "enable agent" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_enable_agent(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	if (!(sv->agent.state & CHK_ST_CONFIGURED)) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Agent was not configured on this server, cannot enable.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	sv->agent.state |= CHK_ST_ENABLED;
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* parse a "enable health" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_enable_health(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	sv->check.state |= CHK_ST_ENABLED;
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* parse a "enable server" command. It always returns 1.
 *
 * Grabs the server lock.
 */
static int cli_parse_enable_server(char **args, struct appctx *appctx, void *private)
{
	struct server *sv;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[2]);
	if (!sv)
		return 1;

	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	srv_adm_set_ready(sv);
	if (!(sv->flags & SRV_F_COOKIESET)
	    && (sv->proxy->ck_opts & PR_CK_DYNAMIC) &&
	    sv->cookie)
		srv_check_for_dup_dyncookie(sv);
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* register cli keywords */
static struct cli_kw_list cli_kws = {{ },{
	{ { "disable", "agent",  NULL }, "disable agent  : disable agent checks (use 'set server' instead)", cli_parse_disable_agent, NULL },
	{ { "disable", "health",  NULL }, "disable health : disable health checks (use 'set server' instead)", cli_parse_disable_health, NULL },
	{ { "disable", "server",  NULL }, "disable server : disable a server for maintenance (use 'set server' instead)", cli_parse_disable_server, NULL },
	{ { "enable", "agent",  NULL }, "enable agent   : enable agent checks (use 'set server' instead)", cli_parse_enable_agent, NULL },
	{ { "enable", "health",  NULL }, "enable health  : enable health checks (use 'set server' instead)", cli_parse_enable_health, NULL },
	{ { "enable", "server",  NULL }, "enable server  : enable a disabled server (use 'set server' instead)", cli_parse_enable_server, NULL },
	{ { "set", "maxconn", "server",  NULL }, "set maxconn server : change a server's maxconn setting", cli_parse_set_maxconn_server, NULL },
	{ { "set", "server", NULL }, "set server     : change a server's state, weight or address",  cli_parse_set_server },
	{ { "get", "weight", NULL }, "get weight     : report a server's current weight",  cli_parse_get_weight },
	{ { "set", "weight", NULL }, "set weight     : change a server's weight (deprecated)",  cli_parse_set_weight },

	{{},}
}};

__attribute__((constructor))
static void __server_init(void)
{
	HA_SPIN_INIT(&updated_servers_lock);
	cli_register_kw(&cli_kws);
}

/*
 * This function applies server's status changes, it is
 * is designed to be called asynchronously.
 *
 */
void srv_update_status(struct server *s)
{
	struct check *check = &s->check;
	int xferred;
	struct proxy *px = s->proxy;
	int prev_srv_count = s->proxy->srv_bck + s->proxy->srv_act;
	int srv_was_stopping = (s->cur_state == SRV_ST_STOPPING) || (s->cur_admin & SRV_ADMF_DRAIN);
	int log_level;
	struct chunk *tmptrash = NULL;


	/* If currently main is not set we try to apply pending state changes */
	if (!(s->cur_admin & SRV_ADMF_MAINT)) {
		int next_admin;

		/* Backup next admin */
		next_admin = s->next_admin;

		/* restore current admin state */
		s->next_admin = s->cur_admin;

		if ((s->cur_state != SRV_ST_STOPPED) && (s->next_state == SRV_ST_STOPPED)) {
			s->last_change = now.tv_sec;
			if (s->proxy->lbprm.set_server_status_down)
				s->proxy->lbprm.set_server_status_down(s);

			if (s->onmarkeddown & HANA_ONMARKEDDOWN_SHUTDOWNSESSIONS)
				srv_shutdown_streams(s, SF_ERR_DOWN);

			/* we might have streams queued on this server and waiting for
			 * a connection. Those which are redispatchable will be queued
			 * to another server or to the proxy itself.
			 */
			xferred = pendconn_redistribute(s);

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s is DOWN", s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id);

				srv_append_status(tmptrash, s, NULL, xferred, 0);
				ha_warning("%s.\n", tmptrash->str);

				/* we don't send an alert if the server was previously paused */
				log_level = srv_was_stopping ? LOG_NOTICE : LOG_ALERT;
				send_log(s->proxy, log_level, "%s.\n", tmptrash->str);
				send_email_alert(s, log_level, "%s", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}
			if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
				set_backend_down(s->proxy);

			s->counters.down_trans++;
		}
		else if ((s->cur_state != SRV_ST_STOPPING) && (s->next_state == SRV_ST_STOPPING)) {
			s->last_change = now.tv_sec;
			if (s->proxy->lbprm.set_server_status_down)
				s->proxy->lbprm.set_server_status_down(s);

			/* we might have streams queued on this server and waiting for
			 * a connection. Those which are redispatchable will be queued
			 * to another server or to the proxy itself.
			 */
			xferred = pendconn_redistribute(s);

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s is stopping", s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id);

				srv_append_status(tmptrash, s, NULL, xferred, 0);

				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}

			if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
				set_backend_down(s->proxy);
		}
		else if (((s->cur_state != SRV_ST_RUNNING) && (s->next_state == SRV_ST_RUNNING))
			 || ((s->cur_state != SRV_ST_STARTING) && (s->next_state == SRV_ST_STARTING))) {
			if (s->proxy->srv_bck == 0 && s->proxy->srv_act == 0) {
				if (s->proxy->last_change < now.tv_sec)		// ignore negative times
					s->proxy->down_time += now.tv_sec - s->proxy->last_change;
				s->proxy->last_change = now.tv_sec;
			}

			if (s->next_state == SRV_ST_STOPPED && s->last_change < now.tv_sec)	// ignore negative times
				s->down_time += now.tv_sec - s->last_change;

			s->last_change = now.tv_sec;
			if (s->next_state == SRV_ST_STARTING)
				task_schedule(s->warmup, tick_add(now_ms, MS_TO_TICKS(MAX(1000, s->slowstart / 20))));

			server_recalc_eweight(s);
			/* now propagate the status change to any LB algorithms */
			if (px->lbprm.update_server_eweight)
				px->lbprm.update_server_eweight(s);
			else if (srv_willbe_usable(s)) {
				if (px->lbprm.set_server_status_up)
					px->lbprm.set_server_status_up(s);
			}
			else {
				if (px->lbprm.set_server_status_down)
					px->lbprm.set_server_status_down(s);
			}

			/* If the server is set with "on-marked-up shutdown-backup-sessions",
			 * and it's not a backup server and its effective weight is > 0,
			 * then it can accept new connections, so we shut down all streams
			 * on all backup servers.
			 */
			if ((s->onmarkedup & HANA_ONMARKEDUP_SHUTDOWNBACKUPSESSIONS) &&
			    !(s->flags & SRV_F_BACKUP) && s->next_eweight)
				srv_shutdown_backup_streams(s->proxy, SF_ERR_UP);

			/* check if we can handle some connections queued at the proxy. We
			 * will take as many as we can handle.
			 */
			xferred = pendconn_grab_from_px(s);

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s is UP", s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id);

				srv_append_status(tmptrash, s, NULL, xferred, 0);
				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				send_email_alert(s, LOG_NOTICE, "%s", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}

			if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
				set_backend_down(s->proxy);
		}
		else if (s->cur_eweight != s->next_eweight) {
			/* now propagate the status change to any LB algorithms */
			if (px->lbprm.update_server_eweight)
				px->lbprm.update_server_eweight(s);
			else if (srv_willbe_usable(s)) {
				if (px->lbprm.set_server_status_up)
					px->lbprm.set_server_status_up(s);
			}
			else {
				if (px->lbprm.set_server_status_down)
					px->lbprm.set_server_status_down(s);
			}

			if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
				set_backend_down(s->proxy);
		}

		s->next_admin = next_admin;
	}

	/* reset operational state change */
	*s->op_st_chg.reason = 0;
	s->op_st_chg.status = s->op_st_chg.code = -1;
	s->op_st_chg.duration = 0;

	/* Now we try to apply pending admin changes */

	/* Maintenance must also disable health checks */
	if (!(s->cur_admin & SRV_ADMF_MAINT) && (s->next_admin & SRV_ADMF_MAINT)) {
		if (s->check.state & CHK_ST_ENABLED) {
			s->check.state |= CHK_ST_PAUSED;
			check->health = 0;
		}

		if (s->cur_state == SRV_ST_STOPPED) {	/* server was already down */
			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				    "%sServer %s/%s was DOWN and now enters maintenance%s%s%s",
				    s->flags & SRV_F_BACKUP ? "Backup " : "", s->proxy->id, s->id,
				    *(s->adm_st_chg_cause) ? " (" : "", s->adm_st_chg_cause, *(s->adm_st_chg_cause) ? ")" : "");

				srv_append_status(tmptrash, s, NULL, -1, (s->next_admin & SRV_ADMF_FMAINT));

				if (!(global.mode & MODE_STARTING)) {
					ha_warning("%s.\n", tmptrash->str);
					send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				}
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}
			/* commit new admin status */

			s->cur_admin = s->next_admin;
		}
		else {	/* server was still running */
			check->health = 0; /* failure */
			s->last_change = now.tv_sec;

			s->next_state = SRV_ST_STOPPED;
			if (s->proxy->lbprm.set_server_status_down)
				s->proxy->lbprm.set_server_status_down(s);

			if (s->onmarkeddown & HANA_ONMARKEDDOWN_SHUTDOWNSESSIONS)
				srv_shutdown_streams(s, SF_ERR_DOWN);

			/* we might have streams queued on this server and waiting for
			 * a connection. Those which are redispatchable will be queued
			 * to another server or to the proxy itself.
			 */
			xferred = pendconn_redistribute(s);

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s is going DOWN for maintenance%s%s%s",
				             s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id,
				             *(s->adm_st_chg_cause) ? " (" : "", s->adm_st_chg_cause, *(s->adm_st_chg_cause) ? ")" : "");

				srv_append_status(tmptrash, s, NULL, xferred, (s->next_admin & SRV_ADMF_FMAINT));

				if (!(global.mode & MODE_STARTING)) {
					ha_warning("%s.\n", tmptrash->str);
					send_log(s->proxy, srv_was_stopping ? LOG_NOTICE : LOG_ALERT, "%s.\n", tmptrash->str);
				}
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}
			if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
				set_backend_down(s->proxy);

			s->counters.down_trans++;
		}
	}
	else if ((s->cur_admin & SRV_ADMF_MAINT) && !(s->next_admin & SRV_ADMF_MAINT)) {
		/* OK here we're leaving maintenance, we have many things to check,
		 * because the server might possibly be coming back up depending on
		 * its state. In practice, leaving maintenance means that we should
		 * immediately turn to UP (more or less the slowstart) under the
		 * following conditions :
		 *   - server is neither checked nor tracked
		 *   - server tracks another server which is not checked
		 *   - server tracks another server which is already up
		 * Which sums up as something simpler :
		 * "either the tracking server is up or the server's checks are disabled
		 * or up". Otherwise we only re-enable health checks. There's a special
		 * case associated to the stopping state which can be inherited. Note
		 * that the server might still be in drain mode, which is naturally dealt
		 * with by the lower level functions.
		 */

		if (s->check.state & CHK_ST_ENABLED) {
			s->check.state &= ~CHK_ST_PAUSED;
			check->health = check->rise; /* start OK but check immediately */
		}

		if ((!s->track || s->track->next_state != SRV_ST_STOPPED) &&
		    (!(s->agent.state & CHK_ST_ENABLED) || (s->agent.health >= s->agent.rise)) &&
		    (!(s->check.state & CHK_ST_ENABLED) || (s->check.health >= s->check.rise))) {
			if (s->track && s->track->next_state == SRV_ST_STOPPING) {
				s->next_state = SRV_ST_STOPPING;
			}
			else {
				s->next_state = SRV_ST_STARTING;
				if (s->slowstart > 0)
					task_schedule(s->warmup, tick_add(now_ms, MS_TO_TICKS(MAX(1000, s->slowstart / 20))));
				else
					s->next_state = SRV_ST_RUNNING;
			}

		}

		tmptrash = alloc_trash_chunk();
		if (tmptrash) {
			if (!(s->next_admin & SRV_ADMF_FMAINT) && (s->cur_admin & SRV_ADMF_FMAINT)) {
				chunk_printf(tmptrash,
					     "%sServer %s/%s is %s/%s (leaving forced maintenance)",
					     s->flags & SRV_F_BACKUP ? "Backup " : "",
					     s->proxy->id, s->id,
					     (s->next_state == SRV_ST_STOPPED) ? "DOWN" : "UP",
					     (s->next_admin & SRV_ADMF_DRAIN) ? "DRAIN" : "READY");
			}
			if (!(s->next_admin & SRV_ADMF_RMAINT) && (s->cur_admin & SRV_ADMF_RMAINT)) {
				chunk_printf(tmptrash,
					     "%sServer %s/%s ('%s') is %s/%s (resolves again)",
					     s->flags & SRV_F_BACKUP ? "Backup " : "",
					     s->proxy->id, s->id, s->hostname,
					     (s->next_state == SRV_ST_STOPPED) ? "DOWN" : "UP",
					     (s->next_admin & SRV_ADMF_DRAIN) ? "DRAIN" : "READY");
			}
			if (!(s->next_admin & SRV_ADMF_IMAINT) && (s->cur_admin & SRV_ADMF_IMAINT)) {
				chunk_printf(tmptrash,
					     "%sServer %s/%s is %s/%s (leaving maintenance)",
					     s->flags & SRV_F_BACKUP ? "Backup " : "",
					     s->proxy->id, s->id,
					     (s->next_state == SRV_ST_STOPPED) ? "DOWN" : "UP",
					     (s->next_admin & SRV_ADMF_DRAIN) ? "DRAIN" : "READY");
			}
			ha_warning("%s.\n", tmptrash->str);
			send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
			free_trash_chunk(tmptrash);
			tmptrash = NULL;
		}

		server_recalc_eweight(s);
		/* now propagate the status change to any LB algorithms */
		if (px->lbprm.update_server_eweight)
			px->lbprm.update_server_eweight(s);
		else if (srv_willbe_usable(s)) {
			if (px->lbprm.set_server_status_up)
				px->lbprm.set_server_status_up(s);
		}
		else {
			if (px->lbprm.set_server_status_down)
				px->lbprm.set_server_status_down(s);
		}

		if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
			set_backend_down(s->proxy);

		/* If the server is set with "on-marked-up shutdown-backup-sessions",
		 * and it's not a backup server and its effective weight is > 0,
		 * then it can accept new connections, so we shut down all streams
		 * on all backup servers.
		 */
		if ((s->onmarkedup & HANA_ONMARKEDUP_SHUTDOWNBACKUPSESSIONS) &&
		    !(s->flags & SRV_F_BACKUP) && s->next_eweight)
			srv_shutdown_backup_streams(s->proxy, SF_ERR_UP);

		/* check if we can handle some connections queued at the proxy. We
		 * will take as many as we can handle.
		 */
		xferred = pendconn_grab_from_px(s);
	}
	else if (s->next_admin & SRV_ADMF_MAINT) {
		/* remaining in maintenance mode, let's inform precisely about the
		 * situation.
		 */
		if (!(s->next_admin & SRV_ADMF_FMAINT) && (s->cur_admin & SRV_ADMF_FMAINT)) {
			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s is leaving forced maintenance but remains in maintenance",
				             s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id);

				if (s->track) /* normally it's mandatory here */
					chunk_appendf(tmptrash, " via %s/%s",
				              s->track->proxy->id, s->track->id);
				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}
		}
		if (!(s->next_admin & SRV_ADMF_RMAINT) && (s->cur_admin & SRV_ADMF_RMAINT)) {
			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s ('%s') resolves again but remains in maintenance",
				             s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id, s->hostname);

				if (s->track) /* normally it's mandatory here */
					chunk_appendf(tmptrash, " via %s/%s",
				              s->track->proxy->id, s->track->id);
				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}
		}
		else if (!(s->next_admin & SRV_ADMF_IMAINT) && (s->cur_admin & SRV_ADMF_IMAINT)) {
			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash,
				             "%sServer %s/%s remains in forced maintenance",
				             s->flags & SRV_F_BACKUP ? "Backup " : "",
				             s->proxy->id, s->id);
				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}
		}
		/* don't report anything when leaving drain mode and remaining in maintenance */

		s->cur_admin = s->next_admin;
	}

	if (!(s->next_admin & SRV_ADMF_MAINT)) {
		if (!(s->cur_admin & SRV_ADMF_DRAIN) && (s->next_admin & SRV_ADMF_DRAIN)) {
			/* drain state is applied only if not yet in maint */

			s->last_change = now.tv_sec;
			if (px->lbprm.set_server_status_down)
				px->lbprm.set_server_status_down(s);

			/* we might have streams queued on this server and waiting for
			 * a connection. Those which are redispatchable will be queued
			 * to another server or to the proxy itself.
			 */
			xferred = pendconn_redistribute(s);

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				chunk_printf(tmptrash, "%sServer %s/%s enters drain state%s%s%s",
					     s->flags & SRV_F_BACKUP ? "Backup " : "", s->proxy->id, s->id,
				             *(s->adm_st_chg_cause) ? " (" : "", s->adm_st_chg_cause, *(s->adm_st_chg_cause) ? ")" : "");

				srv_append_status(tmptrash, s, NULL, xferred, (s->next_admin & SRV_ADMF_FDRAIN));

				if (!(global.mode & MODE_STARTING)) {
					ha_warning("%s.\n", tmptrash->str);
					send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
					send_email_alert(s, LOG_NOTICE, "%s", tmptrash->str);
				}
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}

			if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
				set_backend_down(s->proxy);
		}
		else if ((s->cur_admin & SRV_ADMF_DRAIN) && !(s->next_admin & SRV_ADMF_DRAIN)) {
			/* OK completely leaving drain mode */
			if (s->proxy->srv_bck == 0 && s->proxy->srv_act == 0) {
				if (s->proxy->last_change < now.tv_sec)         // ignore negative times
					s->proxy->down_time += now.tv_sec - s->proxy->last_change;
				s->proxy->last_change = now.tv_sec;
			}

			if (s->last_change < now.tv_sec)                        // ignore negative times
				s->down_time += now.tv_sec - s->last_change;
			s->last_change = now.tv_sec;
			server_recalc_eweight(s);

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				if (!(s->next_admin & SRV_ADMF_FDRAIN)) {
					chunk_printf(tmptrash,
						     "%sServer %s/%s is %s (leaving forced drain)",
						     s->flags & SRV_F_BACKUP ? "Backup " : "",
					             s->proxy->id, s->id,
					             (s->next_state == SRV_ST_STOPPED) ? "DOWN" : "UP");
				}
				else {
					chunk_printf(tmptrash,
					             "%sServer %s/%s is %s (leaving drain)",
					             s->flags & SRV_F_BACKUP ? "Backup " : "",
						     s->proxy->id, s->id,
						     (s->next_state == SRV_ST_STOPPED) ? "DOWN" : "UP");
					if (s->track) /* normally it's mandatory here */
						chunk_appendf(tmptrash, " via %s/%s",
					s->track->proxy->id, s->track->id);
				}

				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}

			/* now propagate the status change to any LB algorithms */
			if (px->lbprm.update_server_eweight)
				px->lbprm.update_server_eweight(s);
			else if (srv_willbe_usable(s)) {
				if (px->lbprm.set_server_status_up)
					px->lbprm.set_server_status_up(s);
			}
			else {
				if (px->lbprm.set_server_status_down)
					px->lbprm.set_server_status_down(s);
			}
		}
		else if ((s->next_admin & SRV_ADMF_DRAIN)) {
			/* remaining in drain mode after removing one of its flags */

			tmptrash = alloc_trash_chunk();
			if (tmptrash) {
				if (!(s->next_admin & SRV_ADMF_FDRAIN)) {
					chunk_printf(tmptrash,
					             "%sServer %s/%s is leaving forced drain but remains in drain mode",
					             s->flags & SRV_F_BACKUP ? "Backup " : "",
					             s->proxy->id, s->id);

					if (s->track) /* normally it's mandatory here */
						chunk_appendf(tmptrash, " via %s/%s",
					              s->track->proxy->id, s->track->id);
				}
				else {
					chunk_printf(tmptrash,
					             "%sServer %s/%s remains in forced drain mode",
					             s->flags & SRV_F_BACKUP ? "Backup " : "",
					             s->proxy->id, s->id);
				}
				ha_warning("%s.\n", tmptrash->str);
				send_log(s->proxy, LOG_NOTICE, "%s.\n", tmptrash->str);
				free_trash_chunk(tmptrash);
				tmptrash = NULL;
			}

			/* commit new admin status */

			s->cur_admin = s->next_admin;
		}
	}

	/* Re-set log strings to empty */
	*s->adm_st_chg_cause = 0;
}
/*
 * This function loops on servers registered for asynchronous
 * status changes
 *
 * NOTE: No needs to lock <updated_servers> list because it is called inside the
 * sync point.
 */
void servers_update_status(void) {
	struct server *s, *stmp;

	list_for_each_entry_safe(s, stmp, &updated_servers, update_status) {
		srv_update_status(s);
		LIST_DEL(&s->update_status);
		LIST_INIT(&s->update_status);
	}
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
