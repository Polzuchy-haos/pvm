/*
 * transport_stream.c
 *
 * The frame-level half of every transport that is a byte stream to a board
 * running on its own: a serial port (transport_serial.c), a TCP connection
 * (transport_tcp.c). See transport_stream.h.
 */
#include "transport_stream.h"

#include "pvmdbg.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define REPLY_TIMEOUT_MS 		 (500)

/* How often to ask a running board whether it has stopped yet. Starts brisk, so
 * a breakpoint that is about to be hit is reported at once, then backs off, so
 * a program that runs for minutes is not pestered forty times a second -- every
 * one of those questions costs the board a reply it has to stop and send. */
#define SETTLE_POLL_MIN_MS 	 (10)
#define SETTLE_POLL_MAX_MS 	 (250)

/* Consecutive unanswered questions before the board is called gone. Each one
 * already waited REPLY_TIMEOUT_MS, so this is a couple of seconds of silence. */
#define SETTLE_SILENCE_LIMIT (4)

unsigned long StreamNowMs(void){
#ifdef _WIN32
	return (unsigned long)GetTickCount();
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long)now.tv_sec * 1000ul + (unsigned long)(now.tv_nsec / 1000000);
#endif
}

void StreamSleepMs(unsigned ms){
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec pause = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
	nanosleep(&pause, 0);
#endif
}

/* Wrap-safe: the tick count rolls over, and unsigned subtraction gives the right
 * answer across the boundary where a "<" comparison would not. */
static int Elapsed(unsigned long since, unsigned long limitMs){
	return (unsigned long)(StreamNowMs() - since) >= limitMs;
}

/* Bytes arrive with no regard for where frames begin, so they are accumulated
 * until a whole frame is there, sliding forward a byte at a time while the
 * front of the buffer cannot start one. */
static int ReadFrame(StreamTransport_s* stream, char* frame){
	unsigned long start = StreamNowMs();
	int 					have 	= 0;

	while(!TransportInterrupted() && !Elapsed(start, REPLY_TIMEOUT_MS)){
		int got, complete;

		if(have > 0){
			complete = PVMDBG_FrameLength(frame, have);
			if(complete > 0) return complete;

			if(complete < 0){ /* junk at the front: drop a byte and look again */
				memmove(frame, frame + 1, (size_t)(have - 1));
				have--;
				continue;
			}
		}

		if(have >= PVMDBG_CFG_BUF_SIZE){ have = 0; continue; } /* cannot happen, but never wedge */

		got = stream->read(stream, frame + have, PVMDBG_CFG_BUF_SIZE - have);
		if(got < 0) break; /* the stream is gone: no point waiting out the timeout */

		have += got;
	}

	return 0;
}

static int StreamExchange(Transport_s* self, uint16_t cmd,
													const uint8_t* data, int dataLen, char* frame){
	StreamTransport_s* stream = (StreamTransport_s*)self;
	int 							 total 	= PVMDBG_FrameBuild(frame, cmd, data, dataLen);

	if(stream->write(stream, frame, total) != total) return 0;
	return ReadFrame(stream, frame);
}

/*
 * The board is already running by the time RUN's reply arrives, and the protocol
 * gives it no way to announce that it has stopped -- so the only thing the host
 * can do is keep asking. An event from the device would turn this into a wait
 * rather than a poll; see docs/debugger.md.
 *
 * There is deliberately no limit on how long a program may run. An earlier
 * version gave up after ten seconds, which meant every 'run' on a program
 * without a breakpoint -- the ordinary case for firmware, which is a loop that
 * never ends -- cost exactly ten seconds of staring at nothing before the prompt
 * came back. What is watched for instead is a board that has stopped answering
 * altogether: an unplugged cable, a reset, a dropped connection, which is a real
 * failure rather than a program taking its time. A run that will not stop on
 * its own is ended with Ctrl+C, which returns to the prompt with the board
 * paused.
 */
static int StreamSettle(Transport_s* self, int budgetMs){
	char 					frame[PVMDBG_CFG_BUF_SIZE];
	int 					silence = 0;
	unsigned 			waitMs  = SETTLE_POLL_MIN_MS;
	unsigned long start 	= StreamNowMs();

	while(!TransportInterrupted()){
		const uint8_t* payload;
		uint16_t 			 replyCmd;
		int 					 payloadLen;
		int 					 len = StreamExchange(self, DBGCMD_STATE, 0, 0, frame);

		if(len > 0 && PVMDBG_FrameParse(frame, len, &replyCmd, &payload, &payloadLen) &&
			 replyCmd == DBGCMD_STATE){
			silence = 0;
			if(payload[2] != DBGMODE_RUNNING) return 1; /* pc(2) mode(1) reason(1) */
		}
		else if(++silence >= SETTLE_SILENCE_LIMIT){
			/* stderr, because stdout may be carrying a machine protocol */
			fprintf(stderr, "  the board has stopped answering\n");
			return 1;
		}

		if(budgetMs > 0 && Elapsed(start, (unsigned long)budgetMs))
			return 0; /* still going -- the caller wanted the wheel back */

		StreamSleepMs(waitMs);
		if(waitMs < SETTLE_POLL_MAX_MS) waitMs *= 2;
	}

	return 1; /* interrupted: the session pauses the board itself */
}

void StreamTransportInit(StreamTransport_s* stream){
	stream->base.exchange = StreamExchange;
	stream->base.settle 	= StreamSettle;
}
