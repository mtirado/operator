#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <signal.h>

#include "../lib/ophost.h"
#include "../eslib/eslib.h"
#include "vllist.h"

/* TODO write some real tests for operator! this is quick n dirty */

void op_exec();
void host_exec();
void peer_exec();

int main()
{
	pid_t host;
	pid_t peer;
	int status;
	int loop = 1;


	/* fork test host */
	host = fork();
	if (host == 0)
		host_exec();
	else if (host == -1)
		return -1;

	/* fork peer */
	usleep(300000); /*0.3 seconds, allow host to register with operator */
	printf("forking peer...\n");
	peer = fork();
	if (peer == 0)
		peer_exec();
	else if (peer == -1)
		return -1;


	while (loop)
	{
		if (waitpid(host, &status, WNOHANG) > 0) {
			if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) < 0)
					printf("[test] host error.\n");
				else
					printf("[test] host exited normally.\n");
			}
			else
				printf("[test] host abort.\n");
			kill(peer, SIGKILL);
			loop = 0;
		}

		if (waitpid(peer, &status, WNOHANG) > 0) {
			if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) < 0)
					printf("[test] peer error.\n");
				else
					printf("[test] peer exited normally.\n");
			}
			else {
				printf("[test] peer aborted.\n");
				kill(host, SIGKILL);
			}
		}
		usleep(10000);
	}
	return 0;
}


/*
 * connect to host, send echo, receive reply
 */
void peer_exec()
{
	int echo;
	const char outmsg[] = "aloha";
	char inmsg[128];
	memset(inmsg, 0, sizeof(inmsg));
	printf("[peer] connecting to host\n");

	echo = ophost_connect("echo_service");
	if (echo == -1) {
		printf("[peer] failed to connect to echo_service\n");
		exit(-1);
	}

	printf("[peer] sending message(%s)\n", outmsg);
	/* send message */
	if (send(echo, outmsg, sizeof(outmsg), 0) != sizeof(outmsg)) {
		printf("[peer] send failed\n");
		exit(-1);
	}

	printf("[peer] waiting for response...\n");
	if (recv(echo, inmsg, sizeof(inmsg), 0) != sizeof(outmsg)) {
		printf("[peer] recv failed\n");
		exit(-1);
	}

	printf("[peer] received message(%s)\n", inmsg);
	/* confirm echo reply */
	if (strncmp(inmsg, outmsg, sizeof(outmsg)) != 0) {
		printf("[peer] unexpected reply\n");
		exit(-1);
	}
	printf("[peer] test passed.\n");
	exit(0);
}


/*
 * host
 */
int echo_service(struct vllist_node **peers);
void host_exec()
{
	struct ophost *host;
	int caller;
	struct vllist_node *peers = NULL;
	struct vllist_node *newnode = NULL;

	printf("[host] regsitering echo_service\n");
	host = ophost_register("echo_service");
	if (host == NULL) {
		printf("[host] registration failed\n");
		exit(-1);
	}

	printf("[host] testhost online\n");
	/* simple fast host connection handling loop */
	while (1)
	{
		/* ipchost upkeep -- accept new callers */
		if (ophost_accept(host)) {
			printf("[host] accept failed\n");
			exit(-1);
		}

		/* check completed, expired, or invalid handshakes */
		while (1)
		{
			caller = ophost_handshake(host);
			if (caller == -1 && errno == EAGAIN) {
				break;
			}
			else if (caller == -1) {
				printf("handshake error\n");
				exit(-1);
			}
			newnode = vllist_allocnode();
			if (newnode == NULL)
				exit(-20);
			vllist_addtail(&peers, newnode, (void *)caller);
			printf("[host] new peer in list: %d\n", caller);
		}
		/* service update */
		if (echo_service(&peers))
			exit(-1);
		usleep(10000); /*10ms*/
	}
	exit(0);
}


/*
 * for every peer node, echo messsages back
 * TODO finish writing test
 */
int echo_service(struct vllist_node **peers)
{
	char msg[128];
	int client;
	struct vllist_node *iter;
	int r;

	if (peers == NULL)
		return 0;

	vllist_while_fwd(peers, iter)
	{
		memset(msg, 0, sizeof(msg));
		client = (int)iter->v;
		r = recv(client, msg, sizeof(msg)-1, MSG_DONTWAIT);
		if (r == 0 || (r == -1 && errno != EAGAIN && errno != EINTR)) {
			printf("[echo_srv] client(%d) disconnect\n", client);
			vllist_eject(peers, iter);
		}
		else if (r != -1) {
			printf("[echo_srv] sending echo back %s\n", msg);
			if (send(client, msg, r, 0) != r) {
				printf("[echo_srv] shmbus_send error\n");
				abort();
			}
		}
		vllist_next(peers, iter);
	}
	return 0;
}



