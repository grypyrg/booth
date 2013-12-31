/* 
 * Copyright (C) 2011 Jiaju Zhang <jjzhang@suse.de>
 * Copyright (C) 2013 Philipp Marek <philipp.marek@linbit.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <errno.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <pacemaker/crm/services.h>
#include <clplumbing/setproctitle.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <error.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include "log.h"
#include "booth.h"
#include "config.h"
#include "transport.h"
#include "inline-fn.h"
#include "pacemaker.h"
#include "ticket.h"

#define RELEASE_VERSION		"1.0"

#define CLIENT_NALLOC		32

int daemonize = 0;


/** Structure for "clients".
 * Filehandles with incoming data get registered here (and in pollfds),
 * along with their callbacks.
 * Because these can be reallocated with every new fd, addressing
 * happens _only_ by their numeric index. */
struct client *clients = NULL;
struct pollfd *pollfds = NULL;
static int client_maxi;
static int client_size = 0;


typedef enum
{
	BOOTHD_STARTED=0,
	BOOTHD_STARTING
} BOOTH_DAEMON_STATE;

int poll_timeout = POLL_TIMEOUT;

typedef enum {
	OP_LIST = 1,
	OP_GRANT,
	OP_REVOKE,
} operation_t;

struct command_line {
	int type;		/* ACT_ */
	int op;			/* OP_ */
	char configfile[BOOTH_PATH_LEN];
	char lockfile[BOOTH_PATH_LEN];

	char site[BOOTH_NAME_LEN];
	struct boothc_ticket_msg msg;
};

struct booth_config *booth_conf;
static struct command_line cl;

int do_read(int fd, void *buf, size_t count)
{
	int rv, off = 0;

	while (off < count) {
		rv = read(fd, (char *)buf + off, count - off);
		if (rv == 0)
			return -1;
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv == -1)
			return -1;
		off += rv;
	}
	return 0;
}

