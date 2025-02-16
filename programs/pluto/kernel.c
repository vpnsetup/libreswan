/* routines that interface with the kernel's IPsec mechanism, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2010  D. Hugh Redelmeier.
 * Copyright (C) 2003-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2007-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008-2010 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2010 Bart Trojanowski <bart@jukie.net>
 * Copyright (C) 2009-2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2010 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2012-2015 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Kim B. Heino <b@bbbs.net>
 * Copyright (C) 2016-2022 Andrew Cagney
 * Copyright (C) 2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
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

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>		/* for WIFEXITED() et.al. */
#include <unistd.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/thread.h>

#include "sysdep.h"
#include "constants.h"

#include "defs.h"
#include "rnd.h"
#include "id.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "timer.h"
#include "kernel.h"
#include "kernel_ops.h"
#include "kernel_xfrm.h"
#include "kernel_policy.h"
#include "packet.h"
#include "x509.h"
#include "pluto_x509.h"
#include "certs.h"
#include "secrets.h"
#include "log.h"
#include "server.h"
#include "whack.h"      /* for RC_LOG_SERIOUS */
#include "keys.h"
#include "ike_alg.h"
#include "ike_alg_encrypt.h"
#include "ike_alg_integ.h"

#include "packet.h"  /* for pb_stream in nat_traversal.h */
#include "nat_traversal.h"
#include "ip_address.h"
#include "ip_info.h"
#include "lswfips.h" /* for libreswan_fipsmode() */
#include "kernel_xfrm_interface.h"
#include "iface.h"
#include "ip_selector.h"
#include "ip_encap.h"
#include "show.h"
#include "rekeyfuzz.h"
#include "orient.h"
#include "kernel_alg.h"
#include "updown.h"
#include "pending.h"
#include "terminate.h"

static void delete_bare_shunt_kernel_policy(const struct bare_shunt *bsp,
					    enum expect_kernel_policy expect_kernel_policy,
					    struct logger *logger, where_t where);

/*
 * The priority assigned to a kernel policy.
 *
 * Lowest wins.
 */

const kernel_priority_t highest_kernel_priority = { .value = 0, };

kernel_priority_t calculate_kernel_priority(const struct connection *c)
{
	if (c->config->child_sa.priority != 0) {
		ldbg(c->logger,
		     "priority calculation overruled by connection specification of %ju (0x%jx)",
		     c->config->child_sa.priority, c->config->child_sa.priority);
		return (kernel_priority_t) { c->config->child_sa.priority, };
	}

	if (is_group(c)) {
		llog_pexpect(c->logger, HERE,
			     "priority calculation of connection skipped - group template does not install SPDs");
		return highest_kernel_priority;
	}

	/* XXX: assume unsigned >= 32-bits */
	PASSERT(c->logger, sizeof(unsigned) >= sizeof(uint32_t));

	/*
	 * Accumulate the priority.
	 *
	 * Add things most-important to least-important. Before ORing
	 * in the new bits, left-shift PRIO to make space.
	 */
	unsigned prio = 0;

	/* Determine the base priority (2 bits) (0 is manual by user). */
	unsigned base;
	if (is_group_instance(c)) {
		if (c->remote->host.config->authby.null) {
			base = 3; /* opportunistic anonymous */
		} else {
			base = 2; /* opportunistic */
		}
	} else {
		base = 1; /* static connection */
	}

	/* XXX: yes the shift is pointless (but it is consistent) */
	prio = (prio << 2) | base;

	/* Penalize wildcard ports (2 bits). */
	unsigned portsw =
		((c->spd->local->client.hport == 0 ? 1 : 0) +
		 (c->spd->remote->client.hport == 0 ? 1 : 0));
	prio = (prio << 2) | portsw;

	/* Penalize wildcard protocol (1 bit). */
	unsigned protow = c->spd->local->client.ipproto == 0 ? 1 : 0;
	prio = (prio << 1) | protow;

	/*
	 * For transport mode or /32 to /32, the client mask bits are
	 * set based on the host_addr parameters.
	 *
	 * A longer prefix wins over a shorter prefix, hence the
	 * reversal.  Value needs to fit 0-128, hence 8 bits.
	 */
	unsigned srcw = 128 - c->spd->local->client.maskbits;
	prio = (prio << 8) | srcw;
	unsigned dstw = 128 - c->spd->remote->client.maskbits;
	prio = (prio << 8) | dstw;

	/*
	 * Penalize template (1 bit).
	 *
	 * "Ensure an instance always has preference over it's
	 * template/OE-group always has preference."
	 */
	unsigned instw = (is_instance(c) ? 0 : 1);
	prio = (prio << 1) | instw;

	ldbg(c->logger,
	     "priority calculation of is %u (%#x) base=%u portsw=%u protow=%u, srcw=%u dstw=%u instw=%u",
	     prio, prio, base, portsw, protow, srcw, dstw, instw);
	return (kernel_priority_t) { prio, };
}

static global_timer_cb kernel_scan_shunts;

/*
 * Add an outbound bare kernel policy, aka shunt.
 *
 * Such a kernel policy determines the fate of packets without the use
 * of any SAs.  These are defaults, in effect.  If a negotiation has
 * not been attempted, use %trap.  If negotiation has failed, the
 * choice between %trap/%pass/%drop/%reject is specified in the policy
 * of connection c.
 *
 * The kernel policy is refered to as bare (naked, global) as it is
 * not paired with a kernel state.
 */

static bool install_prospective_kernel_policies(const struct spd_route *spd,
						enum shunt_kind shunt_kind,
						struct logger *logger, where_t where)
{
	const struct connection *c = spd->connection;

	LDBGP_JAMBUF(DBG_BASE, logger, buf) {
		jam(buf, "kernel: %s() ", __func__);

		jam_connection(buf, c);

		jam_string(buf, " ");
		jam_enum_short(buf, &shunt_kind_names, shunt_kind);

		jam(buf, " ");
		jam_selector_pair(buf, &spd->local->client, &spd->remote->client);

		jam(buf, " config.sec_label=");
		if (c->config->sec_label.len > 0) {
			jam_sanitized_hunk(buf, c->config->sec_label);
		}

		jam_string(buf, " ");
		jam_where(buf, where);
	}

	/*
	 * Only the following shunts are valid.
	 */
	PASSERT(logger, (shunt_kind == SHUNT_KIND_NEVER_NEGOTIATE ||
			 shunt_kind == SHUNT_KIND_ONDEMAND));

	/*
	 * Labeled ipsec has its own ondemand path.
	 */
	if (PBAD(logger, is_labeled(c))) {
		return false;
	}

	/*
	 * Only the following shunts are valid.
	 */
	FOR_EACH_THING(direction, DIRECTION_OUTBOUND, DIRECTION_INBOUND) {
		if (direction == DIRECTION_OUTBOUND ||
		    shunt_kind == SHUNT_KIND_NEVER_NEGOTIATE) {
			if (!add_spd_kernel_policy(spd, KERNEL_POLICY_OP_ADD,
						   direction,
						   shunt_kind,
						   logger, HERE,
						   "prospective kernel_policy")) {
				return false;
			}
		}
	}

	return true;
}

struct bare_shunt {
	ip_selector our_client;
	ip_selector peer_client;
	enum shunt_kind shunt_kind;
	enum shunt_policy shunt_policy;
	const struct ip_protocol *transport_proto; /* XXX: same value in local/remote */
	unsigned long count;
	monotime_t last_activity;

	/*
	 * Note: "why" must be in stable storage (not auto, not heap)
	 * because we use it indefinitely without copying or pfreeing.
	 * Simple rule: use a string literal.
	 */
	const char *why;

	/* The connection to restore when the bare_shunt expires.  */
	co_serial_t restore_serialno;

	struct bare_shunt *next;
};

static struct bare_shunt *bare_shunts = NULL;

#ifdef IPSEC_CONNECTION_LIMIT
int num_ipsec_eroute = 0;
#endif

static void jam_bare_shunt(struct jambuf *buf, const struct bare_shunt *bs)
{
	jam(buf, "bare shunt %p ", bs);
	jam_selector_pair(buf, &bs->our_client, &bs->peer_client);
	jam(buf, " => ");
	jam_enum_short(buf, &shunt_policy_names, bs->shunt_policy);
	jam(buf, "    %s", bs->why);
	if (bs->restore_serialno != UNSET_CO_SERIAL) {
		jam(buf, " "PRI_CO, pri_co(bs->restore_serialno));
	}
}

static void llog_bare_shunt(lset_t rc_flags, struct logger *logger,
			    const struct bare_shunt *bs, const char *op)
{
	LLOG_JAMBUF(rc_flags, logger, buf) {
		jam(buf, "%s ", op);
		jam_bare_shunt(buf, bs);
	}
}

static void ldbg_bare_shunt(const struct logger *logger, const char *op, const struct bare_shunt *bs)
{
	LDBGP_JAMBUF(DBG_BASE, logger, buf) {
		jam(buf, "%s ", op);
		jam_bare_shunt(buf, bs);
	}
}

/*
 * Note: "why" must be in stable storage (not auto, not heap) because
 * we use it indefinitely without copying or pfreeing.
 *
 * Simple rule: use a string literal.
 */

static struct bare_shunt *add_bare_shunt(const ip_selector *our_client,
					 const ip_selector *peer_client,
					 enum shunt_policy shunt_policy,
					 co_serial_t restore_serialno,
					 const char *why, struct logger *logger)
{
	/* report any duplication; this should NOT happen */
	struct bare_shunt **bspp = bare_shunt_ptr(our_client, peer_client, why);

	if (bspp != NULL) {
		/* maybe: passert(bsp == NULL); */
		llog_bare_shunt(RC_LOG, logger, *bspp,
				"CONFLICTING existing");
	}

	struct bare_shunt *bs = alloc_thing(struct bare_shunt, "bare shunt");

	bs->why = why;
	bs->our_client = *our_client;
	bs->peer_client = *peer_client;
	const struct ip_protocol *transport_proto = selector_protocol(*our_client);
	pexpect(transport_proto == selector_protocol(*peer_client));
	bs->transport_proto = transport_proto;
	bs->restore_serialno = restore_serialno;

	bs->shunt_policy = shunt_policy;
	bs->count = 0;
	bs->last_activity = mononow();

	bs->next = bare_shunts;
	bare_shunts = bs;
	ldbg_bare_shunt(logger, "add", bs);

	/* report duplication; this should NOT happen */
	if (bspp != NULL) {
		llog_bare_shunt(RC_LOG, logger, bs,
				"CONFLICTING      new");
	}

	return bs;
}

static reqid_t get_proto_reqid(reqid_t base, const struct ip_protocol *proto)
{
	if (proto == &ip_protocol_ipcomp)
		return reqid_ipcomp(base);

	if (proto == &ip_protocol_esp)
		return reqid_esp(base);

	if (proto == &ip_protocol_ah)
		return reqid_ah(base);

	llog_passert(&global_logger, HERE,
		     "bad protocol %s", proto->name);
}

static const char *said_str(const ip_address dst,
			    const struct ip_protocol *sa_proto,
			    ipsec_spi_t spi,
			    said_buf *buf)
{
	ip_said said = said_from_address_protocol_spi(dst, sa_proto, spi);
	return str_said(&said, buf);
}

ipsec_spi_t get_ipsec_spi(const struct connection *c,
			  const struct ip_protocol *proto,
			  ipsec_spi_t avoid,
			  struct logger *logger)
{
	passert(proto == &ip_protocol_ah || proto == &ip_protocol_esp);
	return kernel_ops_get_ipsec_spi(avoid,
					/*src*/&c->remote->host.addr,
					/*dst*/&c->local->host.addr,
					proto,
					get_proto_reqid(c->child.reqid, proto),
					IPSEC_DOI_SPI_OUR_MIN, 0xffffffffU,
					"SPI", logger);
}

/* Generate Unique CPI numbers.
 * The result is returned as an SPI (4 bytes) in network order!
 * The real bits are in the nework-low-order 2 bytes.
 * Modelled on get_ipsec_spi, but range is more limited:
 * 256-61439.
 * If we can't find one easily, return 0 (a bad SPI,
 * no matter what order) indicating failure.
 */
