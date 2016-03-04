/* (c) 2015 Michael R. Tirado -- GPLv3, GNU General Public License, version 3.
 * contact: mtirado418@gmail.com
 *
 * TODO !!!  config file:
 * for setting user-related parameters, max hosts, owning hostnames, etc
 * could also use the file for expressing which users are allowed to connect
 * to a service as well as specifying host timeout value if watchdog is
 * needed. which uid/user to switch to after initialization of operator, etc.
 */


#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <syslog.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <malloc.h>

#include "eslib/eslib.h"

/* some reasonable limits */
#define UPDATE_FREQ 12   /* 12 frames per second(ish)*/
#define MAXACCEPT   100  /* connections to accept per frame */
#define MAXREG_HSHK 25   /* pending registrations, consumes 1 fd */
#define MAXREQ_HSHK 25   /* connection request handshakes, consume 1 fd */
#define MAXHOSTS    150  /* consume 2 fds make sure we're within ulimit -Sn */
#define MAXHOSTSPERUSER 5/* default hosts per user, root is unlimited */

/* milliseconds */
#define OP_REG_TIMEOUT 5000
#define OP_REQ_TIMEOUT 5000


/* struct is shared between register and request protocols */
struct handshake
{
	int active;
	struct ucred creds;
	struct timeval timestamp;
	int socket;
	int visibility; /* (registration only) */
	pid_t pid; /* 0 if inactive (request only) */
};


/*
 * operators version of ophost.h struct
 * which represents a host that callers will request connections to
 */
struct _ophost
{
	char name[OPHOST_MAXNAME];
	struct _ophost *next; /* linked list */
	int socket;	/* main line to host (send requests here) */
	int relay;	/* relays new connections back to caller */
	uid_t uid;

	/* last confirmation ping, 0's if not ready */
	struct timeval time_created;
	struct timeval last_ack;
};


/*
 * global operator data
 */
struct system_operator
{
	struct handshake registr[MAXREG_HSHK];  /* host registrations */
	struct handshake requests[MAXREQ_HSHK]; /* connection requests */
	struct _ophost *hosts; /* registered hosts */
	unsigned int numhosts;

	/* sockets */
	int registration; /* register a new host */
	int request;	  /* request connection to host */
};
struct system_operator  g_operator;


static int  operator_update_requests();
static int  operator_update_regconnect();
static int  operator_update_registration();
static void operator_update_hosts();
static int init();


char g_errbuf[ESLIB_LOG_MAXMSG];

static void operator_signal_handler(int signum)
{
	printf("received signal(%d): %s\n", signum, strsignal(signum));
	if (signum == SIGTERM) {
		/* TODO notify all hosts of incoming termination of service.*/
		usleep(1000000);
		kill(getpid(), SIGKILL);
	}
}
static void operator_signal_setup()
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, operator_signal_handler);
	signal(SIGINT,  operator_signal_handler);
	signal(SIGQUIT, operator_signal_handler);
	signal(SIGSTOP, operator_signal_handler);
	signal(SIGTSTP, operator_signal_handler);
	signal(SIGTRAP, operator_signal_handler);
}

int main()
{
	if (init()) {
		printf("initialization error\n");
		return -1;
	}

	while(1)
	{
		operator_update_regconnect();
		operator_update_registration();
		operator_update_hosts();
		operator_update_requests();
		usleep(999999/UPDATE_FREQ);
	}
	return -1;
}


static int init()
{
	int i;

	/* ignore sigpipe, and friends.. */
	operator_signal_setup();

	/* global operator instance */
	memset(&g_operator, 0, sizeof(g_operator));
	for (i = 0; i < MAXREG_HSHK; ++i)
		g_operator.registr[i].socket = -1;

	/* create registration socket */
	g_operator.registration = eslib_sock_create_passive(OP_REG_PATH,
							    OPHOST_MAXACCEPT);
	if (g_operator.registration == -1)
		return -1;

	/* create requests socket */
	g_operator.request = eslib_sock_create_passive(OP_REQ_PATH,
						       OPHOST_MAXACCEPT);
	if (g_operator.request == -1)
		return -1;

	return 0;
}



