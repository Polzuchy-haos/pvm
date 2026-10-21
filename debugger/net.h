#ifndef PVMDBG_NET_H_
#define PVMDBG_NET_H_

/*
 * The handful of socket calls the TCP transport and pvmboard need, papered over
 * so the same code builds against Winsock and BSD sockets. Include this before
 * anything that pulls in windows.h: winsock2.h has to come first.
 */

#include <stddef.h>

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* getaddrinfo */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET NetSocket;
#define NET_INVALID 	INVALID_SOCKET
#define NetClose(s) 	closesocket(s)
#define NET_SEND_FLAGS 0

static inline int NetStartup(void){
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
static inline void NetCleanup(void){ WSACleanup(); }

#else

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int NetSocket;
#define NET_INVALID 	(-1)
#define NetClose(s) 	close(s)
#ifdef MSG_NOSIGNAL
#define NET_SEND_FLAGS MSG_NOSIGNAL /* a closed peer is an error to handle, not a SIGPIPE */
#else
#define NET_SEND_FLAGS 0
#endif

static inline int  NetStartup(void){ return 1; }
static inline void NetCleanup(void){}

#endif

/* 1 when `s` has something to read (or has been closed), 0 when `timeoutMs`
 * went by first, -1 on error. */
static inline int NetWaitReadable(NetSocket s, int timeoutMs){
	fd_set 				 set;
	struct timeval tv;

	FD_ZERO(&set);
	FD_SET(s, &set);
	tv.tv_sec  = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;

	return select((int)s + 1, &set, 0, 0, &tv);
}

/* All of `len`, or -1. */
static inline int NetSendAll(NetSocket s, const char* buf, int len){
	int sent = 0;

	while(sent < len){
		int n = send(s, buf + sent, len - sent, NET_SEND_FLAGS);
		if(n <= 0) return -1;
		sent += n;
	}

	return len;
}

/* "host:port", or just "port" for `defaultHost`. 0 if it does not parse. */
static inline int NetSplitAddress(const char* text, const char* defaultHost,
																	char* host, size_t hostLen, char* port, size_t portLen){
	const char* colon = 0;
	size_t 			n;

	for(const char* p = text; *p; p++) if(*p == ':') colon = p;

	if(!colon){
		if(!*text || hostLen <= 1) return 0;
		n = 0;
		while(defaultHost[n] && n + 1 < hostLen){ host[n] = defaultHost[n]; n++; }
		host[n] = '\0';
		colon 	= text - 1;
	}
	else{
		n = (size_t)(colon - text);
		if(n == 0 || n >= hostLen) return 0;
		for(size_t i = 0; i < n; i++) host[i] = text[i];
		host[n] = '\0';
	}

	n = 0;
	for(const char* p = colon + 1; *p; p++){
		if(*p < '0' || *p > '9' || n + 1 >= portLen) return 0;
		port[n++] = *p;
	}
	port[n] = '\0';

	return n > 0;
}

#endif /* PVMDBG_NET_H_ */