int do_write(int fd, void *buf, size_t count)
{
	int rv, off = 0;

retry:
	rv = write(fd, (char *)buf + off, count);
	if (rv == -1 && errno == EINTR)
		goto retry;
	/* If we cannot write _any_ data, we'd be in an (potential) loop. */
	if (rv <= 0) {
		log_error("write failed: %s (%d)", strerror(errno), errno);
		return rv;
	}

	if (rv != count) {
		count -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}


static void client_alloc(void)
{
	int i;

	if (!clients) {
		clients = malloc(CLIENT_NALLOC * sizeof(struct client));
		pollfds = malloc(CLIENT_NALLOC * sizeof(struct pollfd));
	} else {
		clients = realloc(clients, (client_size + CLIENT_NALLOC) *
					sizeof(struct client));
		pollfds = realloc(pollfds, (client_size + CLIENT_NALLOC) *
					sizeof(struct pollfd));
	}
	if (!clients || !pollfds) {
		log_error("can't alloc for client array");
		exit(1);
	}

	for (i = client_size; i < client_size + CLIENT_NALLOC; i++) {
		clients[i].workfn = NULL;
		clients[i].deadfn = NULL;
		clients[i].fd = -1;
		pollfds[i].fd = -1;
		pollfds[i].revents = 0;
	}
	client_size += CLIENT_NALLOC;
}

static void client_dead(int ci)
{
	if (clients[ci].fd != -1)
		close(clients[ci].fd);

	clients[ci].fd = -1;
	clients[ci].workfn = NULL;

	pollfds[ci].fd = -1;
}

int client_add(int fd, const struct booth_transport *tpt,
		void (*workfn)(int ci),
		void (*deadfn)(int ci))
{
	int i;
	struct client *c;


	if (client_size + 2 >= client_maxi ) {
		client_alloc();
	}

	for (i = 0; i < client_size; i++) {
		c = clients + i;
		if (c->fd != -1)
			continue;

		c->workfn = workfn;
		if (deadfn)
			c->deadfn = deadfn;
		else
			c->deadfn = client_dead;

		c->transport = tpt;
		c->fd = fd;

		pollfds[i].fd = fd;
		pollfds[i].events = POLLIN;
		if (i > client_maxi)
			client_maxi = i;

		return i;
	}

	assert(!"no client");
}


/* Only used for client requests, TCP ???*/
void process_connection(int ci)
{
	struct boothc_ticket_msg msg;
	int rv, len, exp, fd;
	void (*deadfn) (int ci);


	fd = clients[ci].fd;
	rv = do_read(fd, &msg.header, sizeof(msg.header));

	if (rv < 0) {
		if (errno == ECONNRESET)
			log_debug("client %d connection reset for fd %d",
					ci, clients[ci].fd);

		goto kill;
	}

	if (check_boothc_header(&msg.header, -1) < 0)
		goto kill;

	/* Basic sanity checks already done. */
	len = ntohl(msg.header.length);
	if (len) {
		if (len != sizeof(msg)) {
bad_len:
			log_error("got wrong length %u", len);
			return;
		}
		exp = len - sizeof(msg.header);
		rv = do_read(clients[ci].fd, msg.header.data, exp);
		if (rv < 0) {
			log_error("connection %d read data error %d, wanted %d",
					ci, rv, exp);
			goto kill;
		}
	}


	switch (ntohl(msg.header.cmd)) {
		case CMD_LIST:
			ticket_answer_list(fd, &msg);
			goto kill;

		case CMD_GRANT:
			/* Expect boothc_ticket_site_msg. */
			if (len != sizeof(msg))
				goto bad_len;
			ticket_answer_grant(fd, &msg);
			goto kill;

		case CMD_REVOKE:
			/* Expect boothc_ticket_site_msg. */
			if (len != sizeof(msg))
				goto bad_len;

			ticket_answer_revoke(fd, &msg);
			goto kill;

		default:
			log_error("connection %d cmd %x unknown",
					ci, ntohl(msg.header.cmd));
			init_header(&msg.header,CMR_GENERAL, RLT_INVALID_ARG, sizeof(msg.header));
			send_header_only(fd, &msg.header);
			goto kill;
	}

	assert(0);
	return;

kill:
	deadfn = clients[ci].deadfn;
	if(deadfn) {
		deadfn(ci);
	}
	return;
}


/** Callback function for the listening TCP socket. */
static void process_listener(int ci)
{
	int fd, i;

	fd = accept(clients[ci].fd, NULL, NULL);
	if (fd < 0) {
		log_error("process_listener: accept error for fd %d: %s (%d)",
			  clients[ci].fd, strerror(errno), errno);
		if (clients[ci].deadfn)
			clients[ci].deadfn(ci);
		return;
	}

	i = client_add(fd, clients[ci].transport, process_connection, NULL);

	log_debug("add client connection %d fd %d", i, fd);
}

static int setup_config(int type)
{
	int rv;

	rv = read_config(cl.configfile);
	if (rv < 0)
		goto out;

	/* Set "local" pointer, ignoring errors. */
	find_myself(NULL, type == CLIENT);


	rv = check_config(type);
	if (rv < 0)
		goto out;


	/* Per default the PID file name is derived from the
	 * configuration name. */
	if (!cl.lockfile[0]) {
		snprintf(cl.lockfile, sizeof(cl.lockfile)-1,
				"%s/%s.pid", BOOTH_RUN_DIR, booth_conf->name);
	}

out:
	return rv;
}

static int setup_transport(void)
{
	int rv;

	rv = transport()->init(message_recv);
	if (rv < 0) {
		log_error("failed to init booth_transport %s", transport()->name);
		goto out;
	}

	rv = booth_transport[TCP].init(NULL);
	if (rv < 0) {
		log_error("failed to init booth_transport[TCP]");
		goto out;
	}

out:
	return rv;
}


static int write_daemon_state(int fd, int state)
{
	char buffer[1024];
	int rv, size;

	size = sizeof(buffer) - 1;
	rv = snprintf(buffer, size,
			"booth_pid=%d "
			"booth_state=%s "
			"booth_type=%s "
			"booth_cfg_name='%s' "
			"booth_addr_string='%s' "
			"booth_port=%d\n",
		getpid(), 
		( state == BOOTHD_STARTED  ? "started"  : 
		  state == BOOTHD_STARTING ? "starting" : 
		  "invalid"), 
		type_to_string(local->type),
		booth_conf->name,
		local->addr_string,
		booth_conf->port);

	if (rv < 0 || rv == size) {
		log_error("Buffer filled up in write_daemon_state().");
		return -1;
	}
	size = rv;


	rv = ftruncate(fd, 0);
	if (rv < 0) {
		log_error("lockfile %s truncate error %d: %s",
				cl.lockfile, errno, strerror(errno));
		return rv;
	}


	rv = lseek(fd, 0, SEEK_SET);
	if (rv < 0) {
		log_error("lseek set fd(%d) offset to 0 error, return(%d), message(%s)",
			fd, rv, strerror(errno));
		rv = -1;
		return rv;
	} 


	rv = write(fd, buffer, size);

	if (rv != size) {
		log_error("write to fd(%d, %d) returned %d, errno %d, message(%s)",
                      fd, size,
		      rv, errno, strerror(errno));
		return -1;
	}

	return 0;
}

static int loop(int fd)
{
	void (*workfn) (int ci);
	void (*deadfn) (int ci);
	int rv, i;

	rv = setup_transport();
	if (rv < 0)
		goto fail;

	rv = setup_ticket();
	if (rv < 0)
		goto fail;


	client_add(local->tcp_fd, booth_transport + TCP,
			process_listener, NULL);


	rv = write_daemon_state(fd, BOOTHD_STARTED);
	if (rv != 0) {
		log_error("write daemon state %d to lockfile error %s: %s",
                      BOOTHD_STARTED, cl.lockfile, strerror(errno));
		goto fail;
	}

	if (cl.type == ARBITRATOR)
		log_info("BOOTH arbitrator daemon started");
	else if (cl.type == SITE)
		log_info("BOOTH cluster site daemon started");

	while (1) {
		rv = poll(pollfds, client_maxi + 1, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0) {
			log_error("poll failed: %s (%d)", strerror(errno), errno);
			goto fail;
		}

		for (i = 0; i <= client_maxi; i++) {
			if (clients[i].fd < 0)
				continue;

			if (pollfds[i].revents & POLLIN) {
				workfn = clients[i].workfn;
				if (workfn)
					workfn(i);
			}
			if (pollfds[i].revents &
					(POLLERR | POLLHUP | POLLNVAL)) {
				deadfn = clients[i].deadfn;
				if (deadfn)
					deadfn(i);
			}
		}

		process_tickets();
	}

	return 0;

fail:
	return -1;
}


static int query_get_string_answer(cmd_request_t cmd)
{
	struct booth_site *site;
	struct boothc_header reply;
	char *data;
	int data_len;
	int rv;
	struct booth_transport const *tpt;

	data = NULL;
	init_header(&cl.msg.header, cmd, 0, sizeof(cl.msg));

	if (!*cl.site)
		site = local;
	else if (!find_site_by_name(cl.site, &site)) {
		log_error("cannot find site \"%s\"", cl.site);
		rv = ENOENT;
		goto out;
	}

	tpt = booth_transport + TCP;
	rv = tpt->open(site);
	if (rv < 0)
		goto out_free;

	rv = tpt->send(site, &cl.msg, sizeof(cl.msg));
	if (rv < 0)
		goto out_free;

	rv = tpt->recv(site, &reply, sizeof(reply));
	if (rv < 0)
		goto out_free;

	data_len = ntohl(reply.length) - sizeof(reply);

	data = malloc(data_len);
	if (!data) {
		rv = -ENOMEM;
		goto out_free;
	}
	rv = tpt->recv(site, data, data_len);
	if (rv < 0)
		goto out_free;

	do_write(STDOUT_FILENO, data, data_len);
	rv = 0;

out_free:
	free(data);
	tpt->close(site);
out:
	return rv;
}


static int do_command(cmd_request_t cmd)
{
	struct booth_site *site;
	struct boothc_header reply;
	struct booth_transport const *tpt;
	int rv;

	rv = 0;
	site = NULL;

	if (!*cl.site)
		site = local;
	else if (!find_site_by_name(cl.site, &site)) {
		log_error("Site \"%s\" not configured.", cl.site);
		goto out_close;
	}

	/* We don't check for existence of ticket, so that asking can be
	 * done without local configuration, too.
	 * Although, that means that the UDP port has to be specified, too. */
	if (!cl.msg.ticket.id[0]) {
		/* If the loaded configuration has only a single ticket defined, use that. */
		if (booth_conf->ticket_count == 1) {
			strcpy(cl.msg.ticket.id, booth_conf->ticket[0].name);
		} else {
			log_error("No ticket given.");
			goto out_close;
		}
	}

	init_header(&cl.msg.header, cmd, 0, sizeof(cl.msg));

	/* Always use TCP for client - at least for now. */
	tpt = booth_transport + TCP;
	rv = tpt->open(site);
	if (rv < 0)
		goto out_close;

	rv = tpt->send(site, &cl.msg, sizeof(cl.msg));
	if (rv < 0)
		goto out_close;


	rv = tpt->recv(site, &reply, sizeof(reply));
	if (rv < 0)
		goto out_close;

	if (reply.result == RLT_INVALID_ARG) {
		log_info("invalid argument!");
		rv = -1;
		goto out_close;
	}

	if (reply.result == RLT_OVERGRANT) {
		log_info("You're granting a granted ticket "
			 "If you wanted to migrate a ticket,"
			 "use revoke first, then use grant");
		rv = -1;
		goto out_close;
	}


	rv = ntohl(reply.result);
	switch (rv) {
	case RLT_ASYNC:
		if (cmd == CMD_GRANT)
			log_info("grant command sent, result will be returned "
				 "asynchronously, you can get the result from "
				 "the log files");
		else if (cmd == CMD_REVOKE)
			log_info("revoke command sent, result will be returned "
				 "asynchronously, you can get the result from "
				 "the log files.");
		else
			log_error("internal error reading reply result!");
		rv = 0;
		break;

	case RLT_SYNC_SUCC:
	case RLT_SUCCESS:
		if (cmd == CMD_GRANT)
			log_info("grant succeeded!");
		else if (cmd == CMD_REVOKE)
			log_info("revoke succeeded!");
		rv = 0;
		break;

	case RLT_SYNC_FAIL:
		if (cmd == CMD_GRANT)
			log_info("grant failed!");
		else if (cmd == CMD_REVOKE)
			log_info("revoke failed!");
		rv = -1;
		break;

	case RLT_INVALID_ARG:
		log_error("\"Invalid argument\", most probably ticket name \"%s\" wrong.",
				cl.msg.ticket.id);
		break;

	default:
		log_error("got an error code: %x", rv);
		rv = -1;
	}


out_close:
	if (site)
		local_transport->close(site);
	return rv;
}

static int do_grant(void)
{
	return do_command(CMD_GRANT);
}

static int do_revoke(void)
{
	return do_command(CMD_REVOKE);
}



static int _lockfile(int mode, int *fdp, pid_t *locked_by)
{
	struct flock lock;
	int fd, rv;


	/* After reboot the directory may not yet exist.
	 * Try to create it, but ignore errors. */
	if (strncmp(cl.lockfile, BOOTH_RUN_DIR,
				strlen(BOOTH_RUN_DIR)) == 0)
		mkdir(BOOTH_RUN_DIR, 0775);


	if (locked_by)
		*locked_by = 0;

	*fdp = -1;
	fd = open(cl.lockfile, mode, 0664);
	if (fd < 0)
		return errno;

	*fdp = fd;

	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	lock.l_pid = 0;


	if (fcntl(fd, F_SETLK, &lock) == 0)
		return 0;

	rv = errno;

	if (locked_by)
		if (fcntl(fd, F_GETLK, &lock) == 0)
			*locked_by = lock.l_pid;

	return rv;
}


static int lockfile(void) {
	int rv, fd;

	fd = -1;
	rv = _lockfile(O_CREAT | O_WRONLY, &fd, NULL);

	if (fd == -1) {
		log_error("lockfile %s open error %d: %s",
				cl.lockfile, rv, strerror(rv));
		return -1;
	}       

	if (rv < 0) {
		log_error("lockfile %s setlk error %d: %s",
				cl.lockfile, rv, strerror(rv));
		goto fail;
	}

	rv = write_daemon_state(fd, BOOTHD_STARTING);
	if (rv != 0) {
		log_error("write daemon state %d to lockfile error %s: %s",
				BOOTHD_STARTING, cl.lockfile, strerror(errno));
		goto fail;
	}

	return fd;

fail:
	close(fd);
	return -1;
}

static void unlink_lockfile(int fd)
{
	unlink(cl.lockfile);
	close(fd);
}

static void print_usage(void)
{
	printf("Usage:\n");
	printf("booth <type> <operation> [options]\n");
	printf("\n");
	printf("Types:\n");
	printf("  arbitrator:   daemon running on arbitrator\n");
	printf("  site:	        daemon running on cluster site\n");
	printf("  client:       command running from client\n");
	printf("\n");
	printf("Operations:\n");
	printf("Please note that operations are valid iff type is client!\n");
	printf("  list:	        List all the tickets\n");
	printf("  grant:        Grant ticket T(-t T) to site S(-s S)\n");
	printf("  revoke:       Revoke ticket T(-t T) from site S(-s S)\n");
	printf("\n");
	printf("Options:\n");
	printf("  -c FILE       Specify config file [default " BOOTH_DEFAULT_CONF "]\n");
	printf("  -l LOCKFILE   Specify lock file path\n");
	printf("  -D            Enable debugging to stderr and don't fork\n");
	printf("  -t            ticket name\n");
	printf("  -S            report local daemon status (for site and arbitrator)\n");
	printf("                RA script compliant return codes.\n");
	printf("  -s            site name\n");
	printf("  -h            Print this help, then exit\n");
}

#define OPTION_STRING		"c:Dl:t:s:hS"


void safe_copy(char *dest, char *value, size_t buflen, const char *description) {
	int content_len = buflen - 1;

	if (strlen(value) >= content_len) {
		fprintf(stderr, "'%s' exceeds maximum %s length of %d\n",
			value, description, content_len);
		exit(EXIT_FAILURE);
	}
	strncpy(dest, value, content_len);
	dest[content_len] = 0;
}

static int host_convert(char *hostname, char *ip_str, size_t ip_size)
{
	struct addrinfo *result = NULL, hints = {0};
	int re = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = BOOTH_PROTO_FAMILY;
	hints.ai_socktype = SOCK_DGRAM;

	re = getaddrinfo(hostname, NULL, &hints, &result);

	if (re == 0) {
		struct in_addr addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
		const char *re_ntop = inet_ntop(BOOTH_PROTO_FAMILY, &addr, ip_str, ip_size);
		if (re_ntop == NULL) {
			re = -1;
		}
	}

	freeaddrinfo(result);
	return re;
}

static int read_arguments(int argc, char **argv)
{
	int optchar;
	char *arg1 = argv[1];
	char *op = NULL;
	char *cp;
	char site_arg[INET_ADDRSTRLEN] = {0};

	if (argc < 2 || !strcmp(arg1, "help") || !strcmp(arg1, "--help") ||
		!strcmp(arg1, "-h")) {
		print_usage();
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "version") || !strcmp(arg1, "--version") ||
		!strcmp(arg1, "-V")) {
		printf("%s %s (built %s %s)\n",
			argv[0], RELEASE_VERSION, __DATE__, __TIME__);
		exit(EXIT_SUCCESS);
	}

	if (strcmp(arg1, "arbitrator") == 0 ||
			strcmp(arg1, "site") == 0 ||
			strcmp(arg1, "start") == 0 ||
			strcmp(arg1, "daemon") == 0) {
		cl.type = DAEMON;
		optind = 2;
	} else if (strcmp(arg1, "status") == 0) {
		cl.type = STATUS;
		optind = 2;
	} else if (strcmp(arg1, "client") == 0) {
		cl.type = CLIENT;
		if (argc < 3) {
			print_usage();
			exit(EXIT_FAILURE);
		}
		op = argv[2];
		optind = 3;
	} else {
		cl.type = CLIENT;
		op = argv[1];
		optind = 2;
	}

	switch (cl.type) {
	case ARBITRATOR:
		break;

	case SITE:
		break;

	case CLIENT:
		if (!strcmp(op, "list"))
			cl.op = OP_LIST;
		else if (!strcmp(op, "grant"))
			cl.op = OP_GRANT;
		else if (!strcmp(op, "revoke"))
			cl.op = OP_REVOKE;
		else {
			fprintf(stderr, "client operation \"%s\" is unknown\n",
				op);
			exit(EXIT_FAILURE);
		}
		break;
	}

	while (optind < argc) {
		optchar = getopt(argc, argv, OPTION_STRING);

		switch (optchar) {
		case 'c':
			if (strchr(optarg, '/')) {
				safe_copy(cl.configfile, optarg,
						sizeof(cl.configfile), "config file");
			} else {
				/* If no "/" in there, use with default directory. */
				strcpy(cl.configfile, BOOTH_DEFAULT_CONF_DIR);
				cp = cl.configfile + strlen(BOOTH_DEFAULT_CONF_DIR);
				assert(cp > cl.configfile);
				assert(*(cp-1) == '/');

				/* Write after the "/" */
				safe_copy(cp + 1, optarg,
						(sizeof(cl.configfile) -
						 (cp -  cl.configfile) -
						 strlen(BOOTH_DEFAULT_CONF_EXT)),
						"config name");

				/* If no extension, append ".conf".
				 * Space is available, see -strlen() above. */
				if (!strchr(cp, '.'))
					strcat(cp, BOOTH_DEFAULT_CONF_EXT);
			}
			break;
		case 'D':
			daemonize = 1;
			debug_level++;
			break;

		case 'l':
			safe_copy(cl.lockfile, optarg, sizeof(cl.lockfile), "lock file");
			break;
		case 't':
			if (cl.op == OP_GRANT || cl.op == OP_REVOKE) {
				safe_copy(cl.msg.ticket.id, optarg, sizeof(cl.msg.ticket.id), "ticket name");
			} else {
				print_usage();
				exit(EXIT_FAILURE);
			}
			break;

		case 's':
			if (cl.op == OP_GRANT || cl.op == OP_REVOKE || cl.op == OP_LIST) {
				int re = host_convert(optarg, site_arg, INET_ADDRSTRLEN);
				if (re == 0) {
					safe_copy(cl.site, site_arg, sizeof(cl.site), "site name");
				} else {
					safe_copy(cl.site, optarg, sizeof(cl.site), "site name");
				}
			} else {
				print_usage();
				exit(EXIT_FAILURE);
			}
			break;

		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);
			break;

		case ':':
		case '?':
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(EXIT_FAILURE);
			break;
		
		default:
			fprintf(stderr, "unknown option: %s\n", argv[optind]);
			exit(EXIT_FAILURE);
			break;
		};
	}

	return 0;
}