/*
 * return a new connection
 */
static int operator_accept_connection(int sock, int nonblock)
{
	int newsock;
	struct sockaddr_un addr;
	socklen_t addrlen = sizeof(addr);

	memset(&addr, 0, sizeof(addr));
	newsock = accept(sock, (struct sockaddr *)&addr, &addrlen);
	if (newsock == -1) {
		if (errno != EAGAIN && errno != EINTR) {
			printf("operator - accept_connection error: %s",
					strerror(errno));
		}
		return -1;
	}


	if (nonblock) {
		if (eslib_sock_setnonblock(newsock)) {
			printf("socket: could not set as nonblocking\n");
			eslib_sock_axe(newsock);
			return -1;
		}
	}

	return newsock;
}


/* return number of handshakes with uid */
static int handshake_count_uid(struct handshake *hshk,
			       unsigned int size, uid_t uid)
{
	unsigned int i;
	int count = 0;
	for (i = 0; i < size; ++i) {
		if (hshk[i].active) {
			if (hshk[i].creds.uid == uid)
				++count;
		}
	}
	return count;
}


/* return number of hosts with uid */
static int hosts_count_uid(struct _ophost *hosts, uid_t uid)
{
	struct _ophost *host = hosts;
	int count = 0;
	while (host)
	{
		if (host->uid == uid)
			++count;
		host = host->next;
	}
	return count;
}


/*
 *  listen for new connections on registration socket.
 *  create a new host registration handshake
 */
static int operator_update_regconnect()
{
	int i, p;
	int sock;
	struct ucred creds;
	socklen_t len = sizeof(struct ucred);

	/* check for new connections to be handled next frame */
	for (i = 0; i < MAXACCEPT; ++i)
	{
		sock = operator_accept_connection(g_operator.registration, 1);
		if (sock == -1)
			break;

		/* peercred gets credentials at time of connect call */
		if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &creds, &len)){
			printf("getsockopt: %s\n", strerror(errno));
			eslib_sock_axe(sock);
			return -1;
		}

		/* bottleneck registration attempts per uid */
		if (handshake_count_uid(g_operator.registr,
					MAXREG_HSHK, creds.uid) > 5) {
			eslib_sock_axe(sock);
			return -1;
		}

		/*
		 * nonroot uid is limited
		 * TODO read limits from operator config file
		 */
		if (creds.uid != 0) {
			if (hosts_count_uid(g_operator.hosts, creds.uid)
					>= MAXHOSTSPERUSER) {
				printf("uid(%d) at host limit\n", creds.uid);
				eslib_sock_axe(sock);
				return -1;
			}
		}
		/* find a free slot */
		for (p = 0; p < MAXREG_HSHK;) {
			if (!g_operator.registr[++p].active)
				break;
		}
		if (p >= MAXREG_HSHK)
			eslib_sock_axe(sock);
		else {
			/* create pending registration */
			memset(&g_operator.registr[p],
					0, sizeof(struct handshake));
			g_operator.registr[p].socket = sock;
			gettimeofday(&g_operator.registr[p].timestamp, NULL);
			memcpy(&g_operator.registr[p].creds,
					&creds, sizeof(creds));
			g_operator.registr[p].active = 1;
		}
	}
	return 0;
}


/* register protocol:
 *
 * pending host sends registration message:
 * 	the desired host name(null terminated string)
 *
 * server acks by sending the relay socket for new connections
 * new host is added to the front of list.
 *
 * the new host will not be sent connection requests until it
 * has sent operator at least one ack, 'K'.
 */
