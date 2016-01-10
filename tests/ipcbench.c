/* (c) 2015 GPLv3, GNU General Public License, version 3. Michael R. Tirado.
 *
 * IPC related benchmarks, using operator.
 *
 * currently, just a huge data transfer benchmark.
 * there are two modes, host and peer. the protocol goes like this:
 *
 * ack is a single character 'K'
 * total size of message is a 32 bit unsigned integer
 *
 * host registers with operator
 * peer connects to host through operator
 *
 * peer sends total size of message to relay
 *	host acks
 * peer sends data to host
 *	host acks upon completion
 *	host sends message back
 * peer writes perf data and closes connection.
 *
 *
 * current transports used:
 *	unix domain sockets	-- socket, AF_UNIX, SOCK_STREAM
 *	shmpair			-- memfd, shmem
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

#include "../lib/ophost.h"
#include "../lib/shmpair.h"
#include "../eslib/eslib.h"

/* get elapsed time between start and end timespecs */
#define elapsed_nano(start_, end_) (unsigned int) (			\
		      ((end_.tv_sec  - start_.tv_sec) * 1000000000)	\
		     + (end_.tv_nsec - start_.tv_nsec)			\
)
#define elapsed_micro(start_, end_) (double) (				\
		      ((end_.tv_sec  - start_.tv_sec) * 1000000.0)	\
		     + ((end_.tv_nsec - start_.tv_nsec)	/ 1000.0)	\
)
#define elapsed_milli(start_, end_) (double) (				\
		      ((end_.tv_sec  - start_.tv_sec) * 1000.0)		\
		     + ((end_.tv_nsec - start_.tv_nsec)	/ 1000000.0)	\
)

struct perfdat
{
	struct timespec t_start;
	struct timespec t_send;
	struct timespec t_sendfin;
	struct timespec t_ack;
	struct timespec t_finish;
};

/* starts at 1K and doubles until 256M @19 passes */
#define NUM_PASSES 19
struct perfdat testdat[NUM_PASSES];
unsigned int g_testsize;

enum {
	AFUNIX=0,
	SHMPAIR
};
enum {
	HOST=0,
	PEER
};

int bench_increment(char *buf, unsigned int size)
{
	unsigned int r = 0;
	unsigned int i = 0;
	while (i < size)
	{
		r += buf[i] + 1;
		++i;
	}
	return r;
}

struct shmpair *shmpair_host_handshake(int afpeer)
{
	struct shmpair *shmpeer = NULL;
	int mfd = -1;
	const char a_ok = 'K';


	if (shmpair_create(&shmpeer, "blah", g_testsize, 2, 0)) {
		printf("could not create shmpair\n");
		return NULL;
	}

	if (eslib_sock_send_fd(afpeer, shmpeer->fdout)) {
		printf("error sending memfd\n");
		goto fail;
	}

	/* wait for peer to send their half back */
	while (1)
	{
		int r;

		r = eslib_sock_recv_fd(afpeer, &mfd);
		if (r == -1 && (errno == EAGAIN || errno == EINTR)) {
			continue;
		}
		else if (r == -1) {
			printf("recv_fd errror\n");
			goto fail;
		}
		else {
			printf("host received peer memfd\n");
			break;
		}
	}

	/* pair them */
	if (shmpair_pair(shmpeer, mfd)) {
		printf("shmpair_pair error\n");
		goto fail;
	}

	/* ack peer so they know we're open for business */
	if (send(afpeer, &a_ok, 1, MSG_DONTWAIT) != 1) {
		printf("send: %s\n", strerror(errno));
		goto fail;
	}

	return shmpeer;

fail:
	close(mfd);
	shmpair_destroy(shmpeer);
	return NULL;
}

struct shmpair *shmpair_peer_handshake(int afhost)
{
	char buf;
	struct shmpair *shmhost = NULL;
	int mfd = -1;

