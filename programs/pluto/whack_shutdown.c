/* shutdown pluto, for libreswan
 *
 * Copyright (C) 1997      Angelos D. Keromytis.
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2007 Ken Bantoft <ken@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2009-2016 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012-2016 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Kim B. Heino <b@bbbs.net>
 * Copyright (C) 2012 Philippe Vouters <Philippe.Vouters@laposte.net>
 * Copyright (C) 2012 Wes Hardaker <opensource@hardakers.net>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2016-2020 Andrew Cagney
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

#include <unistd.h>		/* for exit(2) */

#include "whack_shutdown.h"
#include "whack_delete.h"	/* for whack_delete_connection() */

#include "constants.h"
#include "lswconf.h"		/* for lsw_conf_free_oco() */
#include "lswnss.h"		/* for lsw_nss_shutdown() */
#include "lswalloc.h"		/* for report_leaks() et.al. */

#include "defs.h"		/* for so_serial_t */
#include "log.h"		/* for close_log() et.al. */

#include "server_pool.h"	/* for stop_crypto_helpers() */
#include "pluto_sd.h"		/* for pluto_sd() */
#include "root_certs.h"		/* for free_root_certs() */
#include "keys.h"		/* for free_preshared_secrets() */
#include "connections.h"
#include "fetch.h"		/* for stop_crl_fetch_helper() et.al. */
#include "crl_queue.h"		/* for free_crl_queue() */
#include "iface.h"		/* for shutdown_ifaces() */
#include "kernel.h"		/* for kernel_ops.shutdown() and free_kernel() */
#include "virtual_ip.h"		/* for free_virtual_ip() */
#include "server.h"		/* for free_server() */
#include "revival.h"		/* for free_revivals() */
#ifdef USE_DNSSEC
#include "dnssec.h"		/* for unbound_ctx_free() */
#endif
#include "demux.h"		/* for free_demux() */
#include "impair_message.h"	/* for free_impair_message() */
#include "state_db.h"		/* for check_state_db() */
#include "connection_db.h"	/* for check_connection_db() */
#include "spd_route_db.h"	/* for check_spd_db() */
#include "server_fork.h"	/* for check_server_fork() */
#include "pending.h"
#include "connection_event.h"

volatile bool exiting_pluto = false;
static enum pluto_exit_code pluto_exit_code;

/*
 * leave pluto, with status.
 * Once child is launched, parent must not exit this way because
 * the lock would be released.
 *
 *  0 OK
 *  1 general discomfort
 * 10 lock file exists
 */

static void exit_prologue(enum pluto_exit_code code);
static void exit_epilogue(void) NEVER_RETURNS;
static void server_helpers_stopped_callback(void);

void libreswan_exit(enum pluto_exit_code exit_code)
{
	exit_prologue(exit_code);
	exit_epilogue();
}

static void exit_prologue(enum pluto_exit_code exit_code)
{
	/*
	 * Tell the world, well actually all the threads, that pluto
	 * is exiting and they should quit.  Even if pthread_cancel()
	 * weren't buggy, using it correctly would be hard, so use
	 * this instead.
	 */
	exiting_pluto = true;
	pluto_exit_code = exit_code;

	/* needed because we may be called in odd state */
 #ifdef USE_SYSTEMD_WATCHDOG
	pluto_sd(PLUTO_SD_STOPPING, exit_code);
 #endif
}

static void delete_every_connection(void)
{
	/*
	 * Keep deleting the newest connection until there isn't one.
	 *
	 * Deleting new-to-old means that instances are deleted before
	 * templates.  Picking away at the queue avoids the posability
	 * of a cascading delete deleting multiple connections.
	 */
	while (true) {
		struct connection_filter cq = { .where = HERE, };
		if (!next_connection_new2old(&cq)) {
			break;
		}

		whack_delete_connection(&cq.c, &global_logger);
	}
}