static void set_scheduler(void)
{
	struct sched_param sched_param;
	struct rlimit rlimit;
	int rv;

	rlimit.rlim_cur = RLIM_INFINITY;
	rlimit.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_MEMLOCK, &rlimit);
	rv = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (rv < 0) {
		log_error("mlockall failed");
	}

	rv = sched_get_priority_max(SCHED_RR);
	if (rv != -1) {
		sched_param.sched_priority = rv;
		rv = sched_setscheduler(0, SCHED_RR, &sched_param);
		if (rv == -1)
			log_error("could not set SCHED_RR priority %d: %s (%d)",
					sched_param.sched_priority,
					strerror(errno), errno);
	} else {
		log_error("could not get maximum scheduler priority err %d",
				errno);
	}
}

static void set_oom_adj(int val)
{
        FILE *fp;

        fp = fopen("/proc/self/oom_adj", "w");
        if (!fp)
                return;

        fprintf(fp, "%i", val);
        fclose(fp);
}

static int do_status(int type)
{
	pid_t pid;
	int rv, lock_fd, ret;
	const char *reason = NULL;
	char lockfile_data[1024], *cp;


	ret = PCMK_OCF_NOT_RUNNING;
	/* TODO: query all, and return quit only if it's _cleanly_ not
	 * running, ie. _neither_ of port/lockfile/process is available?
	 *
	 * Currently a single failure says "not running", even if "only" the
	 * lockfile has been removed. */

	rv = setup_config(type);
	if (rv) {
		reason = "Error reading configuration.";
		ret = PCMK_OCF_UNKNOWN_ERROR;
		goto quit;
	}


	if (!local) {
		reason = "No Service IP active here.";
		goto quit;
	}


	rv = _lockfile(O_RDWR, &lock_fd, &pid);
	if (rv == 0) {
		reason = "PID file not locked.";
		goto quit;
	}
	if (lock_fd == -1) {
		reason = "No PID file.";
		goto quit;
	}

	if (pid) {
		fprintf(stdout, "booth_lockpid=%d ", pid);
		fflush(stdout);
	}


	rv = read(lock_fd, lockfile_data, sizeof(lockfile_data) - 1);
	if (rv < 4) {
		reason = "Cannot read lockfile data.";
		ret = PCMK_LSB_UNKNOWN_ERROR;
		goto quit;

	}
	lockfile_data[rv] = 0;

	if (lock_fd != -1)
		close(lock_fd);


	/* Make sure it's only a single line */
	cp = strchr(lockfile_data, '\r');
	if (cp)
		*cp = 0;
	cp = strchr(lockfile_data, '\n');
	if (cp)
		*cp = 0;



	rv = setup_udp_server(1);
	if (rv == 0) {
		reason = "UDP port not in use.";
		goto quit;
	}


	fprintf(stdout, "booth_lockfile='%s' %s\n",
			cl.lockfile, lockfile_data);
	if (daemonize)
		fprintf(stderr, "Booth at %s port %d seems to be running.\n",
				local->addr_string, booth_conf->port);
	return 0;


quit:
	log_debug("not running: %s", reason);
	/* Ie. "DEBUG" */
	if (daemonize)
		fprintf(stderr, "not running: %s\n", reason);
	return ret;
}


