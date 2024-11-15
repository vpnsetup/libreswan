/* Libreswan config file parser (confread.h)
 *
 * Copyright (C) 2001-2002 Mathieu Lafon - Arkoon Network Security
 * Copyright (C) 2009 Jose Quaresma <josequaresma@gmail.com>
 * Copyright (C) 2003-2006 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Antony Antony <antony@phenome.org>
 * Copyright (C) 2016, Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2019 D. Hugh Redelmeier <hugh@mimosa.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef _IPSEC_CONFREAD_H_
#define _IPSEC_CONFREAD_H_

#include <sys/queue.h>		/* for TAILQ_ENTRY() */

#include "ipsecconf/keywords.h"

#include "lset.h"
#include "err.h"
#include "ip_range.h"
#include "ip_subnet.h"
#include "ip_protoport.h"
#include "ip_cidr.h"
#include "lswcdefs.h"
#include "authby.h"

struct logger;

/*
 * Code tests <<set[flag] != k_set>> to detect either k_unset or
 * k_default and allow an override.
 */

enum keyword_set {
	k_unset   = false,
	k_set     = true,
	k_default = 2
};

typedef enum keyword_set keyword_set[KW_roof];

struct keyword_value {
	char *string;
	intmax_t option;
};

typedef struct keyword_value keyword_values[KW_roof];

/*
 * Note: string fields in struct starter_end and struct starter_conn
 * should correspond to STR_FIELD calls in copy_conn_default() and confread_free_conn.
 */

struct starter_end {
	const char *leftright;
	const struct ip_info *host_family;	/* XXX: move to starter_conn? */
	enum keyword_host addrtype;
	enum keyword_host nexttype;
	ip_address addr;
	ip_address nexthop;
	ip_cidr vti_ip;
	ip_protoport protoport;

	keyword_values values;
	keyword_set set;
};

/*
 * Note: string fields in struct starter_end and struct starter_conn
 * should correspond to STR_FIELD calls in copy_conn_default() and confread_free_conn.
 */

struct starter_conn {
	TAILQ_ENTRY(starter_conn) link;
	char *name;

	keyword_values values;
	keyword_set set;

	enum ike_version ike_version;
	struct authby authby;
	lset_t sighash_policy;
	enum shunt_policy shunt[SHUNT_KIND_ROOF];

	struct starter_end left, right;

	const struct ip_info *clientaddrfamily;

	enum {
		STATE_INVALID,
		STATE_LOADED,
		STATE_INCOMPLETE,
		STATE_ADDED,
		STATE_FAILED,
	} state;

	uint32_t xfrm_if_id;
};

struct starter_config {
	struct {
		keyword_set set;
	} setup;
	keyword_values values;

	/* conn %default */
	struct starter_conn conn_default;

	/* connections list (without %default) */
	TAILQ_HEAD(, starter_conn) conns;
};

extern struct config_parsed *parser_load_conf(const char *file,
					      struct logger *logger);
extern void parser_freeany_config_parsed(struct config_parsed **cfg);

extern struct starter_config *confread_load(const char *file,
					    bool setuponly,
					    struct logger *logger);

extern void confread_free(struct starter_config *cfg);

#endif /* _IPSEC_CONFREAD_H_ */