ipsec_spi_t get_ipsec_cpi(const struct connection *c, struct logger *logger)
{
	return kernel_ops_get_ipsec_spi(0,
					/*src*/&c->remote->host.addr,
					/*dst*/&c->local->host.addr,
					&ip_protocol_ipcomp,
					get_proto_reqid(c->child.reqid, &ip_protocol_ipcomp),
					IPCOMP_FIRST_NEGOTIATED,
					IPCOMP_LAST_NEGOTIATED,
					"CPI", logger);
}

/*
 * Build an array of encapsulation rules/tmpl.  Order things
 * inner-most to outer-most so the last entry is what will go across
 * the wire.  A -1 entry of the packet to be encapsulated is implied.
 */

struct kernel_route {
	enum encap_mode mode;
	struct {
		ip_address address; /* ip_endpoint? */
		ip_selector route; /* ip_address? */
	} src, dst;
};

static struct kernel_route kernel_route_from_state(const struct state *st, enum direction direction)
{
	const struct connection *c = st->st_connection;

	enum encap_mode mode = ENCAP_MODE_TRANSPORT;
	FOR_EACH_THING(proto, &st->st_esp, &st->st_ah) {
		if (proto->present && proto->attrs.mode == ENCAPSULATION_MODE_TUNNEL) {
			mode = ENCAP_MODE_TUNNEL;
			break;
		}
	}

	/*
	 * With pfkey and transport mode with nat-traversal we need to
	 * change the remote IPsec SA to point to external ip of the
	 * peer.  Here we substitute real client ip with NATD ip.
	 *
	 * Bug #1004 fix.
	 *
	 * There really isn't "client" with XFRM and transport mode so
	 * eroute must be done to natted, visible ip. If we don't hide
	 * internal IP, communication doesn't work.
	 */
	ip_selector local_route;
	ip_selector remote_route;
	const ip_selectors *local = &c->local->child.selectors.accepted;
	const ip_selectors *remote = &c->remote->child.selectors.accepted;
	switch (mode) {
	case ENCAP_MODE_TUNNEL:
		local_route = unset_selector;	/* XXX: kernel_policy has spd->client */
		remote_route = unset_selector;	/* XXX: kernel_policy has spd->client */
		break;
	case ENCAP_MODE_TRANSPORT:
		/*
		 * XXX: need to work around:
		 *
		 * - IKEv1 which is clueless to selectors.accepted
		 * - CP which skips setting TS
		 * - CK_PERMENANT that doesn't update TS
		 */
		local_route = (local->len > 0 ? local->list[0] :
			       c->spd->local->client);
		ip_selector remote_client = (remote->len > 0 ? remote->list[0] :
					     c->spd->remote->client);
		/* reroute remote to pair up with dest */
		remote_route = selector_from_address_protocol_port(c->remote->host.addr,
								   selector_protocol(remote_client),
								   selector_port(remote_client));
		break;
	default:
		bad_case(mode);
	}

	switch (direction) {
	case DIRECTION_INBOUND:
		return (struct kernel_route) {
			.mode = mode,
			.src.address = c->remote->host.addr,
			.dst.address = c->local->host.addr,
			.src.route = remote_route,
			.dst.route = local_route,
		};
	case DIRECTION_OUTBOUND:
		return (struct kernel_route) {
			.mode = mode,
			.src.address = c->local->host.addr,
			.dst.address = c->remote->host.addr,
			.src.route = local_route,
			.dst.route = remote_route,
		};
	default:
		bad_case(direction);
	}
}

PRINTF_LIKE(4)
static void ldbg_spd(struct logger *logger, unsigned indent,
			  const struct spd_route *spd,
			  const char *fmt, ...)
{
	LDBGP_JAMBUF(DBG_BASE, logger, buf) {
		jam(buf, "%*s", indent, "");
		jam_string(buf, " ");
		jam_enum_short(buf, &routing_names, spd->connection->child.routing);
		jam_string(buf, " ");
		jam_selector_pair(buf, &spd->local->client, &spd->remote->client);
		jam_string(buf, " ");
		jam_connection(buf, spd->connection);
		jam_string(buf, " ");
		va_list ap;
		va_start(ap, fmt);
		jam_va_list(buf, fmt, ap);
		va_end(ap);
	}
}

static const struct spd_route *raw_spd_owner(const ip_selector *local,
					     const struct spd_route *c_spd,
					     enum routing min_routing,
					     struct logger *logger,
					     where_t where,
					     const char *func,
					     unsigned indent)
{
	const ip_selector *remote = &c_spd->remote->client;

	selector_pair_buf spb;
	ldbg(logger, "%*s%s() looking for SPD owner of %s with routing >= %s",
	     indent, "", func,
	     str_selector_pair(local, remote, &spb),
	     enum_name_short(&routing_names, min_routing));

	const struct spd_route *owner = NULL;

	struct spd_route_filter srf = {
		.remote_client_range = remote,
		.where = where,
	};

	indent += 2;
	while (next_spd_route(NEW2OLD, &srf)) {
		struct spd_route *d_spd = srf.spd;
		struct connection *d = d_spd->connection;

		/*
		 * Pprune out anything that isn't conflicting
		 * according to selectors.
		 *
		 * Yes, conflicting is vague.  A good starting point
		 * is to look at what the kernel needs when it is
		 * deleting a policy.  For instance, the selectors
		 * matter, the rules (templ) do not,
		 */

		if (!oriented(d)) {
			/* can happen during shutdown */
			ldbg_spd(logger, indent, d_spd, "skipped; not oriented");
			continue;
		}

		if (c_spd == d_spd) {
			ldbg_spd(logger, indent, d_spd, "skipped; ignoring self");
			continue;
		}

		if (d->child.routing < min_routing) {
			ldbg_spd(logger, indent, d_spd, "skipped; %s is insufficient routing",
				 enum_name_short(&routing_names, min_routing));
			continue;
		}

		/* fast lookup did it's job! */
		PEXPECT(logger, selector_range_eq_selector_range(*remote,
								 d_spd->remote->client));
		if (!selector_eq_selector(*remote, d_spd->remote->client)) {
			ldbg_spd(logger, indent, d_spd, "skipped; different remote selectors");
			continue;
		}

		if (!selector_eq_selector(*local, d_spd->local->client)) {
			ldbg_spd(logger, indent, d_spd, "skipped; different local selectors");
			continue;
		}

		/*
		 * Consider SPDs to be different when the either in or
		 * out marks differ (after masking).
		 */

		const struct sa_marks *sa_marks = &c_spd->connection->sa_marks;
		if ((sa_marks->in.val & sa_marks->in.mask) != (d->sa_marks.in.val & d->sa_marks.in.mask)) {
			ldbg_spd(logger, indent, d_spd,
				 "skipped; marks.in %"PRIu32"/%#08"PRIx32" vs %"PRIu32"/%#08"PRIx32,
				 sa_marks->in.val, sa_marks->in.mask,
				 d->sa_marks.in.val, d->sa_marks.in.mask);
			continue;
		}
		if ((sa_marks->out.val & sa_marks->out.mask) != (d->sa_marks.out.val & d->sa_marks.out.mask)) {
			ldbg_spd(logger, indent, d_spd,
				 "skipped; marks.out %"PRIu32"/%#08"PRIx32" vs %"PRIu32"/%#08"PRIx32,
				 sa_marks->out.val, sa_marks->out.mask,
				 d->sa_marks.out.val, d->sa_marks.out.mask);
			continue;
		}

		/*
		 * Update head/hidden if it bests SPD
		 */
		if (owner == NULL) {
			ldbg_spd(logger, indent, d_spd, "policy saved; first match");
			owner = d_spd;
		} else if (owner->connection->child.routing < d->child.routing) {
			ldbg_spd(logger, indent, d_spd, "policy saved; better match");
			owner = d_spd;
		} else {
			ldbg_spd(logger, indent, d_spd, "skipped policy; not the best");
		}

	}

	return owner;
}

const struct spd_route *bare_cat_owner(const ip_selector *local,
				       const struct spd_route *spd,
				       struct logger *logger, where_t where)
{
	return raw_spd_owner(local, spd, RT_UNROUTED + 1,
			     logger, where, __func__, 0);
}

const struct spd_route *bare_spd_owner(const struct spd_route *spd,
				       struct logger *logger, where_t where)
{
	return raw_spd_owner(&spd->local->client, spd, RT_UNROUTED + 1,
			     logger, where, __func__, 0);
}

const struct spd_route *spd_owner(const struct spd_route *spd,
				  enum routing new_routing,
				  struct logger *logger, where_t where, unsigned indent)
{
	return raw_spd_owner(&spd->local->client, spd, new_routing,
			     logger, where, __func__, indent);
}

/*
 * Find who currently owns the route and kernel policy matching the
 * SPD.
 */

struct spd_owner {
	const struct spd_route *policy;
	const struct spd_route *route;
};

static const struct spd_owner null_spd_owner;

static struct spd_owner spd_conflict(const struct spd_route *c_spd,
				     unsigned indent)
{
	struct connection *c = c_spd->connection;

	struct logger *logger = c->logger;
	if (!oriented(c)) {
		llog(RC_LOG, logger,
		     "connection no longer oriented - system interface change?");
		return null_spd_owner;
	}

	selector_pair_buf spb;
	ldbg(logger, "%*slooking for SPD owners of %s",
	     indent, "",
	     str_selector_pair(&c_spd->local->client, &c_spd->remote->client, &spb));

	struct spd_owner owner = null_spd_owner;

	struct spd_route_filter srf = {
		.remote_client_range = &c_spd->remote->client,
		.where = HERE,
	};

	enum routing min_routing = RT_UNROUTED + 1;

	indent += 2;
	while (next_spd_route(NEW2OLD, &srf)) {
		struct spd_route *d_spd = srf.spd;
		struct connection *d = d_spd->connection;

		/*
		 * Pprune out anything that isn't conflicting
		 * according to selectors.
		 *
		 * Yes, conflicting is vague.  A good starting point
		 * is to look at what the kernel needs when it is
		 * deleting a policy.  For instance, the selectors
		 * matter, the rules (templ) do not,
		 */

		if (!oriented(d)) {
			/* can happen during shutdown */
			ldbg_spd(logger, indent, d_spd, "skipped; not oriented");
			continue;
		}

		if (c_spd == d_spd) {
			/* can only be owner; handled above */
			ldbg_spd(logger, indent, d_spd, "skipped; ignoring self");
			continue;
		}

		if (d->child.routing < min_routing) {
			ldbg_spd(logger, indent, d_spd, "skipped; %s is insufficient routing",
				 enum_name_short(&routing_names, min_routing));
			continue;
		}

		/* fast lookup did it's job! */
		PEXPECT(logger, selector_range_eq_selector_range(c_spd->remote->client,
								 d_spd->remote->client));
		if (!selector_eq_selector(c_spd->remote->client,
					  d_spd->remote->client)) {
			ldbg_spd(logger, indent, d_spd, "skipped; different remote selectors");
			continue;
		}

		/*
		 * Consider SPDs to be different when the either in or
		 * out marks differ (after masking).
		 */

		const struct sa_marks *sa_marks = &c->sa_marks;

		if ((c->sa_marks.in.val & c->sa_marks.in.mask) != (d->sa_marks.in.val & d->sa_marks.in.mask)) {
			ldbg_spd(logger, indent, d_spd,
				 "skipped; marks.in %"PRIu32"/%#08"PRIx32" vs %"PRIu32"/%#08"PRIx32,
				 sa_marks->in.val, sa_marks->in.mask,
				 d->sa_marks.in.val, d->sa_marks.in.mask);
			continue;
		}

		if ((c->sa_marks.out.val & c->sa_marks.out.mask) != (d->sa_marks.out.val & d->sa_marks.out.mask)) {
			ldbg_spd(logger, indent, d_spd,
				 "skipped; marks.out %"PRIu32"/%#08"PRIx32" vs %"PRIu32"/%#08"PRIx32,
				 sa_marks->out.val, sa_marks->out.mask,
				 d->sa_marks.out.val, d->sa_marks.out.mask);
			continue;
		}

		if (c->clonedfrom == d) {
			ldbg_spd(logger, indent, d_spd,
				 "skipped; is connection parent");
			continue;
		}

		/*
		 * Finally selector/route specific checks.
		 */

		if (!selector_eq_selector(c_spd->local->client, d_spd->local->client)) {
			ldbg_spd(logger, indent, d_spd, "policy skipped;  different local selectors");
		} else if (c->config->overlapip && d->config->overlapip) {
			ldbg_spd(logger, indent, d_spd, "policy skipped;  both ends have POLICY_OVERLAPIP");
		} else {
			/* winner? */
			if (owner.policy == NULL) {
				ldbg_spd(logger, indent, d_spd, "saved policy; first match");
				owner.policy = d_spd;
			} else if (owner.policy->connection->child.routing < d->child.routing) {
				ldbg_spd(logger, indent, d_spd, "saved policy; better match");
				owner.policy = d_spd;
			} else {
				ldbg_spd(logger, indent, d_spd, "skipped policy;  not the best");
			}
		}

		/*
		 * XXX: why?
		 *
		 * XXX: isn't host address comparison a routing and
		 * not SPD thing?  Ignoring a conflicting SPD because
		 * of the routing table seems wrong - the SPD still
		 * conflicts so only one is allowed.
		 */
		if (!address_eq_address(c->local->host.addr,
					d->local->host.addr)) {
			ldbg_spd(logger, indent, d_spd, "route skipped; different local address?!?");
		} else if (!routed(d)) {
			ldbg_spd(logger, indent, d_spd, "route skipped; not routed");
		} else {
			/* winner? */
			if (owner.route == NULL) {
				ldbg_spd(logger, indent, d_spd, "saved route; first route match");
				owner.route = d_spd;
			} else if (owner.route->connection->child.routing < d->child.routing) {
				ldbg_spd(logger, indent, d_spd, "saved route; better match");
				owner.route = d_spd;
			} else {
				ldbg_spd(logger, indent, d_spd, "skipped route;  not the best");
			}
		}
	}

	return owner;
}

