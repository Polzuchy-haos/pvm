/*
 * transport_tcp.c
 *
 * Debugging a board over a TCP connection: the same frames as over a serial
 * port, carried by whatever is listening at the far end -- a bridge between the
 * network and a board's UART, or pvmboard (a VM in a process of its own). The
 * frames go through unchanged, so a bridge needs to understand nothing about
 * them: it shovels bytes both ways.
 *
 * Frame handling and polling are shared with the serial transport, in
 * transport_stream.c; what is here is opening the connection and moving bytes.
 */
#include "net.h" /* before anything that might include windows.h */

#include "transport_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whatever is meant to be listening may still be starting -- a bridge launched
 * alongside the debugger, say -- so a refused connection is retried for a
 * while before it counts as nobody there. */
#define CONNECT_RETRY_MS 	(3000ul)
#define CONNECT_PAUSE_MS 	(100u)

/* How long a read waits for the first byte; the frame reader decides when a
 * whole frame is in, and how long that may take. */
#define READ_WAIT_MS 			(25)

typedef struct{
	StreamTransport_s stream;
	NetSocket 				sock;
} Tcp_s;

static int TcpRead(StreamTransport_s* self, char* buf, int max){
	Tcp_s* tcp = (Tcp_s*)self;
	int 	 ready = NetWaitReadable(tcp->sock, READ_WAIT_MS);
	int 	 n;

	if(ready < 0) 	return -1;
	if(ready == 0) return 0;

	n = recv(tcp->sock, buf, max, 0);
	return (n > 0) ? n : -1; /* 0 is the far end closing the connection */
}

static int TcpWrite(StreamTransport_s* self, const char* buf, int len){
	return NetSendAll(((Tcp_s*)self)->sock, buf, len);
}

static void TcpClose(Transport_s* self){
	Tcp_s* tcp = (Tcp_s*)self;

	NetClose(tcp->sock);
	NetCleanup();
	free(tcp);
}

static NetSocket Connect(const char* host, const char* port){
	struct addrinfo  hints;
	struct addrinfo* list = 0;
	NetSocket 			 sock = NET_INVALID;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family 	= AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if(getaddrinfo(host, port, &hints, &list) != 0) return NET_INVALID;

	for(struct addrinfo* ai = list; ai && sock == NET_INVALID; ai = ai->ai_next){
		sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(sock == NET_INVALID) continue;

		if(connect(sock, ai->ai_addr, (int)ai->ai_addrlen) != 0){
			NetClose(sock);
			sock = NET_INVALID;
		}
	}

	freeaddrinfo(list);
	return sock;
}

Transport_s* TransportTcpOpen(const char* address){
	char 					host[256], port[16];
	unsigned long start;
	NetSocket 		sock;
	Tcp_s* 				tcp;
	int 					noDelay = 1;

	if(!NetSplitAddress(address, "127.0.0.1", host, sizeof(host), port, sizeof(port))){
		fprintf(stderr, "pvmdbg: '%s' is not an address -- expected host:port, e.g. 127.0.0.1:5555\n", address);
		return 0;
	}

	if(!NetStartup()){
		fprintf(stderr, "pvmdbg: cannot start the network stack\n");
		return 0;
	}

	start = StreamNowMs();
	while((sock = Connect(host, port)) == NET_INVALID){
		if(StreamNowMs() - start >= CONNECT_RETRY_MS || TransportInterrupted()){
			fprintf(stderr, "pvmdbg: cannot connect to %s:%s -- is anything listening there?\n", host, port);
			NetCleanup();
			return 0;
		}
		StreamSleepMs(CONNECT_PAUSE_MS);
	}

	/* Frames are small and every one waits for an answer, which is the worst
	 * case for Nagle's algorithm: without this each request can sit in the
	 * stack for tens of milliseconds waiting for company that never comes. */
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

	/* Same reasoning as PurgeComm on a serial port: anything a bridge has
	 * buffered from before this session answers nothing we asked. */
	{
		char junk[64];
		while(NetWaitReadable(sock, 0) > 0 && recv(sock, junk, sizeof(junk), 0) > 0){}
	}

	tcp = calloc(1, sizeof(Tcp_s));
	if(!tcp){ NetClose(sock); NetCleanup(); return 0; }

	tcp->sock 							= sock;
	tcp->stream.read 				= TcpRead;
	tcp->stream.write 			= TcpWrite;
	tcp->stream.base.close 	= TcpClose;
	StreamTransportInit(&tcp->stream);
	snprintf(tcp->stream.base.description, sizeof(tcp->stream.base.description),
					 "%s:%s over TCP", host, port);

	return &tcp->stream.base;
}
