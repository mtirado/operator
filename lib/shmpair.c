/* (c) 2015 Michael R. Tirado -- GPLv3, GNU General Public License, version 3.
 * contact: mtirado418@gmail.com
 *
 *
 * sorry about messy debug printing, i stopped working on this when i noticed
 * shmem pages take quite a bit of time to be refreshed and performance isn't
 * significantly improved by using it.
 * systems doing super massive amounts of IPC may see some benefit, maybe
 * something in the kernel can be tweaked to boost shmem page refresh times.
 *
 */

#define _GNU_SOURCE
#include "shmpair.h"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <memory.h>
#include <linux/memfd.h>
#include <linux/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
/* this is poorly hacked for x86 VM testing*/
#ifndef __NR_memfd_create
#define __NR_memfd_create 356
#endif
#ifndef F_SEAL_WRITE_PEER
#define F_SEAL_WRITE_PEER 0x0010
#endif

extern int fcntl(int __fd, int __cmd, ...); /* can't include the header */
int memfd_create(const char *__name, unsigned int __flags)
{
	int retval = -1;
	__asm__("movl $356, %eax");
	__asm__("movl 8(%ebp), %ebx"); /* name */
	__asm__("movl 12(%ebp),%ecx"); /* flags */
	__asm__("int $0x80");
	__asm__("movl %%eax, %0" : "=q" (retval));

	/* supress unused variable warnings, ugh.. */
	__flags = (unsigned int)__name;
	++__flags;

	return retval;
}


int shmpair_create(struct shmpair **self, char *name, int msgsize,
						int slots, int rdonly)
{
	unsigned int shmsize = msgsize * slots * _shmpair_channels
					+ sizeof(struct shmpair_ctrl);
	struct shmpair_ctrl *mem;
	int memfd;
	unsigned int seals;
	unsigned int checkseals;

	if (!self)
		return -1;
	if (slots <= 1) {
		printf("2 slot minimum\n");
		return -1;
	}
	if (shmsize > _shmpair_maxsize) {
		printf("shm size: %u\n", shmsize);
		return -1;
	}
	if (strnlen(name, _shmpair_maxname) >= _shmpair_maxname) {
		printf("namelen\n: %s", name);
		return -1;
	}

	*self = malloc(sizeof(struct shmpair));
	if (*self == NULL) {
		printf("malloc()\n");
		return -1;
	}

	memfd = memfd_create(name, MFD_ALLOW_SEALING);
	if (memfd == -1) {
		printf("create error: %s\n", strerror(errno));
		goto freefail;
	}
	/* size it, if read only we only need out control info. no data*/
	/*if (rdonly) {
		if (ftruncate(memfd, sizeof(struct shmpair_ctrl)) == -1) {
			printf("truncate error: %s\n", strerror(errno));
			return -1;
		}
	}
	else*/
       	if (ftruncate(memfd, shmsize) == -1) {
		printf("truncate error: %s\n", strerror(errno));
		goto freefail;
	}

	mem = mmap(0, shmsize, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0);
	if (mem == MAP_FAILED) {
		printf("mmap error: %s\n", strerror(errno));
		goto freefail;
	}
	/* add seals to block peer writes, and seal the size. */
	seals =	  F_SEAL_SHRINK
		| F_SEAL_GROW
		/*| F_SEAL_WRITE_PEER XXX */
		| F_SEAL_SEAL;
	if (fcntl(memfd, F_ADD_SEALS, seals) == -1)
	{
		printf("seal error: %s\n", strerror(errno));
		goto freefail;
	}

	/* double check... XXX removeme */
	checkseals = (unsigned int)fcntl(memfd, F_GET_SEALS);
	if (checkseals != seals) {
		printf("seals not found on memfd\n");
		goto freefail;
	}

	memset(mem,  0, shmsize);
	memset(*self, 0, sizeof(struct shmpair));
	(*self)->fdin	   = -1; /* waiting for this */
	(*self)->fdout	   = memfd;
	(*self)->outctrl   = mem;
	(*self)->outpool   = (char *)(*self)->outctrl + sizeof(struct shmpair_ctrl);
	(*self)->poolsize  = msgsize * slots * _shmpair_channels;
	(*self)->msgslots  = slots;
	(*self)->msgsize   = msgsize;
	if (rdonly) {
		--rdonly;/* TODO readonly option */
		/*self->noread = 1;
		self->outctrl->rdonly = 1;*/
	}
	/* receivers will read and copy these on open */
	(*self)->outctrl->msgslots = slots;
	(*self)->outctrl->msgsize  = msgsize;
	(*self)->outctrl->ident    = _shmpair_ident;
	strncpy((*self)->outctrl->name, name, _shmpair_maxname-1);
	return 0;

freefail:
	free((*self));
	*self = NULL;
	return -1;
}