static int operator_update_registration()
{
	char msg[OPHOST_MAXNAME];
	char *nameptr;
	struct _ophost *host;
	struct handshake *pending;
	struct timeval tmr;
	int relay[2]; /* AF_UNIX socket pair */
	int retval;
	int len;
	unsigned int p;

	gettimeofday(&tmr, NULL);

	for (p = 0; p < MAXREG_HSHK; ++p) {

		pending = &g_operator.registr[p];
		if (!pending->active)
			continue;

		/* check expiration */
		if (eslib_ms_elapsed(tmr, pending->timestamp,
					    OP_REG_TIMEOUT)) {
			printf("pending connection expired, dropping...\n");
			goto drop_pending;
		}

		if (g_operator.numhosts >= MAXHOSTS)
			continue;

		/* receive host name */
		retval = recv(pending->socket, msg, sizeof(msg), MSG_DONTWAIT);
		if (retval == -1 && (errno == EAGAIN || errno == EINTR)) {
			continue; /* no data to recv */
		}
		else if (retval == 0 || retval == -1) {
			printf("socket error\n");
			goto drop_pending;
		}

		/* validate hostname */
		if (retval <= 1 || msg[0] == '\0' || msg[retval-1] != '\0') {
			static time_t t = 0;
			eslib_logerror_t("operator",
					 "erroneous hostname", &t, 10);
			goto drop_pending;
		}


		/* find host */
		host = g_operator.hosts;
		nameptr = msg;
		len = strnlen(nameptr, OPHOST_MAXNAME);
		if (len >= OPHOST_MAXNAME)
			goto drop_pending;

		for (; host; host = host->next)
			if (strncmp(host->name,	nameptr, len) == 0)
				goto drop_pending; /* host name in use */

		/* name is available */
		host = malloc(sizeof(*host));
		if (host == NULL)
			goto drop_pending;

		memset(host, 0, sizeof(*host));
		strncpy(host->name, nameptr, len);

		/* ack: create and send relay socket */
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, relay))
			goto free_and_drop;

		if (eslib_sock_send_fd(pending->socket, relay[1])) {
			printf("error sending relay fd\n");
			close(relay[0]);
			close(relay[1]);
			goto free_and_drop;
		}
		close(relay[1]); /* don't need this half */


		/* add to front of list */
		gettimeofday(&host->time_created, NULL); /* setup timestamps */
		memcpy(&host->last_ack, &host->time_created,
					sizeof(host->last_ack));
		host->socket = pending->socket;
		host->relay = relay[0];
		host->next = g_operator.hosts;
		host->uid = pending->creds.uid;
		g_operator.hosts = host;
		++g_operator.numhosts;
		/* reset pending slot */
		memset(pending, 0, sizeof(*pending));
		pending->socket = -1;
		continue;
free_and_drop:
		free(host);
drop_pending:
		eslib_sock_axe(pending->socket);
		memset(pending, 0, sizeof(*pending));
		pending->socket = -1;
	}

	return 0;
}

/* find host by name */
static struct _ophost *host_lookup(char *name)
{
	struct _ophost *host = g_operator.hosts;
	while (host)
	{
		if (strncmp(name, host->name, OPHOST_MAXNAME) == 0)
			return host;
		host = host->next;
	}
	return NULL;
}



/*          (new thread)
 * caller<--><operator><-->host request handshake thread.
 * relays AF_UNIX connection fd from host back to caller.
 *
 */
