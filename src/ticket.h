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

#ifndef _TICKET_H
#define _TICKET_H

#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "timer.h"
#include "config.h"
#include "log.h"

#define DEFAULT_TICKET_EXPIRY	600
#define DEFAULT_TICKET_TIMEOUT	5
#define DEFAULT_RETRIES			10


#define foreach_ticket(i_,t_) for(i=0; (t_=booth_conf->ticket+i, i<booth_conf->ticket_count); i++)
#define foreach_node(i_,n_) for(i=0; (n_=booth_conf->site+i, i<booth_conf->site_count); i++)


int check_ticket(char *ticket, struct ticket_config **tc);
int check_site(char *site, int *local);
int grant_ticket(struct ticket_config *ticket);
int revoke_ticket(struct ticket_config *ticket);
int list_ticket(char **pdata, unsigned int *len);

int message_recv(struct boothc_ticket_msg *msg, int msglen);
void reset_ticket(struct ticket_config *tk);
void update_ticket_state(struct ticket_config *tk, struct booth_site *sender);
int setup_ticket(void);
int check_max_len_valid(const char *s, int max);

int do_grant_ticket(struct ticket_config *ticket, int options);
int do_revoke_ticket(struct ticket_config *tk);

int find_ticket_by_name(const char *ticket, struct ticket_config **found);

void set_ticket_wakeup(struct ticket_config *tk);
int postpone_ticket_processing(struct ticket_config *tk);

int test_external_prog(struct ticket_config *tk, int start_election);
int acquire_ticket(struct ticket_config *tk, cmd_reason_t reason);

int ticket_answer_list(int fd, struct boothc_ticket_msg *msg);
int process_client_request(struct client *req_client,
	struct boothc_ticket_msg *msg);

int ticket_write(struct ticket_config *tk);

void process_tickets(void);
void tickets_log_info(void);
char *state_to_string(uint32_t state_ho);
int send_reject(struct booth_site *dest, struct ticket_config *tk,
	cmd_result_t code, struct boothc_ticket_msg *in_msg);
int send_msg (int cmd, struct ticket_config *tk,
	struct booth_site *dest, struct boothc_ticket_msg *in_msg);
void notify_client(struct ticket_config *tk, int rv);
int ticket_broadcast(struct ticket_config *tk, cmd_request_t cmd, cmd_request_t expected_reply, cmd_result_t res, cmd_reason_t reason);

int leader_update_ticket(struct ticket_config *tk);
void add_random_delay(struct ticket_config *tk);
void schedule_election(struct ticket_config *tk, cmd_reason_t reason);

static inline void ticket_next_cron_at(struct ticket_config *tk, timetype when)
{
	tk->next_cron = when;
}

static inline void ticket_next_cron_at_coarse(struct ticket_config *tk, time_t when)
{
	memset(&tk->next_cron, 0, sizeof(tk->next_cron));
	tk->next_cron.tv_sec  = when;
}

static inline void ticket_next_cron_in(struct ticket_config *tk, time_t seconds)
{
	timetype tv;

	get_time(&tv);
	tv.tv_sec += seconds;

	ticket_next_cron_at(tk, tv);
}


static inline void ticket_activate_timeout(struct ticket_config *tk)
{
	/* TODO: increase timeout when no answers */
	tk_log_debug("activate ticket timeout in %d", tk->timeout);
	ticket_next_cron_in(tk, tk->timeout);
}


#endif /* _TICKET_H */