/* get the offset from start of pool to the message slot for given channel */
#define _chanslot_offset(chan_, slot_, numslots_, size_) \
	(size_*numslots_*chan_ + size_ * slot_)
int shmpair_send(struct shmpair *self, char *msg,
		 unsigned int size, unsigned int channel)
{
	unsigned int freeslot;
	char *mloc;

	if (!self || !msg || !size) {
		printf("bad param\n");
		return -1;
	}
	if (size > self->msgsize) {
		printf("bad size\n");
		return -1;
	}
	if (channel >= _shmpair_channels) {
		printf("bad channel\n");
		return -1;
	}

	freeslot = self->outctrl->writeto[channel] + 1;
	if (freeslot >= self->msgslots) /* wrap around */
		freeslot = 0;
	if (freeslot == self->inctrl->readat[channel])
		return 0; /* or block? */


	/* start at pool, go to channel address, then slot */
	mloc = (char *)self->outpool + _chanslot_offset(channel, freeslot,
			self->msgslots, self->msgsize);

	memcpy(mloc, msg, size);
	/*memset(mloc+size, 0, self->msgsize - size);*/

	/* update position after data has been written! */
	self->outctrl->writeto[channel] = freeslot;

	return size;
}


int shmpair_recv(struct shmpair *self, char **buf, unsigned int channel)
{
	unsigned int readat;
	char *mloc;
	if (!self || !buf)
		return -1;
	if (channel >= _shmpair_channels)
		return -1;

	/* check for new messages */
	readat = self->outctrl->readat[channel];
	if(readat == self->inctrl->writeto[channel]) {
		++self->inactivity;
		return 0;
	}

	/* increment to next slot */
	if(++readat >= self->msgslots)
		readat = 0;

	mloc = self->inpool + _chanslot_offset(channel, readat,
			self->msgslots, self->msgsize);
	*buf = mloc;

	/* update position after data has been read!! */
	self->outctrl->readat[channel] = readat;
	self->inactivity = 0;

	return self->msgsize;
}


/*
 *  returns 0 if memfd contains a shmpair that matches
 */