static int req_handshake_process(struct handshake *hshk)
{
	char msg[OPHOST_MAXNAME];
	struct _ophost *host = NULL;
	const char req  = 'R';
	int retval;
	int fd;
	struct timeval tmr;

	/*
	 * connection request protocol:
	 *	wait for caller to send hostname
	 *	send host a connection request message
	 *	receive new connection fd from host
	 *	relay new connection fd back to caller
	 * exit thread.
	 *
	 * host and caller can do their own mystical handshake if they
	 * are so inclined. operator only cares about introducing them.
	 */


	/*
	 * get hostname
	 * make sure we received more than a null terminator & 0 is disconnect
	 */
	retval = recv(hshk->socket, msg, sizeof(msg), 0);
	if (retval <= 1) {
		printf("handshake recv(%d): %s\n", retval, strerror(errno));
		goto eject;
	}
	/* validate string */
	if (msg[0] == '\0' || msg[retval-1] != '\0') {
		static time_t t = 0;
		eslib_logerror_t("operator","invalid handshake message",&t,10);
		goto eject;
	}
	host = host_lookup(msg);

	/* receive name from caller */
	if (host == NULL) { /* TODO remove special characters? */
		printf("handshake: host \"%s\" not found\n", msg);
		goto eject;
	}

	/* make sure host has been confirmed before sending request
	 * any difference in times, means we have received a valid ack
	 */
	if (host->last_ack.tv_sec == host->time_created.tv_sec
			&& host->last_ack.tv_usec
			== host->time_created.tv_usec) {
		printf("host not confirmed yet\n");
		goto eject;
	}

	/* send host a request for connected socket */
	if (send(host->socket, &req, 1, 0) != 1) {
		printf("send req failed\n");
		goto eject;
	}


	while (1)
	{
		usleep(50000);/* 50ms(ish) */
		gettimeofday(&tmr, NULL);
		if (eslib_ms_elapsed(tmr, hshk->timestamp,
					    OP_REQ_TIMEOUT)) {
			printf("registration handshake timeout\n");
			goto eject;
		}

		/* relay fd back to caller */
		/* wait for host to send new AF_UNIX socket */
		retval = eslib_sock_recv_fd(host->relay, &fd);
		if (retval == -1 && errno == EAGAIN)
			continue; /* back to timer check */
		else if (retval) {
			goto eject;
		}

		/* relay back to caller, and we're outta here. */
		if(eslib_sock_send_fd(hshk->socket, fd)) {
			printf("[operator] -- send_fd hshk->socket failed\n");
			close(fd);
			goto eject;
		}
		break;
	}
	close(fd);
	_exit(0);

eject:
	_exit(-1);
}


/*
 * move caller over to a new thread which will complete the handshake,
 * host will connect to itself, and send back that socket.
 */
static int req_handshake_create(int caller)
{
	unsigned int idx;
	pid_t pid;
	struct ucred creds;
	socklen_t len = sizeof(struct ucred);

	/* find free slot */
	for (idx = 0 ; idx < MAXREQ_HSHK; ++idx) {
		if (!g_operator.requests[idx].active)
			break;
	}
	if (idx >= MAXREQ_HSHK) {
		eslib_sock_axe(caller);
		return -1; /* no free slots */
	}

	/* peercred gets credentials at time of connect call */
	if (getsockopt(caller, SOL_SOCKET, SO_PEERCRED, &creds, &len)){
		printf("getsockopt: %s\n", strerror(errno));
		eslib_sock_axe(caller);
		return -1;
	}

	/* bottleneck connection attempts per uid */
	if (handshake_count_uid(g_operator.requests,
				MAXREQ_HSHK, creds.uid) != 0) {
		eslib_sock_axe(caller);
		return -1;
	}

	memset(&g_operator.requests[idx], 0, sizeof(struct handshake));
	g_operator.requests[idx].active = 1;
	g_operator.requests[idx].socket = caller;
	gettimeofday(&g_operator.requests[idx].timestamp, NULL);
	memcpy(&g_operator.requests[idx].creds, &creds, sizeof(creds));

	pid = fork();
	if (pid == 0) { /* enter handshake loop */
		req_handshake_process(&g_operator.requests[idx]);
	}
	else if (pid != -1) /* save pid so we can id process when it returns */
		g_operator.requests[idx].pid = pid;
	else /* error, clear slot */
		memset(&g_operator.requests[idx], 0, sizeof(struct handshake));

	close(caller);
	g_operator.requests[idx].socket = -1;

	if (pid == -1)
		return -1;
	else
		return 0;
}


