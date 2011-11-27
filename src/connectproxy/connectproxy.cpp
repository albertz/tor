
// DYLD_INSERT_LIBRARIES=/Users/az/Library/Developer/Xcode/DerivedData/Tor-fznskzgbnoqjmcaiipujdkqolerb/Build/Products/Debug/libtorify.dylib curl http://www.az2000.de -s >/dev/null

#ifdef USE_GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Header Files */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#ifdef USE_SOCKS_DNS
#include <resolv.h>
#endif

#include "mach_override.h"

#define USE_SOCKS_DNS 1

/* Prototype and function header for connect function */
#define CONNECT_SIGNATURE int __fd, const struct sockaddr * __addr, socklen_t __len

/* The type of socket structure pointer to use to call the 
 * real connect */
#define CONNECT_SOCKARG struct sockaddr *

/* Prototype and function header for select function */
#define SELECT_SIGNATURE int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout

/* Prototype and function header for poll function */
#define POLL_SIGNATURE struct pollfd *ufds, nfds_t nfds, int timeout

/* Prototype and function header for close function */
#define CLOSE_SIGNATURE int fd


/* Global Declarations */
static int (*realconnect)(CONNECT_SIGNATURE);
static int (*realselect)(SELECT_SIGNATURE);
static int (*realpoll)(POLL_SIGNATURE);
static int (*realclose)(CLOSE_SIGNATURE);
#ifdef USE_SOCKS_DNS
static int (*realresinit)(void);
#endif

/* Exported Function Prototypes */
void _init(void);

extern "C" {
int connect(CONNECT_SIGNATURE);
int select(SELECT_SIGNATURE);
int poll(POLL_SIGNATURE);
int close(CLOSE_SIGNATURE);
#ifdef USE_SOCKS_DNS
int res_init(void);
#endif
}

/* Private Function Prototypes */
void _init(void) __attribute__ ((constructor));

void _init(void) {
	fprintf(stderr, "hello from init\n");
	
	realconnect = (int (*)(CONNECT_SIGNATURE))dlsym(RTLD_NEXT, "connect");
	realselect = (int (*)(SELECT_SIGNATURE))dlsym(RTLD_NEXT, "select");
	realpoll = (int (*)(POLL_SIGNATURE))dlsym(RTLD_NEXT, "poll");
	realclose = (int (*)(CLOSE_SIGNATURE))dlsym(RTLD_NEXT, "close");

#ifdef USE_SOCKS_DNS
	realresinit = (int (*)())dlsym(RTLD_NEXT, "res_init");
#endif

	//	Override local function by pointer.
	kern_return_t err = 0;
	err = mach_override_ptr((void*)realconnect, (void*)connect, (void**)&realconnect);	
	err = mach_override_ptr((void*)realselect, (void*)select, (void**)&realselect);	
	err = mach_override_ptr((void*)realpoll, (void*)poll, (void**)&realpoll);	
	err = mach_override_ptr((void*)realclose, (void*)close, (void**)&realclose);	
}

int connect(CONNECT_SIGNATURE) {       
	fprintf(stderr, "connect %i\n", __fd);
	return realconnect(__fd, __addr, __len);
}

int select(SELECT_SIGNATURE) {
	fprintf(stderr, "select %i\n", n);
	return realselect(n, readfds, writefds, exceptfds, timeout);	
}

int poll(POLL_SIGNATURE) {
	fprintf(stderr, "poll\n");
	return realpoll(ufds, nfds, timeout);
}

int close(CLOSE_SIGNATURE) {
	fprintf(stderr, "close %i\n", fd);
	return realclose(fd);
}

#ifdef USE_SOCKS_DNS
int res_init(void) {
	fprintf(stderr, "res_init\n");
	return realresinit();
}
#endif

