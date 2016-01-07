/* (c) 2015 Michael R. Tirado -- GPLv3, GNU General Public License, version 3.
 * contact: mtirado418@gmail.com
 */

#ifndef OPHOST_H__
#define OPHOST_H__

struct timeval;
/* represents a new connection that is being processed */
struct caller_handshake
{
	struct timeval timestamp; /* time of creation */
	struct caller_handshake *next;
	int socket;
};


struct ophost
{
	char name[OPHOST_MAXNAME];
	struct caller_handshake *handshakes;
	unsigned int num_hshks;
	struct timeval time_created;
	struct timeval last_ack;
	int socket; /* main line to operator */
	int relay;  /* relay new connections through operator */
};


/*
 * returns
 * af_unix socket connected to hostname
 * -1 on error
 */
int ophost_connect(char *hostname);


/*
 * returns
 * newly malloc'd and registered host
 * NULL on error
 */
struct ophost *ophost_register(char *hostname);


/*
 *  free any heap memory and shut down host
 *  returns
 *  0 if ok
 */
int ophost_destroy(struct ophost *host);


/*
 *  accept connection requests, create handshake
 *   0 if ok
 *  -1 on error
 */
int ophost_accept(struct ophost *self);

/*
 *  returns
 *  -1 on error
 *   af_unix socket connected to a new peer
 */
int ophost_handshake(struct ophost *self);

#endif
