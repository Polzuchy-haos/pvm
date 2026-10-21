/*
 * pvmboard.c
 *
 * A board that is a process: the VM and its debug agent, exactly as firmware
 * runs them (see Core/Src/pvm_port.c in a board project, or the sketch in
 * pvm/pvmdbg.h), with a TCP socket where the UART would be. pvmdbg --tcp talks
 * to it the way it talks to a real board through a serial-to-TCP bridge --
 * which is what it is for: trying the network path, and testing a bridge or a
 * debugger change, without hardware.
 *
 * One connection at a time, like a UART. The program runs whether or not
 * anyone is connected, as on a board; --paused holds it before the first
 * instruction until a debugger says otherwise.
 *
 * Peripherals and SVCs are the desktop simulator's (simulator/pvmenv.c).
 */
#include "net.h" /* before anything that might include windows.h */

#include "pvm.h"
#include "pvmdbg.h"
#include "pvmenv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ADDRESS 			"127.0.0.1:5555"

/* Instructions run between looks at the socket: enough that the checks cost
 * nothing, few enough that a request is never kept waiting noticeably. */
#define TICKS_PER_CHECK 			(1024)

/* How long to sleep in select() when the VM is stopped and nothing arrives --
 * which keeps an idle board from burning a core. */
#define IDLE_WAIT_MS 					(20)

/* --once gives up when nobody turns up at all, so a test that failed to start
 * its debugger does not leave a board behind for ever. */
#define ONCE_ACCEPT_TIMEOUT_S (15)

static PVM_Word_t g_stack[PVM_CFG_STACK_SIZE];
static PVM_Word_t g_ram[PVM_CFG_RAM_SIZE];

static void Usage(void){
	fprintf(stderr,
		"usage: pvmboard program.bin [--listen [host:]port] [--paused] [--once]\n"
		"       --listen   where to accept the debugger (default " DEFAULT_ADDRESS ");\n"
		"                  0.0.0.0:port to accept it from other machines too\n"
		"       --paused   hold the program before its first instruction\n"
		"       --once     exit when the first debugger disconnects\n");
}

static char* LoadFile(const char* path, int* outLen){
	FILE* f = fopen(path, "rb");
	long 	size;
	char* buf;

	if(!f){ fprintf(stderr, "pvmboard: cannot open '%s'\n", path); return 0; }

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc(size > 0 ? (size_t)size : 1u);
	if(buf && size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size){ free(buf); buf = 0; }
	fclose(f);

	*outLen = (int)size;
	return buf;
}

static NetSocket Listen(const char* host, const char* port){
	struct addrinfo  hints;
	struct addrinfo* list = 0;
	NetSocket 			 sock = NET_INVALID;
	int 						 reuse = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family 	= AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags 		= AI_PASSIVE;

	if(getaddrinfo(host, port, &hints, &list) != 0) return NET_INVALID;

	for(struct addrinfo* ai = list; ai && sock == NET_INVALID; ai = ai->ai_next){
		sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(sock == NET_INVALID) continue;

		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
		if(bind(sock, ai->ai_addr, (int)ai->ai_addrlen) != 0 || listen(sock, 1) != 0){
			NetClose(sock);
			sock = NET_INVALID;
		}
	}

	freeaddrinfo(list);
	return sock;
}

/* Answers every whole frame in `frame`, keeping a partial one for later --
 * the same job, done the same way, as ServeDebugger in the firmware. Returns
 * 0 if the connection broke while answering. */
static int Serve(PVMDbg_s* dbg, NetSocket client, char* frame, int* have){
	char reply[PVMDBG_CFG_BUF_SIZE];

	while(*have > 0){
		int n = PVMDBG_FrameLength(frame, *have);
		int len;

		if(n == 0) break;

		if(n < 0){
			memmove(frame, frame + 1, (size_t)(*have - 1));
			(*have)--;
			continue;
		}

		memcpy(reply, frame, (size_t)n);
		memmove(frame, frame + n, (size_t)(*have - n));
		*have -= n;

		len = n;
		PVMDBG_Poll(dbg, reply, &len);
		if(len > 0 && NetSendAll(client, reply, len) != len) return 0;
	}

	return 1;
}