const struct spd_route *route_owner(struct spd_route *spd)
{
	return spd_conflict(spd, 0).route;
}

/*
 * XXX: can this and/or route_owner() be merged?
 */

static bool get_connection_spd_conflict(struct spd_route *spd, struct logger *logger, unsigned indent)
{
	zero(&spd->wip);

	struct connection *c = spd->connection;

	if (c->config->sec_label.len > 0) {
		/* sec-labels ignore conflicts */
		return true;
	}

	/*
	 * Find how owns the installed SPD (kernel policy).
	 */

	struct spd_owner owner = spd_conflict(spd, indent);

	/*
	 * Double check that it really does own the SPD.
	 */
	if (owner.policy != NULL) {
		const struct connection *oc = owner.policy->connection;
		if (!kernel_policy_installed(oc)) {
			connection_buf ocb;
			llog_pexpect(logger, HERE,
				     "conflicting %s policy for "PRI_CONNECTION" is not installed",
				     enum_name_short(&routing_names, oc->child.routing),
				     pri_connection(oc, &ocb));
		}
	}

	/*
	 * If there's no SPD with a conflicting policy, perhaps
	 * there's a bare one.
	 *
	 * XXX: why not add this to the above hash table?
	 */

	struct bare_shunt **shunt = bare_shunt_ptr(&spd->local->client, &spd->remote->client, __func__);

	/*
	 * Report what was found.
	 */

	selector_pair_buf sb;
	ldbg(logger,
	     "%*s kernel: %s() %s; conflicting: policy=%s route=%s shunt=%s",
	     indent, "",
	     __func__, str_selector_pair(&spd->local->client, &spd->remote->client, &sb),
	     (owner.policy == NULL ? "<none>" : owner.policy->connection->name),
	     (owner.route == NULL ? "<none>" : owner.route->connection->name),
	     (shunt == NULL ? "<none>" : (*shunt)->why));

	if (owner.policy != NULL) {
		connection_buf cb;
		llog(RC_LOG_SERIOUS, logger,
		     "cannot install kernel policy -- it is in use for "PRI_CONNECTION,
		     pri_connection(owner.policy->connection, &cb));
		return false;
	}

	/*
	 * If there is already a ROUTE for peer's client subnet and it
	 * disagrees about interface or nexthop, we cannot steal it.
	 *
	 * XXX: should route_owner() have filtered out this already?
	 *
	 * Note: if this connection is already routed (perhaps for
	 * another state object), the route will agree.  This is as it
	 * should be -- it will arise during rekeying.
	 */
	struct connection *ro = (spd->wip.conflicting.route == NULL ? NULL :
				 spd->wip.conflicting.route->connection);
	if (ro != NULL && (ro->interface->ip_dev != c->interface->ip_dev ||
			   !address_eq_address(ro->local->host.nexthop, c->local->host.nexthop))) {
		/*
		 * Another connection is already using the eroute.
		 *
		 * TODO: XFRM supports this. For now, only allow this
		 * for OE.
		 */
		if (!is_opportunistic(c)) {
			connection_buf cib;
			llog(RC_LOG_SERIOUS, logger,
			     "cannot route -- route already in use for "PRI_CONNECTION"",
			     pri_connection(ro, &cib));
			return false;
		}

		connection_buf cib;
		llog(RC_LOG_SERIOUS, logger,
		     "cannot route -- route already in use for "PRI_CONNECTION" - but allowing anyway",
		     pri_connection(ro, &cib));
	}

	spd->wip.conflicting.shunt = shunt;
	spd->wip.conflicting.route = owner.route;
	return true;
}

static bool get_connection_spd_conflicts(struct connection *c, struct logger *logger)
{
	ldbg(logger, "checking %s for conflicts", c->name);
	FOR_EACH_ITEM(spd, &c->child.spds) {
		if (!get_connection_spd_conflict(spd, logger, 2)) {
			return false;
		}
	}
	return true;
}

static void revert_kernel_policy(struct spd_route *spd,
				 struct state *st/*could be NULL*/,
				 struct logger *logger)
{
	struct connection *c = spd->connection;
	PEXPECT(logger, st == NULL || st->st_connection == c);
	PEXPECT(logger, (logger == c->logger ||
			 logger == st->st_logger));

	/*
	 * Kill the firewall if just installed.
	 */
	if (spd->wip.installed.up) {
		PEXPECT(logger, st != NULL);
		ldbg(logger, "kernel: %s() reverting the firewall", __func__);
		if (!do_updown(UPDOWN_DOWN, c, spd, st, logger)) {
			dbg("kernel: down command returned an error");
		}
		spd->wip.installed.up = false;
	}

	/*
	 * Now unwind the policy.
	 *
	 * Of course, if things failed before the policy was
	 * installed, there's nothing to do.
	 */

	if (!spd->wip.installed.kernel_policy) {
		ldbg(logger, "kernel: %s() no kernel policy to revert", __func__);
		return;
	}

	/*
	 * If there was no bare shunt, just delete everything.
	 *
	 * XXX: is this overkill?  For instance, when an instance
	 * IPsec fails should things go back to the prospective
	 * template?
	 */

	if (spd->wip.conflicting.shunt == NULL) {
		ldbg(logger, "kernel: %s() no previous kernel policy or shunt: delete whatever we installed",
		     __func__);
		delete_spd_kernel_policy(spd, DIRECTION_OUTBOUND,
					 EXPECT_KERNEL_POLICY_OK,
					 c->logger, HERE,
					 "deleting failed policy");
		delete_spd_kernel_policy(spd, DIRECTION_INBOUND,
					 EXPECT_KERNEL_POLICY_OK,
					 c->logger, HERE,
					 "deleting failed policy");
		return;
	}

	/* only one - shunt set when no policy */
	PASSERT(logger, spd->wip.conflicting.shunt != NULL);

	/*
	 * If there's a bare shunt, restore it.
	 *
	 * I don't think that this case is very likely.  Normally a
	 * bare shunt would have been assigned to a connection before
	 * we've gotten this far.
	 */

	ldbg(logger, "kernel: %s() restoring bare shunt", __func__);
	struct bare_shunt *bs = *spd->wip.conflicting.shunt;
	struct nic_offload nic_offload = {};
	setup_esp_nic_offload(&nic_offload, c, NULL);
	if (!install_bare_kernel_policy(bs->our_client, bs->peer_client,
					bs->shunt_kind, bs->shunt_policy,
					&nic_offload, logger, HERE)) {
		llog(RC_LOG, st->st_logger,
		     "%s() failed to restore/replace SA",
		     __func__);
	}
}

bool unrouted_to_routed(struct connection *c, enum shunt_kind shunt_kind, where_t where)
{
	/*
	 * If this is a transport SA, and overlapping SAs are
	 * supported, then this route is not necessary at all.
	 */
	PEXPECT(c->logger, !kernel_ops->overlap_supported); /* still WIP */
	if (kernel_ops->overlap_supported && c->config->child_sa.encap_mode == ENCAP_MODE_TRANSPORT) {
		ldbg(c->logger, "route-unnecessary: overlap and transport");
		return true;
	}

	/*
	 * Pass +0: Lookup the status of each SPD.
	 *
	 * Still call find_spd_conflicts() when a sec_label so that
	 * the structure is zeroed (sec_labels ignore conflicts).
	 */

	if (!get_connection_spd_conflicts(c, c->logger)) {
		return false;
	}

	/*
	 * Pass +1: install / replace kernel policy where needed.
	 */

	bool ok = true;
	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (!ok) {
			break;
		}

		/*
		 * When overlap isn't supported, the old clashing bare
		 * shunt needs to be deleted before the new one can be
		 * installed.  Else it can be deleted after.
		 *
		 * For linux this also requires SA_MARKS to be set
		 * uniquely.
		 */

		if (spd->wip.conflicting.shunt != NULL &&
		    PEXPECT(c->logger, !kernel_ops->overlap_supported)) {
			delete_bare_shunt_kernel_policy(*spd->wip.conflicting.shunt,
							EXPECT_KERNEL_POLICY_OK,
							c->logger, where);
			/* if everything succeeds, delete below */
		}

		ok &= spd->wip.installed.kernel_policy =
			install_prospective_kernel_policies(spd, shunt_kind,
							    c->logger, where);

		if (spd->wip.conflicting.shunt != NULL &&
		    PBAD(c->logger, kernel_ops->overlap_supported)) {
			delete_bare_shunt_kernel_policy(*spd->wip.conflicting.shunt,
							EXPECT_KERNEL_POLICY_OK,
							c->logger, where);
			/* if everything succeeds, delete below */
		}
	}

	/*
	 * Pass +2: add the route.
	 */

	ldbg(c->logger, "kernel: %s() running updown-prepare when needed", __func__);
	FOR_EACH_ITEM(spd, &c->child.spds) {
		if (!ok) {
			break;
		}
		if (spd->wip.conflicting.route == NULL) {
			/* a new route: no deletion required, but preparation is */
			if (!do_updown(UPDOWN_PREPARE, c, spd, NULL/*state*/, c->logger))
				ldbg(c->logger, "kernel: prepare command returned an error");
		}
	}

	ldbg(c->logger, "kernel: %s() running updown-route when needed", __func__);
	FOR_EACH_ITEM(spd, &c->child.spds) {
		if (!ok) {
			break;
		}
		if (spd->wip.conflicting.route == NULL) {
			ok &= spd->wip.installed.route =
				do_updown(UPDOWN_ROUTE, c, spd, NULL/*state*/, c->logger);
		}
	}

	/*
	 * If things failed bail.
	 */

	if (!ok) {
		FOR_EACH_ITEM(spd, &c->child.spds) {
			revert_kernel_policy(spd, NULL/*st*/, c->logger);
		}
		return false;
	}

	/*
	 * Now clean up any shunts that were replaced.
	 */

	FOR_EACH_ITEM(spd, &c->child.spds) {
		struct bare_shunt **bspp = spd->wip.conflicting.shunt;
		if (bspp != NULL) {
			free_bare_shunt(bspp);
		}
	}

	return true;
}