	/* wait for host to send shmpair memfd */
	while (1)
	{
		int r = eslib_sock_recv_fd(afhost, &mfd);
		if (r == -1 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (r == -1)
			goto fail;
		else
			break;
	}

	/* create our shmpair, send our fd back to host */
	if (shmpair_open(&shmhost, mfd))
		goto fail;
	if (eslib_sock_send_fd(afhost, shmhost->fdout)) /* send our half */
		goto fail;

	/* wait for host to ack */
	while (1)
	{
		int r = recv(afhost, &buf, 1, 0);
		if (r == -1 && errno == EINTR)
			continue;
		else if (r == -1)
			goto fail;
		else
			break;
	}

	if (buf != 'K') {
		printf("ack error\n");
		goto fail;
	}


	return shmhost;

fail:
	printf("shmpair_peer_handshake error\n");
	close(mfd);
	return NULL;

}

int af_unix_host()
{
	struct ophost *host;
	int peer;
	int ret = 0;
	unsigned int size = 0;
	unsigned int bytes = 0;
	char *buf;
	const char ack = 'K';
	int recv_count = 0;
	int send_count = 0;

	/* register host */
	host = ophost_register("afunix");
	if (host == NULL) {
		printf("host register failure\n");
		return -1;
	}

	/* wait for peer */
	peer = -1;
	while (peer == -1) {
		if (ophost_accept(host)) {
			printf("operator has gone down\n");
			return -1;
		}
		peer = ophost_handshake(host);
		/*usleep(1000);*/
	}

	/* wait for message size */
	ret = recv(peer, &size, sizeof(size), 0);
	if (ret != sizeof(size)) {
		printf("msg size(%d): %s\n", ret, strerror(errno));
		return -1;
	}

	if (size == 0)
		return -1;
	buf = malloc(size);
	if (!buf)
		return -1;

	/* read entire message */
	bytes = 0;
	while (bytes < size) {
		ret = recv(peer, &buf[bytes], size - bytes, 0);
		if (ret == -1 || ret == 0) {
			printf("host recv error(%d, %d): %s\n",
					ret, errno, strerror(errno));
			printf("size = %u\n", size);
			printf("bytes = %u\n", bytes);
			return -1;
		}
		bytes += ret;
		++recv_count;
		/*printf("[host]bytes received: %u\n", bytes);*/
	}


	/* send ack to host */
	if (send(peer, &ack, 1, 0) != 1) {
		printf("ack error: %s\n", strerror(errno));
		return -1;
	}

	/* some data processing(added to recv time) */
	bench_increment(buf, size);

	/* send message back */
	bytes = 0;
	while (bytes < size) {
		ret = send(peer, buf, size - bytes, 0);
		if (ret == -1 || ret == 0) {
			printf("send error: %s\n", strerror(errno));
			return -1;
		}
		bytes += ret;
		++send_count;
		/*printf("[host]bytes sent: %u\n", bytes);*/
	}

	printf("[host] send iterations: %d\n", send_count);
	printf("[host] recv iterations: %d\n", recv_count);

	ophost_destroy(host);
	free(buf);
	return 0;
}



int af_unix_peer(unsigned int size, struct perfdat *out)
{
	struct timespec t_start, t_send, t_sendfin, t_ack, t_finish;
	int host;
	char *buf;
	unsigned int  bytes;
	int ret;
	int recv_count = 0;
	int send_count = 0;

	/* connect to host */
	if (clock_gettime(CLOCK_REALTIME, &t_start))
		return -1;
	host = ophost_connect("afunix");
	if (host == -1) {
		printf("could not connect\n");
		return -1;
	}

	/* loop with different sizes to chart data < TODO */
	buf = malloc(size);
	if (!buf)
		return -1;

	memset(buf, 'A', size);

	/*send size */
	ret = send(host, &size, sizeof(size), 0);
	if (ret != sizeof(size)) {
		printf("send size error\n");
		return -1;
	}

	/* send data */
	bytes = 0;
	if (clock_gettime(CLOCK_REALTIME, &t_send))
		return -1;
	while (bytes < size) {
		ret = send(host, &buf[bytes], size - bytes, 0);
		if (ret == -1 || ret == 0) {
			printf("send error: %s\n", strerror(errno));
			return -1;
		}
		bytes += ret;
		++send_count;
		/*printf("[peer]bytes sent: %u\n", bytes);*/
	}
	if (clock_gettime(CLOCK_REALTIME, &t_sendfin))
		return -1;

	/* wait for ack */
	ret = recv(host, buf, 1, 0);
	if (ret != 1 || buf[0] != 'K') {
		printf("ack failure\n");
		return -1;
	}
	if (clock_gettime(CLOCK_REALTIME, &t_ack))
		return -1;

	/* recv data */
	bytes = 0;
	while (bytes < size) {
		ret = recv(host, &buf[bytes], size - bytes, 0);
		if (ret == -1 || ret == 0) {
			printf("peer recv error: %s\n", strerror(errno));
			return -1;
		}
		bytes += ret;
		++recv_count;
		/*printf("[peer]bytes received: %u\n", bytes);*/
	}

	/* some data processing(added to recv time) */
	bench_increment(buf, size);

	if (clock_gettime(CLOCK_REALTIME, &t_finish))
		return -1;


	printf("[peer] send iterations: %d\n", send_count);
	printf("[peer] recv iterations: %d\n", recv_count);
	free(buf);

	memcpy(&out->t_start,  &t_start,  sizeof(struct timespec));
	memcpy(&out->t_send,   &t_send,   sizeof(struct timespec));
	memcpy(&out->t_sendfin,&t_sendfin,sizeof(struct timespec));
	memcpy(&out->t_ack,    &t_ack,    sizeof(struct timespec));
	memcpy(&out->t_finish, &t_finish, sizeof(struct timespec));

	shutdown(host, SHUT_RDWR);
	close(host);
	return 0;
}



int shmpair_host()
{
	struct ophost *host;
	struct shmpair *peer;
	int afpeer = -1;
	int ret = 0;
	unsigned int size = 0;
	unsigned int bytes = 0;
	char *buf;
	char ack = 'K';
	int recv_count = 0;
	int send_count = 0;

	/* register host */
	host = ophost_register("shmpair");
	if (!host) {
		printf("host register failure\n");
		return -1;
	}

	/* wait for peer */
	afpeer = -1;
	while (afpeer == -1) {
		if (ophost_accept(host)) {
			printf("operator has gone down\n");
			return -1;
		}
		afpeer = ophost_handshake(host);
		/*usleep(1000);*/
	}

	peer = shmpair_host_handshake(afpeer);
	if (peer == NULL) {
		printf("host handshake error(%d)\n", afpeer);
		return -1;
	}

	/* now for the annoying downside, user has to implement blocking */
	ret = 0;
	while (ret == 0)
	{
		ret = shmpair_recv(peer, &buf, 0);
		/*usleep(1000);*/
	}
	if (ret == -1) {
		/*printf("shmpair_recv failed\n");*/
		return -1;
	}
	memcpy(&size, buf, sizeof(int));
	if (size == 0)
		return -1;
	if (size != g_testsize) {
		printf("size doesnt match\n");
		return -1;
	}

	/* read entire message */
	bytes = 0;
	while (bytes < size) {
		ret = shmpair_recv(peer, &buf, 0);
		if (ret == -1) {
			printf("host recv error(%d, %d): %s\n",
					ret, errno, strerror(errno));
			printf("size = %u\n", size);
			printf("bytes = %u\n", bytes);
			return -1;
		}
		else if (ret != 0) {
			bytes += ret;
			++recv_count;
		}
		/*else if (ret == 0) { XXX check how much it actually blocks
		 * 			iirc it was updating at like 100hz, but
		 * 			that may have been in qemu, not metal.
			usleep(100);
		}*/
		/*printf("[host]bytes received: %u\n", bytes);*/
	}


	if (shmpair_send(peer, &ack, 1, 0) != 1) {
		printf("shmpair_send(ack) failed\n");
		return -1;
	}
	/* some data processing(added to recv time) */
	bench_increment(buf, size);
	bytes = 0;
	while (bytes < size) {
		ret = shmpair_send(peer, buf, peer->msgsize, 0);
		if (ret == -1) {
			printf("shmpair_send failed\n");
			return -1;
		}
		else if (ret != 0) {
			bytes += ret;
			++send_count;
		}
		/*else { XXX check block stats
			usleep(100);
		}*/
		/*printf("[host]bytes sent: %u\n", bytes);*/
	}

	printf("[host] send iterations: %d\n", send_count);
	printf("[host] recv iterations: %d\n", recv_count);

	ophost_destroy(host);
	shmpair_destroy(peer);
	printf("host returning 0\n");
	return 0;
}

/* XXX reeeemove me */
#define _chanslot_offset(chan_, slot_, numslots_, size_) \
	(size_*numslots_*chan_ + size_ * slot_)

int shmpair_peer(unsigned int size, struct perfdat *out)
{
	struct timespec t_start, t_send, t_sendfin, t_ack, t_finish;
	struct shmpair *host;
	int afhost;
	char *buf = NULL;
	char *upload;
	unsigned int  bytes;
	int ret;
	int recv_count = 0;
	int send_count = 0;

	/* connect to host */
	if (clock_gettime(CLOCK_REALTIME, &t_start))
		return -1;

	afhost = ophost_connect("shmpair");
	if (afhost == -1) {
		printf("could not connect\n");
		return -1;
	}

	host = shmpair_peer_handshake(afhost);
	if (host == NULL) {
		printf("peer handshake error\n");
		return -1;
	}

	/* loop with different sizes to chart data < TODO */
	upload = malloc(size);
	if (!upload)
		return -1;

	memset(upload, 'A', size);

do_over:
	/*send size */
	ret = shmpair_send(host, (void *)&size, sizeof(size), 0);
	if (ret != sizeof(size)) {
		if (ret == 0)
			goto do_over;
		printf("send size error\n");
		return -1;
	}


	/* send data */
	bytes = 0;
	if (clock_gettime(CLOCK_REALTIME, &t_send))
		return -1;
	while (bytes < size) {
		ret = shmpair_send(host, upload, size - bytes, 0);
		if (ret == -1) {
			printf("send error: %s\n", strerror(errno));
			return -1;
		}
		else if (ret != 0) {
			bytes += ret;
			++send_count;
		}
		/*printf("[peer]bytes sent: %u\n", bytes);*/
	}
	if (clock_gettime(CLOCK_REALTIME, &t_sendfin))
		return -1;

	/* recv ack */
	ret = 0;
	while (ret == 0)
	{
		/*printf("shmpair_recv()\n");*/
		ret = shmpair_recv(host, &buf, 0);
	/*	usleep(1000);*/
	}
	if (ret == -1) {
		/*printf("shmpair_recv(ack) error\n");*/
		return -1;
	}

	if (clock_gettime(CLOCK_REALTIME, &t_ack))
		return -1;
	if (buf[0] != 'K') {
		printf("invalid ack\n");
		return -1;
	}
	/* recv data */
	bytes = 0;
	while (bytes < size) {
		ret = shmpair_recv(host, &buf, 0);
		if (ret == -1) {
			printf("peer recv error: %s\n", strerror(errno));
			return -1;
		}
		else if (ret != 0) {
			bytes += ret;
			++recv_count;
		}
		/* else { XXX check blocking stats
		 * }
		 */
		/*printf("[peer]bytes received: %u\n", bytes);*/
	}

	/* some data processing(added to recv time) */
	bench_increment(buf, size);

	if (clock_gettime(CLOCK_REALTIME, &t_finish))
		return -1;

	free(upload);
	shmpair_destroy(host);

	printf("[peer] send iterations: %d\n", send_count);
	printf("[peer] recv iterations: %d\n", recv_count);

	memcpy(&out->t_start,  &t_start,  sizeof(struct timespec));
	memcpy(&out->t_send,   &t_send,   sizeof(struct timespec));
	memcpy(&out->t_sendfin,&t_sendfin,sizeof(struct timespec));
	memcpy(&out->t_ack,    &t_ack,    sizeof(struct timespec));
	memcpy(&out->t_finish, &t_finish, sizeof(struct timespec));

	return 0;
}



/*
 * usage:
 * ipcbench [afunix|shmpair]
 */
int main(int argc, char *argv[])
{
	int type = -1;
	int ret = 0;
	unsigned int ipclen;
	struct perfdat p;
	pid_t pid;
	int i;
	int status;

	printf("PID: %u\n", (unsigned int)getpid());
	if (argc != 2)
		goto print_usage;

	memset(testdat, 0, sizeof(testdat));
	ipclen = strnlen(argv[1], 32);

	if (strncmp("afunix", argv[1], ipclen) == 0) {
		printf("ipc type: afunix\n");
		type = AFUNIX;
	}
	else if (strncmp("shmpair", argv[1], ipclen) == 0) {
		printf("ipc type: shmpair\n");
		type = SHMPAIR;
	}
	else
		goto print_usage;

	g_testsize = 1024;
	for (i = 0; i < NUM_PASSES; ++i)
	{
		pid = fork();
		if (pid == 0) {
			if (type == AFUNIX) {
				ret = af_unix_host();
			}
			else {
				ret = shmpair_host();
			}
			if (ret) {
				printf("host error\n");
				return -1;
			}
			_exit(0);
		} else if (pid == -1) {
			printf("fork() %s\n", strerror(errno));
			return -1;
		}
		usleep(100000); /* give host time to register */
		printf("running test size: %u\n", g_testsize);
		if (type == AFUNIX) {
			ret = af_unix_peer(g_testsize, &p);
		}
		else {
			ret = shmpair_peer(g_testsize, &p);
		}
		if (ret) {
			printf("peer error\n");
			return -1;
		}
		g_testsize *= 2;
		memcpy(&testdat[i], &p, sizeof(struct perfdat));
		/* wait for host thread to shut down */
		if (waitpid(pid, &status, 0) != pid) {
			printf("waitpid(): %s\n", strerror(errno));
			return -1;
		}
		/* give operator some time to update */
		usleep(100000);
	}

	/* peer finishes down here */
	g_testsize = 1024;
	for (i = 0; i < NUM_PASSES; ++i)
	{
		printf("\n---------------------------\n");
		printf("pass %dKB\n", g_testsize/1024);
		printf("-----------------------------\n");
		printf("send:  %f\n",   elapsed_milli(testdat[i].t_send,testdat[i].t_sendfin));
		printf("recv:  %f\n",   elapsed_milli(testdat[i].t_ack, testdat[i].t_finish));
		printf("transit: %f\n", elapsed_milli(testdat[i].t_send,testdat[i].t_sendfin)
				      + elapsed_milli(testdat[i].t_ack, testdat[i].t_finish));
		printf("total(+protocol overhead): %f\n", elapsed_milli(testdat[i].t_start, testdat[i].t_finish));
		g_testsize *= 2;
	}


	return 0;


print_usage:
	printf("usage:\n");
	printf("ipcbench [afunix|shmpair]\n");
	return -1;
}
