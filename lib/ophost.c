/* (c) 2015 Michael R. Tirado -- GPLv3, GNU General Public License, version 3.
 * contact: mtirado418@gmail.com
 *
 * implements operator protocol for making af_unix connections
 * with callers from other mount/network namespaces.
 *
 * data sent from operator to host is 1 byte at a time,
 * currently only 'R' -- connection request.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>

#include "ophost.h"
#include "../eslib/eslib.h"


/*
 *  create the new af_unix connection and
 *  send that fd back to caller through operator.
 *
 *  note: this is a socketpair, so checking credentials at time
 *  of connection through SO_PEERCRED will not be very useful
 *  for host trying to authenticate a caller.
 */
static int ophost_create_callerhandshake(struct ophost *self)
{
	struct caller_handshake *new_hshk;
	int pair[2];
	if (!self)
		return -1;

	if (self->num_hshks >= OPHOST_MAXHANDSHAKES)
		return 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
		return -1;

	/* send to caller through operator relay */
	if (eslib_sock_send_fd(self->relay, pair[0])) {
		close(pair[0]);
		close(pair[1]);
		return -1;
	}
	close(pair[0]);

	/* create handshake */
	new_hshk = malloc(sizeof(*new_hshk));
	if (new_hshk == NULL) {
		eslib_sock_axe(pair[1]);
		return 0;
	}

	memset(new_hshk, 0, sizeof(*new_hshk));
	new_hshk->socket = pair[1];
	gettimeofday(&new_hshk->timestamp, NULL);
	++self->num_hshks;

	/* add to list */
	new_hshk->next   = self->handshakes;
	self->handshakes = new_hshk;
	return 0;
}


/*
 * deallocate handshake, and update list,
 * returns previous node.
 *
 * note: this invalidates hshk and prev,
 * they should be updated immediately after call
 * */
static struct caller_handshake *ophost_handshake_destroy(struct ophost *self,
				       struct caller_handshake *prev,
				       struct caller_handshake *hshk)
{
	if (!hshk || !self)
		return NULL;

	if (prev) {
		prev->next = hshk->next;
		free(hshk);
		hshk = prev; /* rewind */
	}
	else {  /* removing at front of list */
		self->handshakes = hshk->next;
		free(hshk);
		hshk = NULL;
	}
	if (self->num_hshks)
		--self->num_hshks;
	return hshk;
}


/*
 *  accept new connections by processing connection requests.
 *  operator sends host a connection request 'R',
 *  we respond by creating a socketpair and send half
 *  back to caller through operator relay
 *
 *  as we cannot call connect across namespace without a bind mount.
 */
int ophost_accept(struct ophost *self)
{
	struct timeval tmr;
	int i;
	int retval;
	char msg;
	const char a_ok = 'K';

	if (!self)
		return -1;

	/* send ping back to operator
	 * TODO make this optional and in it's own function
	 * and make timeout customizable through cfg file
	 * if operator goes down we could possibly
	 * handle that by going into some kind of reconnect loop
	 */
	gettimeofday(&tmr, NULL);
	if (eslib_ms_elapsed(tmr, self->last_ack, OPHOST_PINGDELAY)) {
		if (send(self->socket, &a_ok, 1, MSG_DONTWAIT) != 1) {
			return -1;
		}
		memcpy(&self->last_ack, &tmr, sizeof(tmr));
	}

	/* process requests */
	for (i = 0; i < OPHOST_MAXACCEPT; ++i)
	{
		if (self->num_hshks >= OPHOST_MAXHANDSHAKES)
			return 0;

		retval = recv(self->socket, &msg, 1, MSG_DONTWAIT);
		if (retval == -1 && (errno == EINTR || errno == EAGAIN))
			continue;
		else if (retval == -1)
			return -1;
		else if (msg == 'R') { /* connection request */
			if (ophost_create_callerhandshake(self)) {
				printf("error creating caller handshake\n");
				return -1;
			}
		}
		else {
			printf("ophost_accept recv'd bad operator message\n");
		}
	}
	return 0;
}


/*
 * return the first new connection in list
 * sets errno to EAGAIN if no more handshakes
 */
int ophost_handshake(struct ophost *self)
{
	int ret;
	struct caller_handshake *hshk;

	if (self == NULL) {
		errno = EINVAL;
		return -1;
	}
	hshk = self->handshakes;

	if (!hshk) {
		errno = EAGAIN;
		return -1;
	}

	ret = hshk->socket;
	ophost_handshake_destroy(self, NULL, hshk);
	return ret;
}