/*
 * Find a bare shunt that encompasses the selector pair.
 *
 * Since bare shunt kernel policies have the heighest priority (0) use
 * selector_in_selector for the match.  For instance a bare shunt
 * 1.2.3.4/32/tcp encompass the address 1.2.3.4/32/tcp/22.
 *
 * Trick: return a pointer to the pointer to the entry; this allows
 * the entry to be deleted.
 */
struct bare_shunt **bare_shunt_ptr(const ip_selector *our_client,
				   const ip_selector *peer_client,
				   const char *why)

{
	selector_pair_buf sb;
	dbg("kernel: %s looking for %s",
	    why, str_selector_pair(our_client, peer_client, &sb));
	for (struct bare_shunt **pp = &bare_shunts; *pp != NULL; pp = &(*pp)->next) {
		struct bare_shunt *p = *pp;
		ldbg_bare_shunt(&global_logger, "comparing", p);
		if (selector_in_selector(*our_client, p->our_client) &&
		    selector_in_selector(*peer_client, p->peer_client)) {
			return pp;
		}
	}
	return NULL;
}

/*
 * Free a bare_shunt entry, given a pointer to the pointer.
 */
void free_bare_shunt(struct bare_shunt **pp)
{
	struct bare_shunt *p;

	passert(pp != NULL);

	p = *pp;

	*pp = p->next;
	ldbg_bare_shunt(&global_logger, "delete", p);
	pfree(p);
}

unsigned shunt_count(void)
{
	unsigned i = 0;

	for (const struct bare_shunt *bs = bare_shunts; bs != NULL; bs = bs->next) {
		i++;
	}

	return i;
}

void show_shunt_status(struct show *s)
{
	show_separator(s);
	show_comment(s, "Bare Shunt list:");
	show_separator(s);

	for (const struct bare_shunt *bs = bare_shunts; bs != NULL; bs = bs->next) {
		/* Print interesting fields.  Ignore count and last_active. */
		SHOW_JAMBUF(RC_COMMENT, s, buf) {
			jam_selector_subnet_port(buf, &(bs)->our_client);
			jam(buf, " -%d-> ", bs->transport_proto->ipproto);
			jam_selector_subnet_port(buf, &(bs)->peer_client);
			jam_string(buf, " => ");
			jam_enum(buf, &shunt_policy_percent_names, bs->shunt_policy);
			jam_string(buf, "    ");
			jam_string(buf, bs->why);
			if (bs->restore_serialno != UNSET_CO_SERIAL) {
				jam_string(buf, " ");
				jam_co(buf, bs->restore_serialno);
			}
		}
	}
}

static void delete_bare_shunt_kernel_policy(const struct bare_shunt *bsp,
					    enum expect_kernel_policy expect_kernel_policy,
					    struct logger *logger, where_t where)
{
	/*
	 * XXX: bare_kernel_policy() does not strip the port but this
	 * code does.
	 *
	 * Presumably it is because bare shunts is widened to include
	 * all protocols / ports.  But if that were true the selectors
	 * would have already excluded the port.
	 *
	 * XXX: this is probably a bug.  Any widening should happen
	 * before the bare shunt is added.
	 */
#if 0
	pexpect(bsp->our_client.hport == 0);
	pexpect(bsp->peer_client.hport == 0);
#endif
	const struct ip_protocol *transport_proto = bsp->transport_proto;
	ip_address src_address = selector_prefix(bsp->our_client);
	ip_address dst_address = selector_prefix(bsp->peer_client);
	ip_selector src = selector_from_address_protocol(src_address, transport_proto);
	ip_selector dst = selector_from_address_protocol(dst_address, transport_proto);
	/* assume low code logged action */
	if (!delete_kernel_policy(DIRECTION_OUTBOUND,
				  expect_kernel_policy,
				  &src, &dst,
				  /*sa_marks*/NULL, /*xfrmi*/NULL, /*bare-shunt*/
				  DEFAULT_KERNEL_POLICY_ID,
				  /* bare-shunt: no sec_label XXX: ?!? */
				  null_shunk,
				  logger, where, "bare shunt")) {
		/* ??? we could not delete a bare shunt */
		llog_bare_shunt(RC_LOG, logger, bsp, "failed to delete kernel policy");
	}
}

/*
 * Clear any bare shunt holds that overlap with the network we have
 * just routed.  We only consider "narrow" holds: ones for a single
 * address to single address.
 */

static void clear_narrow_holds(const ip_selector *src_client,
			       const ip_selector *dst_client,
			       struct logger *logger)
{
	const struct ip_protocol *transport_proto = protocol_from_ipproto(src_client->ipproto);
	struct bare_shunt **bspp = &bare_shunts;
	while (*bspp != NULL) {
		/*
		 * is bsp->{local,remote} within {local,remote}.
		 */
		struct bare_shunt *bsp = *bspp;
		if (bsp->shunt_policy == SHUNT_HOLD &&
		    transport_proto == bsp->transport_proto &&
		    selector_in_selector(bsp->our_client, *src_client) &&
		    selector_in_selector(bsp->peer_client, *dst_client)) {
			delete_bare_shunt_kernel_policy(bsp,
							EXPECT_KERNEL_POLICY_OK,
							logger, HERE);
			free_bare_shunt(bspp);
		} else {
			bspp = &(*bspp)->next;
		}
	}
}

/*
 * Install inbound and outbound traps for the first SPD with sec_label
 * attached.
 *
 * unrouted_to_routed_ondemand() installs multiple SPDs and only
 * outbound.
 */
bool unrouted_to_routed_ondemand_sec_label(struct connection *c, struct logger *logger, where_t where)
{
	connection_buf cb;
	ldbg(logger,
	     "kernel: %s() "PRI_CO" "PRI_CO" "PRI_CONNECTION" routed %s sec_label="PRI_SHUNK,
	     __func__,
	     pri_connection_co(c),
	     pri_connection_co(c->clonedfrom),
	     pri_connection(c, &cb),
	     enum_name(&routing_names, c->child.routing),
	     pri_shunk(c->config->sec_label));

	if (!PEXPECT(logger, is_labeled_template(c) || is_labeled_parent(c))) {
		return false;
	}

	if (PBAD(logger, kernel_policy_installed(c))) {
		dbg("kernel: %s() connection already routed", __func__);
		return true;
	}

	/*
	 * SE installs both an outgoing and incoming policy.  Normal
	 * connections do not.
	 */
	FOR_EACH_THING(direction, DIRECTION_OUTBOUND, DIRECTION_INBOUND) {
		if (!add_sec_label_kernel_policy(c->spd, direction,
						 /*logger*/logger, where,
						 "ondemand security label")) {
			if (direction == DIRECTION_INBOUND) {
				/*
				 * Need to pull the just installed
				 * outbound policy.
				 */
				ldbg(logger, "pulling previously installed outbound policy");
				pexpect(direction == DIRECTION_INBOUND);
				delete_spd_kernel_policy(c->spd, DIRECTION_OUTBOUND,
							 EXPECT_KERNEL_POLICY_OK,
							 /*logger*/logger,
							 where, "security label policy");
			}
			return false;
		}
	}

	/* a new route: no deletion required, but preparation is */
	if (!do_updown(UPDOWN_PREPARE, c, c->spd, NULL/*ST*/, logger)) {
		ldbg(logger, "kernel: %s() prepare command returned an error", __func__);
	}

	if (!do_updown(UPDOWN_ROUTE, c, c->spd, NULL/*ST*/, logger)) {
		/* Failure!  Unwind our work. */
		ldbg(logger, "kernel: %s() route command returned an error", __func__);
		if (!do_updown(UPDOWN_DOWN, c, c->spd, NULL/*st*/, logger)) {
			ldbg(logger, "kernel: down command returned an error");
		}
		delete_spd_kernel_policy(c->spd, DIRECTION_OUTBOUND,
					 EXPECT_KERNEL_POLICY_OK,
					 logger, where, "failed security label");
		delete_spd_kernel_policy(c->spd, DIRECTION_INBOUND,
					 EXPECT_KERNEL_POLICY_OK,
					 logger, where, "failed security label");
		return false;
	}

	return true;
}

void setup_esp_nic_offload(struct nic_offload *nic_offload, const struct connection *c,
		bool *nic_offload_fallback)
{
	if (c->config->nic_offload == offload_no ||
	    c->interface == NULL || c->interface->ip_dev == NULL ||
	    c->interface->ip_dev->id_rname == NULL) {
		dbg("kernel: NIC esp-hw-offload disabled for connection '%s'", c->name);
		return;
	}

	if (c->config->nic_offload == offload_auto) {
		if (!c->interface->ip_dev->id_nic_offload) {
			dbg("kernel: NIC esp-hw-offload not for connection '%s' not available on interface %s",
				c->name, c->interface->ip_dev->id_rname);
			return;
		}
		if (nic_offload_fallback)
			*nic_offload_fallback = true;
		dbg("kernel: NIC esp-hw-offload offload for connection '%s' enabled on interface %s",
		    c->name, c->interface->ip_dev->id_rname);
	}
	nic_offload->dev = c->interface->ip_dev->id_rname;
	nic_offload->type = (c->config->nic_offload == offload_packet) ?
				OFFLOAD_PACKET : OFFLOAD_CRYPTO;
}

/*
 * Set up one direction of the SA bundle
 */

