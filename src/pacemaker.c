/* 
 * Copyright (C) 2011 Jiaju Zhang <jjzhang@suse.de>
 * Copyright (C) 2013-2014 Philipp Marek <philipp.marek@linbit.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "log.h"
#include "pacemaker.h"
#include "inline-fn.h"


enum atomic_ticket_supported {
	YES=0,
	NO,
	FILENOTFOUND,  /* Ie. UNKNOWN */
	UNKNOWN = FILENOTFOUND,
};
/* http://thedailywtf.com/Articles/What_Is_Truth_0x3f_.aspx */


enum atomic_ticket_supported atomicity = UNKNOWN;



#define COMMAND_MAX	1024


/** Determines whether the installed crm_ticket can do atomic ticket grants,
 * _including_ multiple attribute changes.
 *
 * See
 *   https://bugzilla.novell.com/show_bug.cgi?id=855099
 *
 * Run "crm_ticket" without "--force";
 *   - the old version asks for "Y/N" via STDIN, and returns 0
 *     when reading "no";
 *   - the new version just reports an error without asking.
 */
static void test_atomicity(void)
{
	int rv;

	if (atomicity != UNKNOWN)
		return;

	rv = system("echo n | crm_ticket -g -t any-ticket-name > /dev/null 2> /dev/null");
	if (rv == -1) {
		log_error("Cannot run \"crm_ticket\"!");
		/* BIG problem. Abort. */
		exit(1);
	}

	if (WIFSIGNALED(rv)) {
		log_error("\"crm_ticket\" terminated by a signal!");
		/* Problem. Abort. */
		exit(1);
	}

	switch (WEXITSTATUS(rv)) {
	case 0:
		atomicity = NO;
		log_info("Old \"crm_ticket\" found, using non-atomic ticket updates.");
		break;

	case 1:
		atomicity = YES;
		log_info("New \"crm_ticket\" found, using atomic ticket updates.");
		break;

	default:
		log_error("Unexpected return value from \"crm_ticket\" (%d), "
				"falling back to non-atomic ticket updates.",
				rv);
		atomicity = NO;
	}

	assert(atomicity == YES || atomicity == NO);
}


const char * interpret_rv(int rv)
{
	static char text[64];
	int p;


	if (rv == 0)
		return "0";

	p = sprintf(text, "rv %d", WEXITSTATUS(rv));

	if (WIFSIGNALED(rv))
		sprintf(text + p, "  signal %d", WTERMSIG(rv));

	return text;
}


static int pcmk_write_ticket_atomic(struct ticket_config *tk, int grant)
{
	char cmd[COMMAND_MAX];
	int rv;


	/* The values are appended to "-v", so that NO_ONE
	 * (which is -1) isn't seen as another option. */
	snprintf(cmd, COMMAND_MAX,
			"crm_ticket -t '%s' "
			"%s --force "
			"-S owner -v%" PRIi32 " "
			"-S expires -v%" PRIi64 " "
			"-S term -v%" PRIi64,
			tk->name,
			(grant > 0 ? "-g" :
			 grant < 0 ? "-r" :
			 ""),
			(int32_t)get_node_id(tk->leader),
			(int64_t)wall_ts(tk->term_expires),
			(int64_t)tk->current_term);

	rv = system(cmd);
	log_debug("command: '%s' was executed", cmd);
	if (rv != 0)
		log_error("error: \"%s\" failed, %s", cmd, interpret_rv(rv));

	return rv;
}


static int pcmk_store_ticket_nonatomic(struct ticket_config *tk);

static int pcmk_grant_ticket(struct ticket_config *tk)
{
	char cmd[COMMAND_MAX];
	int rv;


	test_atomicity();
	if (atomicity == YES)
		return pcmk_write_ticket_atomic(tk, +1);


	rv = pcmk_store_ticket_nonatomic(tk);
	if (rv)
		return rv;

	snprintf(cmd, COMMAND_MAX, "crm_ticket -t %s -g --force",
			tk->name);
	log_debug("command: '%s' was executed", cmd);
	rv = system(cmd);
	if (rv != 0)
		log_error("error: \"%s\" failed, %s", cmd, interpret_rv(rv));
	return rv;
}