static int do_server(int type)
{
	int lock_fd = -1;
	int rv = -1;
	static char log_ent[128] = DAEMON_NAME "-";

	rv = setup_config(type);
	if (rv < 0)
		goto out;


	if (!local) {
		log_error("Cannot find myself in the configuration.");
		exit(EXIT_FAILURE);
	}

	if (!daemonize) {
		if (daemon(0, 0) < 0) {
			perror("daemon error");
			exit(EXIT_FAILURE);
		}
	}

	/* The lockfile must be written to _after_ the call to daemon(), so
	 * that the lockfile contains the pid of the daemon, not the parent. */
	lock_fd = lockfile();
	if (lock_fd < 0)
		return lock_fd;

	strcat(log_ent, type_to_string(local->type));
	cl_log_set_entity(log_ent);
	cl_log_enable_stderr(debug_level ? TRUE : FALSE);
	cl_log_set_facility(HA_LOG_FACILITY);
	cl_inherit_logging_environment(0);


	log_info("BOOTH %s daemon is starting, node id is %08X.",
			type_to_string(local->type),
			local->site_id);

	signal(SIGUSR1, (__sighandler_t)tickets_log_info);

	set_scheduler();
	set_oom_adj(-16);
	set_proc_title("%s %s for [%s]:%d",
			DAEMON_NAME,
			type_to_string(local->type),
			local->addr_string,
			booth_conf->port);

	rv = loop(lock_fd);

out:
	if (lock_fd >= 0)
		unlink_lockfile(lock_fd);

	return rv;
}

