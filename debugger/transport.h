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

	/* Returns once the program has stopped again, after a command that set it
	 * going. The two transports differ sharply here: in-process, this is the
	 * loop that actually executes instructions; over a wire, the board is already
	 * running by itself and this only waits for it to come to rest. */
	void (*settle)(Transport_s* self);

	void (*close)(Transport_s* self);

	char description[128];
};

/* Both return null, having explained why on stderr, if they cannot start. */
Transport_s* TransportLocalOpen(const char* bytecodePath);
Transport_s* TransportSerialOpen(const char* portName);

/* Set by the session's Ctrl+C handler. Transports check it so that a long run,
 * or a board that has stopped answering, can be given up on. */
int TransportInterrupted(void);

#endif /* PVMDBG_TRANSPORT_H_ */