static int shmpair_validate(struct shmpair *self, int memfd)
{
	unsigned int seals;
	off_t mapsize;
	char *mem;
	struct shmpair_ctrl preamble;

	if (!self)
		return -1;

	/* make sure it's a memfd, with proper seals */
	seals = fcntl(memfd, F_GET_SEALS);
	if (seals != ( F_SEAL_SHRINK
		     | F_SEAL_GROW
		     /*| F_SEAL_WRITE_PEER XXX */
		     | F_SEAL_SEAL ))
		return -1;

	/* map and validate control data */
	mem = mmap(0, sizeof(struct shmpair_ctrl),
			PROT_READ, MAP_PRIVATE, memfd, 0);
	if (mem == MAP_FAILED || mem == 0x0) {
		printf("mmap failed\n");
		return -1;
	}
	/* read data */
	memcpy(&preamble, mem, sizeof(struct shmpair_ctrl));
	munmap(mem, sizeof(struct shmpair_ctrl));

	/* check for shmpair identifier */
	if (preamble.ident != _shmpair_ident) {
		printf("bad shmpair ident\n");
		return -1;
	}
	if (preamble.msgslots <= 1) {
		printf("2 slot minimum\n");
		return -1;
	}
	/*
	 * check size of actual mapping
	 * XXX size limited by int max, off_t is 32/64 like size_t, but signed
	 * XXX -- also, if you add variable num channels, don't forget
	 *	  to validate that too ;)
	 *	  SEAL_WRITE_PEER also applies SEAL_SHRINK
	 */
	mapsize = lseek(memfd, 0, SEEK_END);
	if (mapsize <=0 || (unsigned int)mapsize != self->poolsize
						  + sizeof(struct shmpair_ctrl)
			|| preamble.msgsize  != self->msgsize
			|| preamble.msgslots != self->msgslots) {
		printf("invalid map size\n");
		return -1;
	}

	return 0;
}


/*
 * connect input to an already created shmpair.
 * establish connection between self and another shmpair memfd.
 */
int shmpair_pair(struct shmpair *self, int memfd)
{
	unsigned int shmsize;
	struct shmpair_ctrl *mem;

	if (shmpair_validate(self, memfd)) {
		printf("shmpair_pair: invalid memfd\n");
		return -1;
	}
	shmsize = self->poolsize + sizeof(struct shmpair_ctrl);

	mem = mmap(0, shmsize, PROT_READ, MAP_PRIVATE, memfd, 0);
	if (mem == MAP_FAILED || mem == 0x0) {
		printf("mmap error: %s\n", strerror(errno));
		return -1;
	}

	self->fdin = memfd;
	self->inctrl = mem;
	self->inpool = (char *)mem + sizeof(struct shmpair_ctrl);
	return 0;
}


/*
 *  create a shmpair from an incoming file descriptor
 *  VERIFY SEALS open fd, read control data, verify size, create new bus
 *  finish by connecting the buses.
 */
int shmpair_open(struct shmpair **self, int memfd)
{
	char *mem;
	struct shmpair_ctrl preamble;
	int retval;

	if (self == NULL)
		return -1;

	/* map it and validate control data */
	mem = mmap(0, sizeof(struct shmpair_ctrl),
			PROT_READ, MAP_PRIVATE, memfd, 0);
	if (mem == MAP_FAILED || mem == 0x0) {
		printf("failed:%d -- %s\n", errno, strerror(errno));
		return -1;
	}

	memcpy(&preamble, mem, sizeof(struct shmpair_ctrl));
	munmap(mem, sizeof(struct shmpair_ctrl));


	/* create our new half */
	if ( (retval = shmpair_create(self, preamble.name, preamble.msgsize,
				preamble.msgslots, preamble.rdonly)) ) {
		printf("error creating shmpair: return code %d\n", retval);
		return -1;
	}

	/* validate other */
	if ((retval = shmpair_validate(*self, memfd))) {
		printf("invalid memfd, or control data: %d\n", retval);
		return -1;
	}

	/* pair them */
	if (shmpair_pair(*self, memfd)) {
		printf("shmpair_pair error\n");
		return -1;
	}

	return 0;
}


int shmpair_destroy(struct shmpair *self)
{
	int retval;

	if (!self)
		return -1;

	retval = 0;
	/* zero ctrl struct */
	/*memset(self->outctrl, 0, sizeof(struct shmpair_ctrl));*/
	if (munmap(self->outctrl, self->poolsize+sizeof(struct shmpair_ctrl)))
		retval = -1;
	if (munmap(self->inctrl, self->poolsize+sizeof(struct shmpair_ctrl)))
		retval = -1;
	if (close(self->fdout))
		retval = -1;
	if (close(self->fdin))
		retval = -1;

	memset(self, 0, sizeof(struct shmpair));
	free(self);
	return retval;
}