static bool setup_half_kernel_state(struct state *st, enum direction direction)
{
	/* Build an inbound or outbound SA */

	struct connection *c = st->st_connection;
	bool replace = (direction == DIRECTION_INBOUND && (kernel_ops->get_ipsec_spi != NULL));
	bool nic_offload_fallback = false;

	/* SPIs, saved for spigrouping or undoing, if necessary */
	struct kernel_state said[EM_MAXRELSPIS];
	struct kernel_state *said_next = said;

	/* same scope as said[] */
	said_buf text_ipcomp;
	said_buf text_esp;
	said_buf text_ah;

	uint64_t sa_ipsec_soft_bytes =  c->config->sa_ipsec_max_bytes;
	uint64_t sa_ipsec_soft_packets = c->config->sa_ipsec_max_packets;

	if (c->config->rekey) {
		sa_ipsec_soft_bytes = fuzz_soft_limit("ipsec-max-bytes",st->st_sa_role,
						      c->config->sa_ipsec_max_bytes,
						      IPSEC_SA_MAX_SOFT_LIMIT_PERCENTAGE,
						      st->st_logger);
		sa_ipsec_soft_packets = fuzz_soft_limit("ipsec-max-packets", st->st_sa_role,
							c->config->sa_ipsec_max_packets,
							IPSEC_SA_MAX_SOFT_LIMIT_PERCENTAGE,
							st->st_logger);
	}


	struct kernel_route route = kernel_route_from_state(st, direction);

	const struct kernel_state said_boilerplate = {
		.src.address = route.src.address,
		.dst.address = route.dst.address,
		.src.route = route.src.route,
		.dst.route = route.dst.route,
		.direction = direction,
		.tunnel = (route.mode == ENCAP_MODE_TUNNEL),
		.sa_lifetime = c->config->sa_ipsec_max_lifetime,
		.sa_max_soft_bytes = sa_ipsec_soft_bytes,
		.sa_max_soft_packets = sa_ipsec_soft_packets,
		.sa_ipsec_max_bytes = c->config->sa_ipsec_max_bytes,
		.sa_ipsec_max_packets = c->config->sa_ipsec_max_packets,
		.sec_label = (st->st_v1_seen_sec_label.len > 0 ? st->st_v1_seen_sec_label :
			      st->st_v1_acquired_sec_label.len > 0 ? st->st_v1_acquired_sec_label :
			      c->child.sec_label /* assume connection outlive their kernel_sa's */),
	};

	address_buf sab, dab;
	selector_buf scb, dcb;
	dbg("kernel: %s() %s %s->[%s=%s=>%s]->%s sec_label="PRI_SHUNK"%s",
	    __func__,
	    enum_name_short(&direction_names, said_boilerplate.direction),
	    str_selector(&said_boilerplate.src.route, &scb),
	    str_address(&said_boilerplate.src.address, &sab),
	    enum_name_short(&encap_mode_names, route.mode),
	    str_address(&said_boilerplate.dst.address, &dab),
	    str_selector(&said_boilerplate.dst.route, &dcb),
	    /* see above */
	    pri_shunk(said_boilerplate.sec_label),
	    (st->st_v1_seen_sec_label.len > 0 ? " (IKEv1 seen)" :
	     st->st_v1_acquired_sec_label.len > 0 ? " (IKEv1 acquired)" :
	     c->child.sec_label.len > 0 ? " (IKEv2 this)" :
	     ""))

	/* set up IPCOMP SA, if any */

	if (st->st_ipcomp.present) {
		ipsec_spi_t ipcomp_spi = (direction == DIRECTION_INBOUND ? st->st_ipcomp.inbound.spi :
					  st->st_ipcomp.outbound.spi);
		*said_next = said_boilerplate;
		said_next->spi = ipcomp_spi;
		said_next->proto = &ip_protocol_ipcomp;

		said_next->ipcomp = st->st_ipcomp.attrs.transattrs.ta_ipcomp;
		said_next->level = said_next - said;
		said_next->reqid = reqid_ipcomp(c->child.reqid);
		said_next->story = said_str(route.dst.address,
					    &ip_protocol_ipcomp,
					    ipcomp_spi, &text_ipcomp);

		if (c->xfrmi != NULL) {
			said_next->xfrm_if_id = c->xfrmi->if_id;
			said_next->mark_set = c->sa_marks.out;
		}

		if (!kernel_ops_add_sa(said_next, replace, st->st_logger)) {
			log_state(RC_LOG, st, "add_sa ipcomp failed");
			goto fail;
		}
		said_next++;
	}

	/* set up ESP SA, if any */

	if (st->st_esp.present) {
		ipsec_spi_t esp_spi = (direction == DIRECTION_INBOUND ? st->st_esp.inbound.spi :
				       st->st_esp.outbound.spi);
		chunk_t esp_keymat = (direction == DIRECTION_INBOUND ? st->st_esp.inbound.keymat :
				      st->st_esp.outbound.keymat);
		const struct trans_attrs *ta = &st->st_esp.attrs.transattrs;

		const struct ip_encap *encap_type = NULL;
		uint16_t encap_sport = 0, encap_dport = 0;
		ip_address natt_oa;

		if (st->hidden_variables.st_nat_traversal & NAT_T_DETECTED ||
		    st->st_interface->io->protocol == &ip_protocol_tcp) {
			encap_type = st->st_interface->io->protocol->encap_esp;
			switch (direction) {
			case DIRECTION_INBOUND:
				encap_sport = endpoint_hport(st->st_remote_endpoint);
				encap_dport = endpoint_hport(st->st_interface->local_endpoint);
				break;
			case DIRECTION_OUTBOUND:
				encap_sport = endpoint_hport(st->st_interface->local_endpoint);
				encap_dport = endpoint_hport(st->st_remote_endpoint);
				break;
			default:
				bad_case(direction);
			}
			natt_oa = st->hidden_variables.st_nat_oa;
			dbg("kernel: natt/tcp sa encap_type="PRI_IP_ENCAP" sport=%d dport=%d",
			    pri_ip_encap(encap_type), encap_sport, encap_dport);
		}

		dbg("kernel: looking for alg with encrypt: %s keylen: %d integ: %s",
		    ta->ta_encrypt->common.fqn, ta->enckeylen, ta->ta_integ->common.fqn);

		/*
		 * Check that both integrity and encryption are
		 * supported by the kernel.
		 *
		 * Since the parser uses these exact same checks when
		 * loading the connection, they should never fail (if
		 * they do then strange things have been going on
		 * since the connection was loaded).
		 */
		if (!kernel_alg_integ_ok(ta->ta_integ)) {
			log_state(RC_LOG_SERIOUS, st,
				  "ESP integrity algorithm %s is not implemented or allowed",
				  ta->ta_integ->common.fqn);
			goto fail;
		}
		if (!kernel_alg_encrypt_ok(ta->ta_encrypt)) {
			log_state(RC_LOG_SERIOUS, st,
				  "ESP encryption algorithm %s is not implemented or allowed",
				  ta->ta_encrypt->common.fqn);
			goto fail;
		}

		/*
		 * Validate the encryption key size.
		 */
		size_t encrypt_keymat_size;
		if (!kernel_alg_encrypt_key_size(ta->ta_encrypt, ta->enckeylen,
						 &encrypt_keymat_size)) {
			log_state(RC_LOG_SERIOUS, st,
				  "ESP encryption algorithm %s with key length %d not implemented or allowed",
				  ta->ta_encrypt->common.fqn, ta->enckeylen);
			goto fail;
		}

		/* Fixup key lengths for special cases */
#ifdef USE_3DES
		if (ta->ta_encrypt == &ike_alg_encrypt_3des_cbc) {
			/* Grrrrr.... f*cking 7 bits jurassic algos */
			/* 168 bits in kernel, need 192 bits for keymat_len */
			if (encrypt_keymat_size == 21) {
				dbg("kernel: %s requires a 7-bit jurassic adjust",
				    ta->ta_encrypt->common.fqn);
				encrypt_keymat_size = 24;
			}
		}
#endif

		if (ta->ta_encrypt->salt_size > 0) {
			dbg("kernel: %s requires %zu salt bytes",
			    ta->ta_encrypt->common.fqn, ta->ta_encrypt->salt_size);
			encrypt_keymat_size += ta->ta_encrypt->salt_size;
		}

		size_t integ_keymat_size = ta->ta_integ->integ_keymat_size; /* BYTES */

		dbg("kernel: st->st_esp.keymat_len=%zu is encrypt_keymat_size=%zu + integ_keymat_size=%zu",
		    esp_keymat.len, encrypt_keymat_size, integ_keymat_size);

		PASSERT(st->st_logger, esp_keymat.len == encrypt_keymat_size + integ_keymat_size);

		*said_next = said_boilerplate;
		said_next->spi = esp_spi;
		said_next->proto = &ip_protocol_esp;
		said_next->replay_window = c->config->child_sa.replay_window;
		ldbg(st->st_logger, "kernel: setting IPsec SA replay-window to %ju",
		     c->config->child_sa.replay_window);

		if (c->xfrmi != NULL) {
			said_next->xfrm_if_id = c->xfrmi->if_id;
			said_next->mark_set = c->sa_marks.out;
		}

		if (direction == DIRECTION_OUTBOUND &&
		    c->config->child_sa.tfcpad != 0 &&
		    !st->st_seen_no_tfc) {
			ldbg(st->st_logger, "kernel: Enabling TFC at %ju bytes (up to PMTU)",
			     c->config->child_sa.tfcpad);
			said_next->tfcpad = c->config->child_sa.tfcpad;
		}

		if (c->config->decap_dscp) {
			ldbg(st->st_logger, "kernel: Enabling Decap ToS/DSCP bits");
			said_next->decap_dscp = true;
		}
		if (!c->config->encap_dscp) {
			ldbg(st->st_logger, "kernel: Disabling Encap ToS/DSCP bits");
			said_next->encap_dscp = false;
		}
		if (c->config->nopmtudisc) {
			ldbg(st->st_logger, "kernel: Disabling Path MTU Discovery");
			said_next->nopmtudisc = true;
		}

		said_next->integ = ta->ta_integ;
#ifdef USE_SHA2
		if (said_next->integ == &ike_alg_integ_sha2_256 &&
		    c->config->sha2_truncbug) {
			if (kernel_ops->sha2_truncbug_support) {
				if (libreswan_fipsmode() == 1) {
					log_state(RC_LOG_SERIOUS, st,
						  "Error: sha2-truncbug=yes is not allowed in FIPS mode");
					goto fail;
				}
				dbg("kernel:  authalg converted for sha2 truncation at 96bits instead of IETF's mandated 128bits");
				/*
				 * We need to tell the kernel to mangle
				 * the sha2_256, as instructed by the user
				 */
				said_next->integ = &ike_alg_integ_hmac_sha2_256_truncbug;
			} else {
				log_state(RC_LOG_SERIOUS, st,
					  "Error: %s stack does not support sha2_truncbug=yes",
					  kernel_ops->interface_name);
				goto fail;
			}
		}
#endif
		if (st->st_esp.attrs.transattrs.esn_enabled) {
			dbg("kernel: Enabling ESN");
			said_next->esn = true;
		}

		/*
		 * XXX: Assume SADB_ and ESP_ numbers match!  Clearly
		 * setting .compalg is wrong, don't yet trust
		 * lower-level code to be right.
		 */
		said_next->encrypt = ta->ta_encrypt;

		/* divide up keying material */
		said_next->encrypt_key = shunk2(esp_keymat.ptr, encrypt_keymat_size); /*BYTES*/
		said_next->integ_key = shunk2(esp_keymat.ptr + encrypt_keymat_size, integ_keymat_size); /*BYTES*/

		said_next->level = said_next - said;
		said_next->reqid = reqid_esp(c->child.reqid);

		said_next->src.encap_port = encap_sport;
		said_next->dst.encap_port = encap_dport;
		said_next->encap_type = encap_type;
		said_next->natt_oa = &natt_oa;
		said_next->story = said_str(route.dst.address,
					    &ip_protocol_esp,
					    esp_spi, &text_esp);

		if (DBGP(DBG_PRIVATE) || DBGP(DBG_CRYPT)) {
			DBG_dump_hunk("ESP encrypt key:",  said_next->encrypt_key);
			DBG_dump_hunk("ESP integrity key:", said_next->integ_key);
		}

		setup_esp_nic_offload(&said_next->nic_offload, c, &nic_offload_fallback);

		bool ret = kernel_ops_add_sa(said_next, replace, st->st_logger);

		if (!ret && nic_offload_fallback &&
			said_next->nic_offload.dev != NULL) {
			/* Fallback to crypto offload from packet offload */
			if (said_next->nic_offload.type == OFFLOAD_PACKET) {
				said_next->nic_offload.type = OFFLOAD_CRYPTO;
				ret = kernel_ops_add_sa(said_next, replace, st->st_logger);
			}

			if (!ret) {
				/* Fallback to non-nic-offload crypto */
				said_next->nic_offload.dev = NULL;
				ret = kernel_ops_add_sa(said_next, replace, st->st_logger);
			}
		}

		/* scrub keys from memory */
		memset(esp_keymat.ptr, 0, esp_keymat.len);

		if (!ret)
			goto fail;

		said_next++;
	}

	/* set up AH SA, if any */

	if (st->st_ah.present) {
		ipsec_spi_t ah_spi = (direction == DIRECTION_INBOUND ? st->st_ah.inbound.spi :
				      st->st_ah.outbound.spi);
		chunk_t ah_keymat = (direction == DIRECTION_INBOUND ? st->st_ah.inbound.keymat :
				     st->st_ah.outbound.keymat);

		const struct integ_desc *integ = st->st_ah.attrs.transattrs.ta_integ;
		if (integ->integ_ikev1_ah_transform <= 0) {
			log_state(RC_LOG_SERIOUS, st,
				  "%s not implemented",
				  integ->common.fqn);
			goto fail;
		}

		PASSERT(st->st_logger, ah_keymat.len == integ->integ_keymat_size);

		*said_next = said_boilerplate;
		said_next->spi = ah_spi;
		said_next->proto = &ip_protocol_ah;
		said_next->integ = integ;
		said_next->integ_key = shunk2(ah_keymat.ptr, ah_keymat.len);
		said_next->level = said_next - said;
		said_next->reqid = reqid_ah(c->child.reqid);
		said_next->story = said_str(route.dst.address,
					    &ip_protocol_ah,
					    ah_spi, &text_ah);

		said_next->replay_window = c->config->child_sa.replay_window;
		ldbg(st->st_logger, "kernel: setting IPsec SA replay-window to %ju",
		     c->config->child_sa.replay_window);

		if (st->st_ah.attrs.transattrs.esn_enabled) {
			dbg("kernel: Enabling ESN");
			said_next->esn = true;
		}

		if (DBGP(DBG_PRIVATE) || DBGP(DBG_CRYPT)) {
			DBG_dump_hunk("AH authkey:", said_next->integ_key);
		}

		bool ret = kernel_ops_add_sa(said_next, replace, st->st_logger);
		/* scrub key from memory */
		memset(ah_keymat.ptr, 0, ah_keymat.len);

		if (!ret) {
			goto fail;
		}

		said_next++;
	}

	/* If there are multiple SPIs, group them. */

	if (kernel_ops->grp_sa != NULL && said_next > &said[1]) {
		/*
		 * group SAs, two at a time, inner to outer (backwards in
		 * said[])
		 *
		 * The grouping is by pairs.  So if said[] contains
		 * ah esp ipip,
		 *
		 * the grouping would be ipip:esp, esp:ah.
		 */
		for (struct kernel_state *s = said; s < said_next - 1; s++) {
			dbg("kernel: grouping %s and %s",
			    s[0].story, s[1].story);
			if (!kernel_ops->grp_sa(s + 1, s)) {
				log_state(RC_LOG, st, "grp_sa failed");
				goto fail;
			}
		}
		/* could update said, but it will not be used */
	}
	/* if the impaired is set, pretend this fails */
	if (impair.sa_creation) {
		DBG_log("Impair SA creation is set, pretending to fail");
		goto fail;
	}
	return true;

fail:
	log_state(RC_LOG, st, "setup_half_ipsec_sa() hit fail:");
	/*
	 * Undo the done SPIs.
	 *
	 * Deleting the SPI also deletes any SAs attached to them.
	 */
	while (said_next-- != said) {
		if (said_next->proto != NULL) {
			kernel_ops_del_ipsec_spi(said_next->spi,
						 said_next->proto,
						 &said_next->src.address,
						 &said_next->dst.address,
						 st->st_logger);
		}
	}
	return false;
}