static int do_client(void)
{
	int rv = -1;

	rv = setup_config(CLIENT);
	if (rv < 0) {
		log_error("cannot read config");
		goto out;
	}

	switch (cl.op) {
	case OP_LIST:
		rv = query_get_string_answer(CMD_LIST);
		break;

	case OP_GRANT:
		rv = do_grant();
		break;

	case OP_REVOKE:
		rv = do_revoke();
		break;
	}

out:
	return rv;
}

int main(int argc, char *argv[], char *envp[])
{
	int rv;

	init_set_proc_title(argc, argv, envp);

	memset(&cl, 0, sizeof(cl));
	strncpy(cl.configfile,
			BOOTH_DEFAULT_CONF, BOOTH_PATH_LEN - 1);
	cl.lockfile[0] = 0;
	debug_level = 0;
	cl_log_set_entity("booth");
	cl_log_enable_stderr(TRUE);
	cl_log_set_facility(0);


	rv = read_arguments(argc, argv);
	if (rv < 0)
		goto out;


	switch (cl.type) {
	case STATUS:
		rv = do_status(cl.type);
		break;

	case ARBITRATOR:
	case DAEMON:
	case SITE:
		rv = do_server(cl.type);
		break;

	case CLIENT:
		rv = do_client();
		break;
	}

out:
	/* Normalize values. 0x100 would be seen as "OK" by waitpid(). */
	return (rv >= 0 && rv < 0x70) ? rv : 1;
}
