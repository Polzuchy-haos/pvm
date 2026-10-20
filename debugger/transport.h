#ifndef PVMDBG_TRANSPORT_H_
#define PVMDBG_TRANSPORT_H_

#include <stdint.h>

/*
 * How the session reaches the debug agent it is driving. Everything above this
 * line -- every command the user types -- is written against these three
 * functions and nothing else, which is what lets the very same session drive a
 * VM in this process or a board on the far end of a serial cable.
 */
typedef struct Transport_s Transport_s;

struct Transport_s{
	/* Sends one command and returns the reply's length in `frame`, or 0 when
	 * nothing came back (a corrupt frame, or a board that is not answering). */
	int  (*exchange)(Transport_s* self, uint16_t cmd,
									 const uint8_t* data, int dataLen, char* frame);

	/*
	 * Lets a program that was set going make progress, and returns 1 once it has
	 * stopped. The two transports differ sharply here: in-process, this is the
	 * loop that actually executes instructions; over a wire, the board is already
	 * running by itself and this only waits for it to come to rest.
	 *
	 * `budgetMs` caps how long to stay inside, returning 0 if the program is
	 * still running when it runs out. The terminal session passes 0 for "no
	 * limit" and leans on Ctrl+C; a caller that has to stay responsive to its own
	 * input -- the machine mode driving VS Code -- passes a small budget and
	 * comes back round.
	 */
	int (*settle)(Transport_s* self, int budgetMs);

	void (*close)(Transport_s* self);

	char description[128];
};

/* All return null, having explained why on stderr, if they cannot start. */
Transport_s* TransportLocalOpen(const char* bytecodePath);
Transport_s* TransportSerialOpen(const char* portName);

/* "host:port", or "port" alone for this machine. The frames are exactly the
 * serial port's, so whatever listens there may be a plain byte bridge to a
 * board's UART. */
Transport_s* TransportTcpOpen(const char* address);

/* Set by the session's Ctrl+C handler. Transports check it so that a long run,
 * or a board that has stopped answering, can be given up on. */
int TransportInterrupted(void);

#endif /* PVMDBG_TRANSPORT_H_ */