static bool install_inbound_ipsec_kernel_policies(struct child_sa *child)
{
	const struct connection *c = child->sa.st_connection;
	struct logger *logger = child->sa.st_logger;
	/*
	 * Add an inbound eroute to enforce an arrival check.
	 *
	 * If inbound,
	 * ??? and some more mysterious conditions,
	 * Note reversed ends.
	 * Not much to be done on failure.
	 */
	ldbg(logger, "kernel: %s() owner="PRI_SO,
	     __func__, pri_so(c->newest_routing_sa));

	if (is_labeled_child(c)) {
		ldbg(logger, "kernel: %s() skipping as IKEv2 config.sec_label="PRI_SHUNK,
		     __func__, pri_shunk(c->config->sec_label));
		return true;
	}

	if (c->newest_routing_sa != SOS_NOBODY &&
	    c->newest_routing_sa != child->sa.st_serialno) {
		ldbg(logger, "kernel: %s() skipping as already has owner "PRI_SO,
		     __func__, pri_so(c->newest_routing_sa));
		return true;
	}

	FOR_EACH_ITEM(spd, &c->child.spds) {
		selector_pair_buf spb;
		ldbg(logger, "kernel: %s() is installing SPD for %s",
		     __func__, str_selector_pair(&spd->remote->client, &spd->local->client, &spb));
		install_inbound_ipsec_kernel_policy(child, spd, HERE);
	}

	return true;
}

/*
 * XXX: Two cases:
 *
 * - the protocol was negotiated (and presumably installed)
 *   (.present)
 *
 * - the protocol was proposed but never finished (.out_spi
 *   inbound)
 */

struct dead_sa {	/* XXX: this is ip_said+src */
	const struct ip_protocol *protocol;
	ipsec_spi_t spi;
	ip_address src;
	ip_address dst;
};

static unsigned append_teardown(struct dead_sa *dead, enum direction direction,
				const struct ipsec_proto_info *proto,
				ip_address host_addr, ip_address effective_remote_address)
{
	bool present = proto->present;
	if (!present &&
	    direction == DIRECTION_INBOUND &&
	    proto->inbound.spi != 0 &&
	    proto->outbound.spi == 0) {
		dbg("kernel: forcing inbound delete of %s as .inbound.spi: "PRI_IPSEC_SPI"; attrs.spi: "PRI_IPSEC_SPI,
		    proto->protocol->name,
		    pri_ipsec_spi(proto->inbound.spi),
		    pri_ipsec_spi(proto->outbound.spi));
		present = true;
	}
	if (present) {
		dead->protocol = proto->protocol;
		switch (direction) {
		case DIRECTION_INBOUND:
			if (proto->inbound.kernel_sa_expired & SA_HARD_EXPIRED) {
				dbg("kernel expired SPI 0x%x skip deleting",
				    ntohl(proto->inbound.spi));
				return 0;
			}
			dead->spi = proto->inbound.spi; /* incoming */
			dead->src = effective_remote_address;
			dead->dst = host_addr;
			break;
		case DIRECTION_OUTBOUND:
			if (proto->outbound.kernel_sa_expired & SA_HARD_EXPIRED) {
				dbg("kernel hard expired SPI 0x%x skip deleting",
				    ntohl(proto->outbound.spi));
				return 0;
			}
			dead->spi = proto->outbound.spi; /* outgoing */
			dead->src = host_addr;
			dead->dst = effective_remote_address;
			break;
		default:
			bad_case(direction);
		}
		return 1;
	}
	return 0;
}

/*
 * Delete any AH, ESP, and IPCOMP kernel states.
 *
 * Deleting only requires the addresses, protocol, and IPsec SPIs.
 */

static bool uninstall_kernel_state(struct child_sa *child, enum direction direction)
{
	struct connection *const c = child->sa.st_connection;
	ldbg_sa(child, "kernel: %s() deleting %s",
		__func__, enum_name_short(&direction_names, direction));

	/*
	 * If we have a new address in c->remote->host.addr,
	 * we are the initiator, have been redirected,
	 * and yet this routine must use the old address.
	 *
	 * We point effective_remote_host_address to the appropriate
	 * address.
	 */

	ip_address effective_remote_address = c->remote->host.addr;
	if (!endpoint_address_eq_address(child->sa.st_remote_endpoint, effective_remote_address) &&
	    address_is_specified(c->redirect.ip)) {
		effective_remote_address = endpoint_address(child->sa.st_remote_endpoint);
	}

	/* collect each proto SA that needs deleting */

	struct dead_sa dead[3];	/* at most 3 entries */
	unsigned nr = 0;
	nr += append_teardown(dead + nr, direction, &child->sa.st_ah,
			      c->local->host.addr, effective_remote_address);
	nr += append_teardown(dead + nr, direction, &child->sa.st_esp,
			      c->local->host.addr, effective_remote_address);
	nr += append_teardown(dead + nr, direction, &child->sa.st_ipcomp,
			      c->local->host.addr, effective_remote_address);
	passert(nr < elemsof(dead));

	/*
	 * If the SAs have been grouped, deleting any one will do: we
	 * just delete the first one found.
	 */
	if (kernel_ops->grp_sa != NULL && nr > 1) {
		nr = 1;
	}

	/*
	 * Delete each proto that needs deleting.
	 *
	 * Deleting the SPI also deletes any corresponding SA.
	 */
	bool result = true;
	for (unsigned i = 0; i < nr; i++) {
		const struct dead_sa *tbd = &dead[i];
		result &= kernel_ops_del_ipsec_spi(tbd->spi,
						   tbd->protocol,
						   &tbd->src, &tbd->dst,
						   child->sa.st_logger);
	}

	return result;
}

const struct kernel_ops *const kernel_stacks[] = {
#ifdef KERNEL_XFRM
	&xfrm_kernel_ops,
#endif
#ifdef KERNEL_PFKEYV2
	&pfkeyv2_kernel_ops,
#endif
	NULL,
};

const struct kernel_ops *kernel_ops = NULL/*kernel_stacks[0]*/;

deltatime_t bare_shunt_interval = DELTATIME_INIT(SHUNT_SCAN_INTERVAL);

void init_kernel(struct logger *logger)
{
	struct utsname un;

	/* get kernel version */
	uname(&un);
	llog(RC_LOG, logger,
	     "using %s %s kernel support code on %s",
	     un.sysname, kernel_ops->interface_name, un.version);

	passert(kernel_ops->init != NULL);
	kernel_ops->init(logger);

	/* Add the port bypass polcies */

	if (kernel_ops->v6holes != NULL) {
		/* may not return */
		kernel_ops->v6holes(logger);
	}

	enable_periodic_timer(EVENT_SHUNT_SCAN, kernel_scan_shunts,
			      bare_shunt_interval);
}

void show_kernel_interface(struct show *s)
{
	if (kernel_ops != NULL) {
		show_comment(s, "using kernel interface: %s",
			     kernel_ops->interface_name);
	}
}

static bool install_outbound_ipsec_kernel_policies(struct child_sa *child, bool up)
{
	struct logger *logger = child->sa.st_logger;
	struct connection *c = child->sa.st_connection;

	if (is_labeled_child(c)) {
		ldbg(logger, "kernel: %s() skipping install of IPsec policies as security label", __func__);
		return true;
	}

	if (c->newest_routing_sa != SOS_NOBODY &&
	    c->newest_routing_sa != child->sa.st_serialno) {
		ldbg(logger, "kernel: %s() skipping kernel policies as already owner", __func__);
		return true;
	}

	ldbg(logger,
	     "kernel: %s() installing IPsec policies for "PRI_SO": connection is currently "PRI_SO" %s up=%s",
	     __func__,
	     pri_so(child->sa.st_serialno),
	     pri_so(c->newest_routing_sa),
	     enum_name(&routing_names, c->child.routing),
	     bool_str(up));

#ifdef IPSEC_CONNECTION_LIMIT
	unsigned new_spds = 0;
	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_cisco_split(spd)) {
			/* XXX: why is CISCO skipped? */
			continue;
		}

		if (spd->wip.conflicting.shunt == NULL) {
			new_spds++;
		}
	}
	if (num_ipsec_eroute + new_spds >= IPSEC_CONNECTION_LIMIT) {
		llog(RC_LOG_SERIOUS, logger,
		     "Maximum number of IPsec connections reached (%d)",
		     IPSEC_CONNECTION_LIMIT);
		return false;
	}