/* connect to a registered host */
int ophost_connect(char *hostname)
{
	char msg[OPHOST_MAXNAME];
	struct sockaddr_un addr;
	struct timeval tmr, start;
	unsigned int len;
	int retval;
	int sock = -1;
	int fd = -1;

	if (hostname == NULL)
		return -1;

	memset(&addr, 0, sizeof(addr));
	strncpy(addr.sun_path, OP_REQ_PATH, sizeof(addr.sun_path)-1);
	addr.sun_family = AF_UNIX;
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1)
		return -1;
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		printf("operator connect: %s\n", strerror(errno));
		return -1;
	}

	/* send hostname request */
	strncpy(msg, hostname, OPHOST_MAXNAME-1);
	msg[OPHOST_MAXNAME-1] = '\0';
	len = strnlen(msg, OPHOST_MAXNAME-1) + 1;
	if (send(sock, msg, len, MSG_DONTWAIT) != (int)len ) {
		printf("send error: %s\n", strerror(errno));
		goto fail;
	}

	/* setup timer */
	gettimeofday(&start, NULL);
	memcpy(&tmr, &start, sizeof(tmr));

	/* response should be a half of a socketpair */
	fd = -1;
	while(!eslib_ms_elapsed(tmr, start, OPHOST_HSHKDELAY))
	{
		gettimeofday(&tmr, NULL);
		retval = eslib_sock_recv_fd(sock, &fd);
		if (retval == 0)
			break;
		else if (retval == -1 && errno != EAGAIN) {
			printf("recv_fd failed\n");
			goto fail;
		}

		usleep(1000); /* 1ms */
	}
	if (fd == -1)
		goto fail;

	eslib_sock_axe(sock);
	return fd;

fail:
	printf("connect failed.\n");
	eslib_sock_axe(sock);
	return -1;
}


static struct ophost *ophost_create(int opsock, int relay)
{
	struct ophost *host = NULL;
	const char a_ok = 'K';

	if (opsock == -1 || relay == -1)
		return NULL;

	host = malloc(sizeof(*host));
	if (!host)
		return NULL;

	memset(host, 0, sizeof(*host));
	gettimeofday(&host->time_created, NULL); /* setup timestamps */
	memcpy(&host->last_ack,	&host->time_created, sizeof(host->last_ack));
	host->socket  = opsock;
	host->relay   = relay;

	/* send initial ack back to operator to activate host */
	if (send(opsock, &a_ok, 1, MSG_DONTWAIT) != 1) {
		printf("ophost_create: ack failed.\n");
		free(host);
		return NULL;
	}

	return host;
}


/* register host with operator */
struct ophost *ophost_register(char *hostname)
{
	int sock;
	int relay;
	int retval;
	struct sockaddr_un addr;
	char msg[OPHOST_MAXNAME+sizeof(int)];
	struct timeval tmr, stamp;
	struct ophost *newhost = NULL;
	int len;

	if (!hostname)
		return NULL;
	if (strnlen(hostname, OPHOST_MAXNAME) >= OPHOST_MAXNAME)
		return NULL;

	memset(&addr, 0, sizeof(addr));
	strcpy(addr.sun_path, OP_REG_PATH);
	addr.sun_family = AF_UNIX;
	gettimeofday(&stamp, NULL);
	memcpy(&tmr, &stamp, sizeof(stamp));

	/* connect to operator */
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1) {
		printf("socket: %s\n", strerror(errno));
		goto fail;
	}
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		printf("could not connect to operator %s\n", strerror(errno));
		goto fail;
	}

	/* send registration message */
	strncpy(msg, hostname, OPHOST_MAXNAME-1);
	msg[OPHOST_MAXNAME-1] = '\0';
	len = strnlen(msg, OPHOST_MAXNAME-1) + 1;
	if (send(sock, msg, len, MSG_DONTWAIT) != len) {
		printf("send: %s\n", strerror(errno));
		goto fail;
	}

	/* wait for relay */
	relay = -1;
	while(!eslib_ms_elapsed(tmr, stamp, OPHOST_HSHKDELAY))
	{
		gettimeofday(&tmr, NULL);
		retval = eslib_sock_recv_fd(sock, &relay);
		if (retval == 0) /* got it */
			break;
		else if (retval == -1 && errno != EAGAIN) {
			printf("recv relay error: %s\n", strerror(errno));
			goto fail;
		}

		usleep(1000); /* 1ms */
	}
	if (relay == -1) {
		printf("ophost handshake timeout\n");
		goto fail;
	}

	/* allocate new host struct */
	newhost = ophost_create(sock, relay);
	if (newhost == NULL) {
		printf("ophost_create error\n");
		goto fail;
	}
	return newhost;
fail:
	close(sock);
	close(relay);
	return NULL;

}


int ophost_destroy(struct ophost *self)
{
	struct caller_handshake *tmp;

	if (self == NULL)
		return -1;

	eslib_sock_axe(self->socket);
	eslib_sock_axe(self->relay);
	self->socket  = -1;
	self->relay   = -1;

	/* destroy handshakes */
	while(self->handshakes)
	{
		tmp = self->handshakes;
		self->handshakes = tmp->next;
		eslib_sock_axe(tmp->socket);
		free(tmp);
	}

	free(self);
	return 0;

}




