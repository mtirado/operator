/* (c) 2015 Michael R. Tirado -- GPLv3, GNU General Public License, version 3.
 * contact: mtirado418@gmail.com
 *
 * shmpair
 *
 * a pair of connected memfd's for send/recv packetized messages.
 * This turns out to be only slightly faster than AF_UNIX sockets after adding
 * data increment function to benchmark, which forces stale shmem pages to
 * refresh. so not as useful as I had hoped :( one possible advantage is that
 * you could have many channels open while consuming only 2 fd's.
 *
 * additionally you would need my non-standard MFD_SEAL_WRITE_PEER patch to
 * prevent other process from remaping memfd as r+w shared and
 * screwing everything up
 *
 */

#ifndef SHMPAIR_H__
#define SHMPAIR_H__
#include <sys/types.h>
/* TODO; variable number of channels */
#define _shmpair_channels 1
#define _shmpair_maxname 64
/* TODO this needs to be set by user somehow. */
#define _shmpair_maxsize (512 * 1024 * 1024 * _shmpair_channels \
			+ sizeof(struct shmpair_ctrl))
#define _shmpair_ident 0xb0b51ed5 /* we have a bobsled team */

/* writeto and readat may be confusing, writeto represents our write position.
 * readat represents our read position in other ends message pool.
 * this structure resides immediately before message slots
 * */
struct shmpair_ctrl
{
	/* writeto will always be ahead of other ctrl's readat */
	unsigned int writeto[_shmpair_channels];
	unsigned int readat[_shmpair_channels];
	char name[_shmpair_maxname];
	unsigned int ident; /* should always be _shmpair_ident */

	/*
	 * the below variables should only be used initially, when
	 * the connection is being negotiated, other end can modify
	 * this memory so do not rely on it to be valid!
	 */
	unsigned int msgslots;
	unsigned int msgsize;
	unsigned int rdonly; /* TODO */
};


/*
 * each channel has the same number of message slots of fixed size.
 * create will allocate a pool size of
 * msgsize * msgcount * channels + shmpair_ctrl size.
 *
 * messages are stored as a ring buffer
 * send will fail if no slots are available
 * recv will read the next message and mark slot as empty
 *
 */
struct shmpair
{
	char *inpool;
	char *outpool;
	struct shmpair_ctrl *outctrl;
	struct shmpair_ctrl *inctrl;
	unsigned int poolsize;
	unsigned int msgsize;
	unsigned int msgslots;

	int fdin;  /* memfd read */
	int fdout; /* memfd write */

	int nowrite; /* TODO this and finish hooking up rdonly */
	int open;   /* open for communication */
	int empty;  /* this slot is empty(for hosts peer array) */

	/* number of consecutive recv's that got no message */
	unsigned int inactivity;
};



/*
 * creates and opens a new shmpair. you can pass the file descriptor
 * and other processes can open it up using shmpair_open. the file
 * descriptor will remain open and memory mapped untill shmpair_close
 *
 *  notes XXX does not set CLOEXEC!
 *  you must do this to prevent file descriptor from being inherited on exec
 *
 * params
 *  self will be set to point at newly allocated object
 *  name of shmpair   < could probably remove this
 *  size of a message
 *  number of message slots, if full send fails
 *
 * returns
 *   0 all good
 *  -1 error
 */
int shmpair_create(struct shmpair **self,
		   char *name,
		   int msgsize,
		   int slots,
		   int rdonly);

/*
 * open a memfd and fill out shmpair structutre
 *
 * params
 *  instance
 *  file descriptor
 *
 * returns
 *    0 all good
 *   -1 error.
 * */
int shmpair_open(struct shmpair **self,
		 int memfd);

/*
 * destroy shmpair
 * */
int shmpair_destroy(struct shmpair *self);

/*
 * returns
 *  n bytes sent
 *  0 no send, try again later
 * -1 error
 * */
int shmpair_send(struct shmpair *self,
		 char *msg,
		 unsigned int size,
		 unsigned int channel);
/*
 * set buf to point at the new message slot. when you call recv again
 * this pointer may be overwritten, you should copy data out if need be.
 * returns
 *  n (message size)
 *  0 no new messages available
 * -1 error
 * */
int shmpair_recv(struct shmpair *self,
		 char **buf,
		 unsigned int channel);

/*
 * connect self to memfd, validates memfd is matching shmpair
 * returns
 * -1 error
 *  0
 */
int shmpair_pair(struct shmpair *self, int memfd);


#endif