#endif

	bool ok = true;	/* sticky: once false, stays false */

	/*
	 * Install the IPsec kernel policies.
	 */

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (!ok) {
			break;
		}

		if (is_v1_cisco_split(spd)) {
			/* XXX: why is CISCO skipped? */
			continue;
		}

		enum kernel_policy_op op =
			(spd->wip.conflicting.shunt != NULL ? KERNEL_POLICY_OP_REPLACE :
			 KERNEL_POLICY_OP_ADD);
		if (spd->block) {
			llog(RC_LOG, logger, "state spd requires a block (and no CAT?)");
			ok = spd->wip.installed.kernel_policy =
				add_spd_kernel_policy(spd, op, DIRECTION_OUTBOUND,
						      SHUNT_KIND_BLOCK,
						      logger, HERE,
						      "install IPsec block policy");
		} else {
			ok = spd->wip.installed.kernel_policy =
				install_outbound_ipsec_kernel_policy(child, spd, op, HERE);
		}
	}

	/*
	 * Do we have to make a mess of the routing?
	 *
	 * Probably.  This code path needs a re-think.
	 */

	ldbg(logger, "kernel: %s() running updown-prepare", __func__);
	if (ok) {
		FOR_EACH_ITEM(spd, &c->child.spds) {

			if (is_v1_cisco_split(spd)) {
				continue;
			}

			if (spd->wip.conflicting.route == NULL) {
				/* a new route: no deletion required, but preparation is */
				if (!do_updown(UPDOWN_PREPARE, c, spd, &child->sa, logger))
					ldbg(logger, "kernel: prepare command returned an error");
			}
		}
	}

	ldbg(logger, "kernel: %s() running updown-route", __func__);
	FOR_EACH_ITEM(spd, &c->child.spds) {
		if (!ok) {
			break;
		}

		if (is_v1_cisco_split(spd)) {
			continue;
		}

		if (spd->wip.conflicting.route == NULL) {
			/* a new route: no deletion required, but preparation is */
			ok = spd->wip.installed.route =
				do_updown(UPDOWN_ROUTE, c, spd, &child->sa, logger);
		}
	}

	/*
	 * Do we have to notify the firewall?
	 *
	 * Yes if this is the first time that the tunnel is
	 * established (rekeys do not need to re-UP).
	 *
	 * Yes, if we are installing a tunnel eroute and the firewall
	 * wasn't notified for a previous tunnel with the same
	 * clients.  Any Previous tunnel would have to be for our
	 * connection, so the actual test is simple.
	 */

	if (up) {
		FOR_EACH_ITEM(spd, &c->child.spds) {

			if (!ok) {
				break;
			}

			if (is_v1_cisco_split(spd)) {
				continue;
			}

			ok = spd->wip.installed.up =
				do_updown(UPDOWN_UP, c, spd, &child->sa, logger);
		}
	}

	/*
	 * Finally clean up.
	 */

	if (!ok) {
		delete_cat_kernel_policies(child->sa.st_connection, child->sa.st_logger, HERE);
		FOR_EACH_ITEM(spd, &c->child.spds) {
			revert_kernel_policy(spd, &child->sa, logger);
		}
		return false;
	}

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd)) {
			continue;
		}

		struct bare_shunt **bspp = spd->wip.conflicting.shunt;
		if (bspp != NULL) {
			free_bare_shunt(bspp);
		}
		/* clear host shunts that clash with freshly installed route */
		clear_narrow_holds(&spd->local->client, &spd->remote->client, logger);
	}


#ifdef IPSEC_CONNECTION_LIMIT
	num_ipsec_eroute += new_spds;
	llog(RC_COMMENT, logger,
	     "%d IPsec connections are currently being managed",
	     num_ipsec_eroute);
#endif

	return true;
}

bool install_inbound_ipsec_sa(struct child_sa *child, where_t where)
{
	struct logger *logger = child->sa.st_logger;
	struct connection *c = child->sa.st_connection;

	ldbg(logger, "kernel: %s() for "PRI_SO": inbound "PRI_WHERE,
	     __func__, pri_so(child->sa.st_serialno),
	     pri_where(where));

	/*
	 * Pass +0: Lookup the status of each SPD.
	 *
	 * Still call find_spd_conflicts() when a sec_label so that
	 * the structure is zeroed (sec_labels ignore conflicts).
	 */

	/*
	 * if this is a transport SA, and overlapping SAs are supported, then
	 * this route is not necessary at all.
	 */
	PEXPECT(logger, !kernel_ops->overlap_supported); /* still WIP */
	if (kernel_ops->overlap_supported && c->config->child_sa.encap_mode == ENCAP_MODE_TRANSPORT) {
		ldbg(logger, "route-unnecessary: overlap and transport");
	}

	if (!get_connection_spd_conflicts(c, logger)) {
		return false;
	}

	/* now setup inbound SA */

	if (!setup_half_kernel_state(&child->sa, DIRECTION_INBOUND)) {
		ldbg(logger, "kernel: %s() failed to install inbound kernel state", __func__);
		return false;
	}

	if (!install_inbound_ipsec_kernel_policies(child)) {
		ldbg(logger, "kernel: %s() failed to install inbound kernel policy", __func__);
		return false;
	}

	return true;
}

bool install_outbound_ipsec_sa(struct child_sa *child, bool up, where_t where)
{
	struct logger *logger = child->sa.st_logger;
	struct connection *c = child->sa.st_connection;

	ldbg(logger, "kernel: %s() for "PRI_SO": outbound "PRI_WHERE,
	     __func__, pri_so(child->sa.st_serialno),
	     pri_where(where));

	/*
	 * Pass +0: Lookup the status of each SPD.
	 *
	 * Still call find_spd_conflicts() when a sec_label so that
	 * the structure is zeroed (sec_labels ignore conflicts).
	 */

	/*
	 * if this is a transport SA, and overlapping SAs are supported, then
	 * this route is not necessary at all.
	 */
	PEXPECT(logger, !kernel_ops->overlap_supported); /* still WIP */
	if (kernel_ops->overlap_supported && c->config->child_sa.encap_mode == ENCAP_MODE_TRANSPORT) {
		ldbg(logger, "route-unnecessary: overlap and transport");
	}

	if (!get_connection_spd_conflicts(c, logger)) {
		return false;
	}

	/* (attempt to) actually set up the SA group */

	if (!setup_half_kernel_state(&child->sa, DIRECTION_OUTBOUND)) {
		ldbg(logger, "kernel: %s() failed to install outbound kernel state", __func__);
		return false;
	}

	if (!install_outbound_ipsec_kernel_policies(child, up)) {
		return false;
	}

	/*
	 * Transfer ownership of the connection's IPsec SA (kernel
	 * state) to the new Child.
	 *
	 * For IKEv2, both inbound and outbound IPsec SAs are
	 * installed at the same time so direction doesn't matter.
	 *
	 * For IKEv1, on the responder during quick mode, inbound and
	 * then outbound IPsec SAs are installed during separate
	 * exchanges, hence direction does matter.
	 *
	 * Since the above code updates routing, the routing owner
	 * should match the child.
	 */

#if 0
	/*
	 * XXX: triggers when two peers initiate
	 * simultaneously eventually finding themselves
	 * fighting over the same Child SA, for instance in
	 * ikev2-systemrole-04-mesh.
	 */
	PEXPECT(child->sa.st_logger, new_ipsec_sa >= old_ipsec_sa);
#endif
#if 0
	PEXPECT(child->sa.st_logger, routing_sa == new_ipsec_sa);
#endif

	/*
	 * We successfully installed an IPsec SA, meaning it
	 * is safe to clear our revival back-off delay. This
	 * is based on the assumption that an unwilling
	 * partner might complete an IKE SA to us, but won't
	 * complete an IPsec SA to us.
	 */
	child->sa.st_connection->revival.attempt = 0;
	child->sa.st_connection->revival.delay = deltatime(0);

	/* we only audit once for IPsec SA's, we picked the inbound SA */

	linux_audit_conn(&child->sa, LAK_CHILD_START);

	return true;
}

void uninstall_kernel_states(struct child_sa *child)
{
	if (child->sa.st_esp.present || child->sa.st_ah.present) {
		/* ESP or AH means this was an established IPsec SA */
		linux_audit_conn(&child->sa, LAK_CHILD_DESTROY);
	}

	uninstall_kernel_state(child, DIRECTION_OUTBOUND);
	/* For larval IPsec SAs this may not exist */
	uninstall_kernel_state(child, DIRECTION_INBOUND);
}

void teardown_ipsec_kernel_states(struct child_sa *child)
{
	/* caller snafued with pexpect_child_sa(st) */
	if (pbad(child == NULL)) {
		return;
	}

	switch (child->sa.st_ike_version) {
	case IKEv1:
		if (IS_IPSEC_SA_ESTABLISHED(&child->sa)) {
#if 0
			/* see comments below about multiple calls */
			PEXPECT(logger, c->child.routing == RT_ROUTED_TUNNEL);
#endif
			uninstall_kernel_states(child);
		}
		break;
	case IKEv2:
		if (IS_CHILD_SA_ESTABLISHED(&child->sa)) {
			/*
			 * XXX: There's a race when an SA is replaced
			 * simultaneous to the pluto being shutdown.
			 *
			 * For instance, ikev2-13-ah, this pexpect is
			 * triggered because #2, which was replaced by
			 * #3, tries to tear down the SA.
			 */
#if 0
			PEXPECT(logger, c->child.routing == RT_ROUTED_TUNNEL);
#endif
			uninstall_kernel_states(child);
		} else if (child->sa.st_sa_role == SA_INITIATOR &&
			   child->sa.st_sa_type_when_established == IPSEC_SA) {
			/*
			 * XXX: so much for dreams of becoming an
			 * established Child SA.
			 *
			 * This seems to be is overkill as just the
			 * outgoing SA needs to be deleted?
			 *
			 * Actually, no.  During acquire the
			 * prospective hold installs both inbound and
			 * outbound kernel policies?
			 */
			uninstall_kernel_states(child);
		}
		break;
	}
}

/*
 * Check if there was traffic on given SA during the last idle_max
 * seconds.  If TRUE, the SA was idle and DPD exchange should be
 * performed.  If FALSE, DPD is not necessary.  We also return TRUE
 * for errors, as they could mean that the SA is broken and needs to
 * be replace anyway.
 *
 * note: this mutates *st by calling get_sa_bundle_info
 *
 * XXX:
 *
 * The use of get_sa_bundle_info() here is likely bogus.  The function
 * returns the SA's add time (PF_KEY v2 documents it as such, xfrm
 * returns the .add_time field so presumably ...) when it is assumed
 * to be returning the idle time.
 *
 * Code most likely needs to track data+call-time and see if traffic
 * flowed since the last call.
 */

bool was_eroute_idle(struct child_sa *child, deltatime_t since_when)
{
	passert(child != NULL);
	struct ipsec_proto_info *first_proto_info =
		(child->sa.st_ah.present ? &child->sa.st_ah :
		 child->sa.st_esp.present ? &child->sa.st_esp :
		 child->sa.st_ipcomp.present ? &child->sa.st_ipcomp :
		 NULL);

	if (!get_ipsec_traffic(child, first_proto_info, DIRECTION_INBOUND)) {
		/* snafu; assume idle!?! */
		return true;
	}
	deltatime_t idle_time = realtimediff(realnow(), first_proto_info->inbound.last_used);
	return deltatime_cmp(idle_time, >=, since_when);
}

static void set_sa_info(struct ipsec_proto_info *p2, uint64_t bytes,
			 uint64_t add_time, bool inbound, deltatime_t *ago)
{
	if (p2->add_time == 0 && add_time != 0) {
		p2->add_time = add_time; /* this should happen exactly once */
	}

	pexpect(p2->add_time == add_time);

	realtime_t now = realnow();

	if (inbound) {
		if (bytes > p2->inbound.bytes) {
			p2->inbound.bytes = bytes;
			p2->inbound.last_used = now;
		}
		if (ago != NULL) {
			*ago = realtimediff(now, p2->inbound.last_used);
		}
	} else {
		if (bytes > p2->outbound.bytes) {
			p2->outbound.bytes = bytes;
			p2->outbound.last_used = now;
		}
		if (ago != NULL)
			*ago = realtimediff(now, p2->outbound.last_used);
	}
}

/*
 * get information about a given SA bundle
 *
 * Note: this mutates *st.
 * Note: this only changes counts in the first SA in the bundle!
 */
bool get_ipsec_traffic(struct child_sa *child,
		       struct ipsec_proto_info *proto_info,
		       enum direction direction)
{
	struct connection *const c = child->sa.st_connection;

	if (!pexpect(proto_info != NULL)) {
		/* pacify coverity */
		return false;
	}

	if (kernel_ops->get_kernel_state == NULL) {
		return false;
	}

	/*
	 * If we're being redirected (using the REDIRECT mechanism),
	 * then use the state's current remote endpoint, and not the
	 * connection's value.
	 *
	 * XXX: why not just use redirect_ip?
	 */
	bool redirected = (!endpoint_address_eq_address(child->sa.st_remote_endpoint, c->remote->host.addr) &&
			   address_is_specified(c->redirect.ip));
	ip_address remote_ip = (redirected ?  endpoint_address(child->sa.st_remote_endpoint) :
				c->remote->host.addr);

	struct ipsec_flow *flow;
	ip_address src, dst;
	switch (direction) {
	case DIRECTION_INBOUND:
		flow = &proto_info->inbound;
		src = remote_ip;
		dst = c->local->host.addr;
		break;
	case DIRECTION_OUTBOUND:
		flow = &proto_info->outbound;
		src = c->local->host.addr;
		dst = remote_ip;
		break;
	default:
		bad_case(direction);
	}