static int pcmk_revoke_ticket(struct ticket_config *tk)
{
	char cmd[COMMAND_MAX];
	int rv;


	test_atomicity();
	if (atomicity == YES)
		return pcmk_write_ticket_atomic(tk, -1);


	rv = pcmk_store_ticket_nonatomic(tk);
	if (rv)
		return rv;

	snprintf(cmd, COMMAND_MAX, "crm_ticket -t %s -r --force",
			tk->name);
	log_debug("command: '%s' was executed", cmd);
	rv = system(cmd);
	if (rv != 0)
		log_error("error: \"%s\" failed, %s", cmd, interpret_rv(rv));
	return rv;
}


static int crm_ticket_set(const struct ticket_config *tk, const char *attr, int64_t val)
{
	char cmd[COMMAND_MAX];
	int i, rv;


	snprintf(cmd, COMMAND_MAX,
		 "crm_ticket -t '%s' -S '%s' -v %" PRIi64,
		 tk->name, attr, val);
	/* If there are errors, there's not much we can do but retry ... */
	for (i=0; i<3 &&
			(rv = system(cmd));
			i++) ;

	log_debug("'%s' gave result %s", cmd, interpret_rv(rv));

	return rv;
}


static int pcmk_store_ticket_nonatomic(struct ticket_config *tk)
{
	int rv;

	/* Always try to store *each* attribute, even if there's an error
	 * for one of them. */
	rv = crm_ticket_set(tk, "owner", (int32_t)get_node_id(tk->leader));
	rv = crm_ticket_set(tk, "expires", wall_ts(tk->term_expires))  || rv;
	rv = crm_ticket_set(tk, "term", tk->current_term)     || rv;

	if (rv)
		log_error("setting crm_ticket attributes failed; %s",
				interpret_rv(rv));
	else
		log_info("setting crm_ticket attributes successful");

	return rv;
}


static int crm_ticket_get(struct ticket_config *tk,
		const char *attr, int64_t *data)
{
	char cmd[COMMAND_MAX];
	char line[256];
	int rv;
	int64_t v;
	FILE *p;


	*data = -1;
	v = 0;
	snprintf(cmd, COMMAND_MAX,
			"crm_ticket -t '%s' -G '%s' --quiet",
			tk->name, attr);

	p = popen(cmd, "r");
	if (p == NULL) {
		rv = errno;
		log_error("popen error %d (%s) for \"%s\"",
				rv, strerror(rv), cmd);
		return rv || -EINVAL;
	}
	if (fgets(line, sizeof(line) - 1, p) == NULL) {
		rv = ENODATA;
		goto out;
	}

	rv = EINVAL;
	if (!strncmp(line, "false", 5)) {
		v = 0;
		rv = 0;
	} else if (!strncmp(line, "true", 4)) {
		v = 1;
		rv = 0;
	} else if (sscanf(line, "%" PRIi64, &v) == 1) {
		rv = 0;
	}

	*data = v;

out:
	rv = pclose(p);
	log_debug("command \"%s\" returned %s, value %" PRIi64, cmd, interpret_rv(rv), v);
	return rv;
}


static int pcmk_load_ticket(struct ticket_config *tk)
{
	int rv;
	int64_t v;


	/* This here gets run during startup; testing that here means that
	 * normal operation won't be interrupted with that test. */
	test_atomicity();


	rv = crm_ticket_get(tk, "expires", &v);
	if (!rv) {
		tk->term_expires = unwall_ts(v);
	}

	rv = crm_ticket_get(tk, "term", &v);
	if (!rv) {
		tk->current_term = v;
	}

	rv = crm_ticket_get(tk, "granted", &v);
	if (!rv) {
		tk->is_granted = v;
	}

	rv = crm_ticket_get(tk, "owner", &v);
	if (!rv) {
		/* No check, node could have been deconfigured. */
		if (!find_site_by_id(v, &tk->leader)) {
			/* Hmm, no site found for the ticket we have in the
			 * CIB!?
			 * Assume that the ticket belonged to us if it was
			 * granted here!
			 */
			log_warn("%s: no site matches; site got reconfigured?",
				tk->name);
			if (tk->is_granted) {
				log_warn("%s: granted here, assume it belonged to us",
					tk->name);
				tk->leader = local;
			}
		}
	}

	return rv;
}


struct ticket_handler pcmk_handler = {
	.grant_ticket   = pcmk_grant_ticket,
	.revoke_ticket  = pcmk_revoke_ticket,
	.load_ticket    = pcmk_load_ticket,
};