void exit_epilogue(void)
{
	struct logger logger[1] = { global_logger, };

	if (pluto_exit_code == PLUTO_EXIT_LEAVE_STATE) {
		lsw_nss_shutdown();
		free_preshared_secrets(logger);
		delete_lock();	/* delete any lock files */
		close_log();	/* close the logfiles */
#ifdef USE_SYSTEMD_WATCHDOG
		pluto_sd(PLUTO_SD_EXIT, PLUTO_EXIT_LEAVE_STATE);
#endif
		exit(PLUTO_EXIT_LEAVE_STATE);
	}

	/*
	 * Before ripping everything down; check internal state.
	 */
	state_db_check(logger);
	connection_db_check(logger);
	spd_route_db_check(logger);
	check_server_fork(logger); /*pid_entry_db_check()*/

	/*
	 * This should wipe pretty much everything: states, revivals,
	 * ...
	 */
	delete_every_connection();

	free_server_helper_jobs(logger);

	free_root_certs(logger);
	free_preshared_secrets(logger);
	free_remembered_public_keys();
	/*
	 * free memory allocated by initialization routines.  Please don't
	 * forget to do this.
	 */

#if defined(LIBCURL) || defined(LIBLDAP)
	/*
	 * Wait for the CRL fetch handler to finish its current task.
	 * Without this CRL fetch requests are left hanging and, after
	 * the NSS DB has been closed (below), the helper can crash.
	 */
	stop_crl_fetch_helper(logger);
	/*
	 * free the crl list that the fetch-helper is currently
	 * processing
	 */
	free_crl_fetch();
	/*
	 * free the crl requests that are waiting to be picked and
	 * processed by the fetch-helper.
	 */
	free_crl_queue();
#endif

	lsw_conf_free_oco();	/* free global_oco containing path names */

	/*
	 * The impair_message code has pointers to to msg_digest
	 * which, in turn has pointers to iface.  Hence it must be
	 * shutdown (and links released) before the interfaces.
	 */
	shutdown_impair_message(logger);

	shutdown_ifaces(logger);	/* free interface list from memory */
	shutdown_kernel(logger);
	lsw_nss_shutdown();
	delete_lock();	/* delete any lock files */
#ifdef USE_DNSSEC
	unbound_ctx_free();	/* needs event-loop aka server */
#endif

	/*
	 * No libevent events beyond this point.
	 */
	free_server();

	free_virtual_ip();	/* virtual_private= */
	free_pluto_main();	/* our static chars */

	/* report memory leaks now, after all free_* calls */
	if (leak_detective) {
		report_leaks(logger);
	}
	close_log();	/* close the logfiles */
#ifdef USE_SYSTEMD_WATCHDOG
	pluto_sd(PLUTO_SD_EXIT, pluto_exit_code);
#endif
	exit(pluto_exit_code);	/* exit, with our error code */
}

void whack_shutdown(struct logger *logger, enum pluto_exit_code exit_code)
{
	switch (exit_code) {
	case PLUTO_EXIT_LEAVE_STATE:
		llog(LOG_STREAM|RC_LOG, logger, "Pluto is shutting down (leaving state)");
		break;
	case PLUTO_EXIT_OK:
		llog(LOG_STREAM|RC_LOG, logger, "Pluto is shutting down");
		break;
	default:
		bad_enum(logger, &pluto_exit_code_names, exit_code);
	}

	/*
	 * Leak (unlink but don't close aka delref) the currently
	 * attached whackfd.
	 *
	 * Unlinking the whackfd from the logger stops any further log
	 * messages reaching the attached whack (the entire exit
	 * process is radio silent).
	 *
	 * Leaving whackfd open means that whack will remain attached
	 * until pluto exits.
	 *
	 * See also whack_handle_cb() sets whackfd[0] to the current
	 * whack's FD before, indirectly, calling this function.
	 */
	fd_leak(logger->whackfd[0], HERE);

	/*
	 * Start the shutdown process.
	 *
	 * Flag that things are going down and delete anything that
	 * isn't asynchronous (or depends on something asynchronous).
	 */
	exit_prologue(exit_code);

	/*
	 * Wait for the crypto-helper threads to notice EXITING_PLUTO
	 * and exit (if necessary, wake any sleeping helpers from
	 * their slumber).  Without this any helper using NSS after
	 * the code below has shutdown the NSS DB will crash.
	 *
	 * This does not try to delete any tasks left waiting on the
	 * helper queue.  Instead, code further down deleting
	 * connections (which in turn deletes states) should clean
	 * that up?
	 *
	 * This also does not try to delete any completed tasks
	 * waiting on the event loop.  One theory is for the helper
	 * code to be changed so that helper tasks can be "cancelled"
	 * after the've completed?
	 */
	stop_server_helpers(server_helpers_stopped_callback);
	/*
	 * helper_threads_stopped_callback() is called once both all
	 * helper-threads have exited, and all helper-thread events
	 * lurking in the event-queue have been processed).
	 */
}

static void server_stopped_callback(int r) NEVER_RETURNS;

void server_helpers_stopped_callback(void)
{
	/*
	 * As libevent to shutdown the event-loop, once completed
	 * SERVER_STOPPED_CALLBACK is called.
	 *
	 * XXX: don't hardwire the callback - passing it in as an
	 * explicit parameter hopefully makes following the code flow
	 * a little easier(?).
	 */
	stop_server(server_stopped_callback);
	/*
	 * server_stopped() is called once the event-loop exits.
	 */
}

void server_stopped_callback(int r)
{
	dbg("event loop exited: %s",
	    r < 0 ? "an error occurred" :
	    r > 0 ? "no pending or active events" :
	    "success");

	exit_epilogue();
}