int main(int argc, char** argv){
	const char* path 		= 0;
	const char* address = DEFAULT_ADDRESS;
	int 				paused 	= 0;
	int 				once 		= 0;
	char 				host[256], port[16];
	char* 			code;
	int 				len;
	PVM_State_s vm;
	PVMDbg_s 		dbg;
	NetSocket 	server, client = NET_INVALID;
	char 				frame[PVMDBG_CFG_BUF_SIZE];
	int 				have 	 = 0;
	int 				served = 0;
	time_t 			started;

	for(int i = 1; i < argc; i++){
		if(!strcmp(argv[i], "--listen")){
			if(++i >= argc){ Usage(); return EXIT_FAILURE; }
			address = argv[i];
		}
		else if(!strcmp(argv[i], "--paused")) paused = 1;
		else if(!strcmp(argv[i], "--once")) 	once 	 = 1;
		else if(argv[i][0] == '-'){ Usage(); return EXIT_FAILURE; }
		else path = argv[i];
	}

	if(!path){ Usage(); return EXIT_FAILURE; }
	if(!NetSplitAddress(address, "127.0.0.1", host, sizeof(host), port, sizeof(port))){
		fprintf(stderr, "pvmboard: '%s' is not an address -- expected [host:]port\n", address);
		return EXIT_FAILURE;
	}

	code = LoadFile(path, &len);
	if(!code) return EXIT_FAILURE;

	memset(&vm, 0, sizeof(vm));
	PVM_Init(&vm, code, len, g_stack, PVM_CFG_STACK_SIZE, g_ram, PVM_CFG_RAM_SIZE);
	PVMENV_Attach(&vm);
	PVMDBG_Init(&dbg, &vm);

	if(paused){
		dbg.mode 	 = DBGMODE_STOPPED;
		dbg.reason = DBGSTOP_HOST;
	}

	if(!NetStartup()){ fprintf(stderr, "pvmboard: cannot start the network stack\n"); return EXIT_FAILURE; }

	server = Listen(host, port);
	if(server == NET_INVALID){
		fprintf(stderr, "pvmboard: cannot listen on %s:%s -- in use already?\n", host, port);
		NetCleanup();
		return EXIT_FAILURE;
	}

	/* stderr, like everything else this tool says. */
	fprintf(stderr, "pvmboard: '%s' (%d bytes), debugger on %s:%s%s\n",
					path, len, host, port, paused ? ", paused" : "");
	started = time(0);

	for(;;){
		/* Sleep in select() only when there is nothing to execute. */
		int waitMs = (dbg.mode == DBGMODE_STOPPED) ? IDLE_WAIT_MS : 0;

		if(client == NET_INVALID){
			if(NetWaitReadable(server, waitMs) > 0){
				int noDelay = 1;

				client = accept(server, 0, 0);
				if(client != NET_INVALID){
					setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
					have 	 = 0;
					served = 1;
					fprintf(stderr, "pvmboard: debugger connected\n");
				}
			}
			else if(once && !served && time(0) - started > ONCE_ACCEPT_TIMEOUT_S){
				fprintf(stderr, "pvmboard: nobody connected, giving up\n");
				break;
			}
		}
		else if(NetWaitReadable(client, waitMs) > 0){
			int n = recv(client, frame + have, (int)sizeof(frame) - have, 0);

			if(n > 0){
				have += n;
				if(have >= (int)sizeof(frame) && PVMDBG_FrameLength(frame, have) == 0) have = 0; /* never wedge */
			}

			if(n <= 0 || !Serve(&dbg, client, frame, &have)){
				NetClose(client);
				client = NET_INVALID;
				/* Unlike a UART, a socket says when the debugger has gone: let the
				 * program go even if it never got to send DETACH. */
				PVMDBG_Detach(&dbg);
				fprintf(stderr, "pvmboard: debugger disconnected\n");
				if(once) break;
			}
		}

		for(int i = 0; i < TICKS_PER_CHECK && PVMDBG_IsAccessExecTick(&dbg); i++){
			int status = PVM_ExecTick(&vm);

			/* As on a board with nobody watching: stop on the instruction that
			 * failed instead of retrying it for ever. */
			if(status < 0){
				fprintf(stderr, "pvmboard: execution error %d -- paused\n", status);
				dbg.mode 	 = DBGMODE_STOPPED;
				dbg.reason = DBGSTOP_HOST;
				break;
			}
		}
	}

	if(client != NET_INVALID) NetClose(client);
	NetClose(server);
	NetCleanup();
	free(code);
	return EXIT_SUCCESS;
}
