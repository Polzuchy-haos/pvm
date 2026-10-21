#ifndef PVMDBG_TRANSPORT_STREAM_H_
#define PVMDBG_TRANSPORT_STREAM_H_

/*
 * What a serial port and a TCP connection have in common: a byte stream that
 * delivers whatever has arrived, in chunks that have nothing to do with where
 * frames begin and end, from a board that runs by itself and can only be asked
 * whether it has stopped. Everything above the raw read and write -- picking
 * frames out of the stream, waiting for a reply, polling a running board -- is
 * written once here, so the two cannot come to behave differently.
 */

#include "transport.h"

typedef struct StreamTransport_s StreamTransport_s;

struct StreamTransport_s{
	Transport_s base; /* first, so a StreamTransport_s* is a Transport_s* */

	/* Whatever has arrived, up to `max` bytes, waiting a few tens of
	 * milliseconds at most for the first one. 0 when nothing came in that
	 * time, -1 when the stream is gone for good (a closed connection). */
	int (*read)(StreamTransport_s* self, char* buf, int max);

	/* All `len` bytes, or -1. */
	int (*write)(StreamTransport_s* self, const char* buf, int len);
};

/* Fills in base.exchange and base.settle; the caller provides read, write,
 * base.close and base.description. */
void StreamTransportInit(StreamTransport_s* stream);

/* Milliseconds from an arbitrary start, and a pause -- here because both
 * transports need them and the platform calls differ. */
unsigned long StreamNowMs(void);
void 					StreamSleepMs(unsigned ms);

#endif /* PVMDBG_TRANSPORT_STREAM_H_ */