	if (flow->kernel_sa_expired & SA_HARD_EXPIRED) {
		ldbg_sa(child,
			"kernel: %s() expired %s SA SPI "PRI_IPSEC_SPI" get_sa_info()",
			__func__, enum_name_short(&direction_names, direction),
			pri_ipsec_spi(flow->spi));
		return true; /* all is well use last known info */
	}

	said_buf sb;
	struct kernel_state sa = {
		.spi = flow->spi,
		.proto = proto_info->protocol,
		.src.address = src,
		.dst.address = dst,
		.story = said_str(dst, proto_info->protocol, flow->spi, &sb),
	};

	ldbg_sa(child, "kernel: %s() %s", __func__, sa.story);

	uint64_t bytes = 0;
	uint64_t add_time = 0;
	uint64_t lastused = 0;
	if (!kernel_ops->get_kernel_state(&sa, &bytes, &add_time, &lastused,
					  child->sa.st_logger))
		return false;
	ldbg_sa(child, "kernel: %s() bytes=%"PRIu64" add_time=%"PRIu64" lastused=%"PRIu64,
		__func__, bytes, add_time, lastused);

	proto_info->add_time = add_time;

	/* field has been set? */
	passert(!is_realtime_epoch(flow->last_used));

	if (bytes > flow->bytes) {
		flow->bytes = bytes;
		if (lastused > 0)
			flow->last_used = realtime(lastused);
		else
			flow->last_used = realnow();
	}

	return true;
}

void orphan_holdpass(struct connection *c,
		     struct spd_route *sr,
		     struct logger *logger)
{
	/*
	 * ... UPDATE kernel policy if needed.
	 *
	 * This really causes the name to remain "oe-failing",
	 * we should be able to update only the name of the
	 * shunt.
	 */

	dbg("kernel: installing bare_shunt/failure_shunt");

	/* fudge up parameter list */
	const ip_address *src_address = &sr->local->host->addr;
	const ip_address *dst_address = &sr->remote->host->addr;
	const char *why = "oe-failing";

	/* fudge up replace_bare_shunt() */
	const struct ip_info *afi = address_type(src_address);
	passert(afi == address_type(dst_address));
	const struct ip_protocol *protocol = protocol_from_ipproto(sr->local->client.ipproto);
	/* ports? assumed wide? */
	ip_selector src = selector_from_address_protocol(*src_address, protocol);
	ip_selector dst = selector_from_address_protocol(*dst_address, protocol);

	selector_pair_buf sb;
	dbg("kernel: replace bare shunt %s for %s",
	    str_selector_pair(&src, &dst, &sb), why);

	/*
	 * ??? this comment might be obsolete.
	 *
	 * If the transport protocol is not the wildcard (0),
	 * then we need to look for a host<->host shunt, and
	 * replace that with the shunt spi, and then we add a
	 * %HOLD for what was there before.
	 *
	 * This is at odds with !repl, which should delete
	 * things.
	 *
	 * XXX: does replacing a sec_label kernel policy with
	 * something bare make sense?  Should sec_label be
	 * included?
	 */

	struct nic_offload nic_offload = {};
	setup_esp_nic_offload(&nic_offload, c, NULL);
	if (install_bare_kernel_policy(src, dst,
				       SHUNT_KIND_FAILURE, c->config->shunt[SHUNT_KIND_FAILURE],
				       &nic_offload,
				       logger, HERE)) {
		/*
		 * If the bare shunt exactly matches the template,
		 * then mark the template as needing restoring when
		 * the bare shunt expires.
		 *
		 * XXX: as a quick and dirty hack, assume there's only
		 * one selector.
		 */
		co_serial_t restore;
		struct connection *t = c->clonedfrom;
		if (selector_eq_selector(src, t->local->child.selectors.proposed.list[0]) &&
		    selector_eq_selector(dst, t->remote->child.selectors.proposed.list[0])) {
			restore = t->serialno;
		} else {
			restore = UNSET_CO_SERIAL;
		}

		/*
		 * Change over to new bare eroute ours, peers,
		 * transport_proto are the same.
		 */


		struct bare_shunt *bs =
			add_bare_shunt(&src, &dst,
				       c->config->failure_shunt,
				       restore,
				       why, logger);
		ldbg_bare_shunt(logger, "replace", bs);
	} else {
		llog(RC_LOG, logger,
		     "replace kernel shunt %s failed - deleting from pluto shunt table",
		     str_selector_pair_sensitive(&src, &dst, &sb));
	}

}

static void expire_bare_shunts(struct logger *logger)
{
	dbg("kernel: checking for aged bare shunts from shunt table to expire");
	for (struct bare_shunt **bspp = &bare_shunts; *bspp != NULL; ) {
		struct bare_shunt *bsp = *bspp;
		time_t age = deltasecs(monotimediff(mononow(), bsp->last_activity));

		if (age > deltasecs(pluto_shunt_lifetime)) {
			ldbg_bare_shunt(logger, "expiring old", bsp);
			if (co_serial_is_set(bsp->restore_serialno)) {
				/*
				 * Time to restore the connection's
				 * shunt.  Presumably the bare shunt
				 * was a place holder while things
				 * were given time to rest (back-off).
				 */
				struct connection *c = connection_by_serialno(bsp->restore_serialno);
				if (c != NULL) {
					enum shunt_kind shunt_kind = (never_negotiate(c) ? SHUNT_KIND_NEVER_NEGOTIATE :
								      SHUNT_KIND_ONDEMAND);
					if (!install_prospective_kernel_policies(c->spd,
										 shunt_kind,
										 logger, HERE)) {
						llog(RC_LOG, logger,
						     "trap shunt install failed ");
					}
				}
			} else {
				delete_bare_shunt_kernel_policy(bsp,
								EXPECT_KERNEL_POLICY_OK,
								logger, HERE);
			}
			free_bare_shunt(bspp);
		} else {
			ldbg_bare_shunt(logger, "keeping recent", bsp);
			bspp = &bsp->next;
		}
	}
}

static void delete_bare_shunt_kernel_policies(struct logger *logger)
{
	dbg("kernel: emptying bare shunt table");
	while (bare_shunts != NULL) { /* nothing left */
		const struct bare_shunt *bsp = bare_shunts;
		delete_bare_shunt_kernel_policy(bsp,
						EXPECT_KERNEL_POLICY_OK,
						logger, HERE);
		free_bare_shunt(&bare_shunts); /* also updates BARE_SHUNTS */
	}
}

static void kernel_scan_shunts(struct logger *logger)
{
	expire_bare_shunts(logger);
}

void shutdown_kernel(struct logger *logger)
{
	delete_bare_shunt_kernel_policies(logger);
	kernel_ops->shutdown(logger);
}

void handle_sa_expire(ipsec_spi_t spi, uint8_t protoid, ip_address dst,
		      bool hard, uint64_t bytes, uint64_t packets, uint64_t add_time,
		      struct logger *logger)
{
	struct child_sa *child = find_v2_child_sa_by_spi(spi, protoid, dst);

	if (child == NULL) {
		address_buf a;
		ldbg(logger,
		     "received kernel %s EXPIRE event for IPsec SPI "PRI_IPSEC_SPI", but there is no connection with this SPI and dst %s bytes %" PRIu64 " packets %" PRIu64,
		     hard ? "hard" : "soft",
		     pri_ipsec_spi(spi),
		     str_address(&dst, &a), bytes, packets);
		return;
	}

	const struct connection *c = child->sa.st_connection;

	if ((hard && impair.ignore_hard_expire) ||
	    (!hard && impair.ignore_soft_expire)) {
		address_buf a;
		llog_sa(RC_LOG, child,
			"IMPAIR: suppressing a %s EXPIRE event spi "PRI_IPSEC_SPI" dst %s bytes %" PRIu64 " packets %" PRIu64,
			hard ? "hard" : "soft",
			pri_ipsec_spi(spi),
			str_address(&dst, &a),
			bytes, packets);
		return;
	}

	bool rekey = c->config->rekey;
	bool newest = c->newest_ipsec_sa == child->sa.st_serialno;
	struct state *st =  &child->sa;
	struct ipsec_proto_info *pr = (st->st_esp.present ? &st->st_esp :
				       st->st_ah.present ? &st->st_ah :
				       st->st_ipcomp.present ? &st->st_ipcomp :
				       NULL);

	bool already_softexpired = ((pr->inbound.kernel_sa_expired & SA_SOFT_EXPIRED) ||
				    (pr->outbound.kernel_sa_expired & SA_SOFT_EXPIRED));

	bool already_hardexpired = ((pr->inbound.kernel_sa_expired & SA_HARD_EXPIRED) ||
				    (pr->outbound.kernel_sa_expired & SA_HARD_EXPIRED));

	enum sa_expire_kind expire = hard ? SA_HARD_EXPIRED : SA_SOFT_EXPIRED;

	/*
	 * OUR_SPI was sent by us to our peer, so that our peer can
	 * include it in all inbound IPsec messages.
	 */
	const bool inbound = (pr->inbound.spi == spi);

	llog_sa(RC_LOG, child,
		"received %s EXPIRE for %s SPI "PRI_IPSEC_SPI" bytes %" PRIu64 " packets %" PRIu64 " rekey=%s%s%s%s%s",
		hard ? "hard" : "soft",
		(inbound ? "inbound" : "outbound"), pri_ipsec_spi(spi),
		bytes, packets,
		rekey ?  "yes" : "no",
		already_softexpired ? "; already soft expired" : "",
		already_hardexpired ? "; already hard expired" : "",
		(newest ? "" : "; deleting old SA"),
		(newest && rekey && !already_softexpired && !already_hardexpired) ? "; replacing" : "");

	if ((already_softexpired && expire == SA_SOFT_EXPIRED)  ||
	    (already_hardexpired && expire == SA_HARD_EXPIRED)) {
		dbg("#%lu one of the SA has already expired ignore this %s EXPIRE",
		    child->sa.st_serialno, hard ? "hard" : "soft");
		/*
		 * likely the other direction SA EXPIRED, it triggered a rekey first.
		 * It should be safe to ignore the second one. No need to log.
		 */
	} else if (!already_hardexpired && expire == SA_HARD_EXPIRED) {
		if (inbound) {
			pr->inbound.kernel_sa_expired |= expire;
			set_sa_info(pr, bytes, add_time, true /* inbound */, NULL);
		} else {
			pr->outbound.kernel_sa_expired |= expire;
			set_sa_info(pr, bytes, add_time, false /* outbound */, NULL);
		}
		set_sa_expire_next_event(EVENT_SA_EXPIRE, &child->sa);
	} else if (newest && rekey && !already_hardexpired && !already_softexpired && expire == SA_SOFT_EXPIRED) {
		if (inbound) {
			pr->inbound.kernel_sa_expired |= expire;
			set_sa_info(pr, bytes, add_time, true /* inbound */, NULL);
		} else {
			pr->outbound.kernel_sa_expired |= expire;
			set_sa_info(pr, bytes, add_time, false /* outbound */, NULL);
		}
		set_sa_expire_next_event(EVENT_NULL/*either v2 REKEY or v1 REPLACE*/, &child->sa);
	} else {
		/*
		 * 'if' and multiple 'else if's are using multiple variables.
		 * I may have overlooked some cases. lets break hard on unexpected cases.
		 */
		passert(1); /* lets break! */
	}
}

void jam_kernel_acquire(struct jambuf *buf, const struct kernel_acquire *b)
{
	jam(buf, "initiate on-demand for packet ");
	jam_packet(buf, &b->packet);
	if (!b->by_acquire) {
		jam(buf, " by whack");
	}
	if (b->sec_label.len > 0) {
		jam(buf, " sec_label=");
		jam_sanitized_hunk(buf, b->sec_label);
	}
#if 0
	if (b->state_id > 0) {
		jam(buf, " seq=%u", (unsigned)b->state_id);
	}
	if (b->policy_id > 0) {
		jam(buf, " policy=%u", (unsigned)b->policy_id);
	}
#endif
}