/*
 * requests are a remote caller trying to look up a registered host.
 * the handshake function happens in it's own thread.
 * operator will remove handshakes that idle for too long,
 * and collect pid's for successful handshakes.
 */
static int operator_update_requests()
{
	struct timeval tmr;
	unsigned int i, h;
	int sock;
	int status;
	pid_t retpid;
	pid_t wpid = -1;

	gettimeofday(&tmr, NULL);

	/* check handshake status */
	for (i = 0; i < MAXREQ_HSHK; ++i)
	{
		if (!g_operator.requests[i].active)
			continue;

		/* check expired handshakes */
		if (eslib_ms_elapsed(tmr, g_operator.requests[i].timestamp,
					OP_REQ_TIMEOUT)) {
			printf("handshake timeout pid: %d\n",
					g_operator.requests[i].pid);
			/* don't ever try to kill init */
			if (g_operator.requests[i].pid > 1)
				kill(g_operator.requests[i].pid, SIGKILL);
			retpid = g_operator.requests[i].pid;
			goto clear_slot;
		}

		/* collect successful handshakes */
		retpid = waitpid(wpid, &status, WNOHANG);
		if (retpid == 0) {
			continue;
		}
		else if (retpid < 0) {
			static time_t t = 0;
			eslib_logerror_t("operator", "request pid error",
					&t, 2);
			continue;
		}
clear_slot:
		/* free up the handshake slot in global array */
		for (h = 0; h < MAXREQ_HSHK; ++h) {
			if (g_operator.requests[h].pid == retpid) {
				memset(&g_operator.requests[h], 0,
						sizeof(struct handshake));
				break;
			}
		} /* was not found, should never happen */
		if (h >= MAXREQ_HSHK) {
			snprintf(g_errbuf, sizeof(g_errbuf),
					"handshake pid %d not found", retpid);
			eslib_logcritical("operator", g_errbuf);
			memset(g_operator.requests, 0,	sizeof(g_operator.requests));
		}
	}

	/* check for new connections to be handled next frame */
	for (i = 0; i < MAXACCEPT; ++i) {
		sock = operator_accept_connection(g_operator.request, 0);
		if (sock == -1)
			break;
		/* connection has been established */
		if (req_handshake_create(sock))
			continue;
	}

	return 0;
}


/* remove current, and return prev */
static struct _ophost *remove_host(struct _ophost *prev,
				   struct _ophost *current)
{
	if (!current) {
		eslib_logcritical("operator", "remove_host current==NULL");
		return NULL;
	}
	if (prev)
		prev->next = current->next;
	else
		g_operator.hosts = current->next;

	eslib_sock_axe(current->socket);
	eslib_sock_axe(current->relay);
	free(current);
	if (g_operator.numhosts)
		--g_operator.numhosts;
	if (prev)
		return prev;
	else
		return g_operator.hosts;
}


/* check for host pings, disconnects, and TODO handle host timeout */
static void operator_update_hosts()
{
	struct _ophost *host, *prev;
	struct timeval tmr;
	int retval;
	char buf;

	prev = NULL;
	host = g_operator.hosts;
	while (host)
	{
		/* check connection status */
		retval = recv(host->socket, &buf, 1, MSG_DONTWAIT);
		if ((retval == -1 && (errno != EAGAIN && errno != EINTR))
				|| retval == 0) {
			/* disconnected */
			printf("\n----------------------------------------\n");
			printf("HOST REMOVED: %s\n", host->name);
			printf("\n----------------------------------------");
			host = remove_host(prev, host);
		}
		else if (buf == 'K') {
			/* update last ack timestamp */
			gettimeofday(&tmr, NULL);
			memcpy(&host->last_ack, &tmr, sizeof(tmr));
		}

		/* iterate */
		if (host) {
			prev = host;
			host = host->next;
		}
	}
}










