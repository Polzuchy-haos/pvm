/*
 * transport_serial.c
 *
 * Debugging a board over a serial port. The frames are identical to the local
 * transport's -- the difference is only that they are written to and read from
 * a COM port, one byte at a time, in whatever chunks the driver feels like
 * delivering. Hence PVMDBG_FrameLength: bytes arrive with no regard for where
 * frames begin, so the reader accumulates until a whole one is present and
 * slides forward a byte at a time when the front of the buffer is nonsense.
 *
 * Only Windows is implemented; every other platform gets an honest refusal
 * rather than silence.
 */
#include "transport.h"

#include "pvmdbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

#define BAUD_RATE 					 CBR_115200
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

typedef struct{
	Transport_s base;
	HANDLE 			handle;
} Serial_s;

/* Wrap-safe: GetTickCount rolls over every 49 days, and unsigned subtraction
 * gives the right answer across the boundary while a "<" comparison would not. */
static int Elapsed(DWORD since, DWORD limitMs){
	return (DWORD)(GetTickCount() - since) >= limitMs;
}

static int SerialReadFrame(Serial_s* serial, char* frame){
	DWORD start = GetTickCount();
	int 	have 	= 0;

	while(!TransportInterrupted() && !Elapsed(start, REPLY_TIMEOUT_MS)){
		DWORD got = 0;
		int 	complete;

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

		if(!ReadFile(serial->handle, frame + have, (DWORD)(PVMDBG_CFG_BUF_SIZE - have), &got, 0))
			break;

		have += (int)got;
	}

	return 0;
}

static int SerialExchange(Transport_s* self, uint16_t cmd,
													const uint8_t* data, int dataLen, char* frame){
	Serial_s* serial 	= (Serial_s*)self;
	int 			total 	= PVMDBG_FrameBuild(frame, cmd, data, dataLen);
	DWORD 		written = 0;

	if(!WriteFile(serial->handle, frame, (DWORD)total, &written, 0) || written != (DWORD)total)
		return 0;

	return SerialReadFrame(serial, frame);
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
 * altogether: an unplugged cable or a reset, which is a real failure rather than
 * a program taking its time. A run that will not stop on its own is ended with
 * Ctrl+C, which returns to the prompt with the board paused.
 */
static int SerialSettle(Transport_s* self, int budgetMs){
	char 	frame[PVMDBG_CFG_BUF_SIZE];
	int 	silence = 0;
	int 	waitMs  = SETTLE_POLL_MIN_MS;
	DWORD start 	= GetTickCount();

	while(!TransportInterrupted()){
		const uint8_t* payload;
		uint16_t 			 replyCmd;
		int 					 payloadLen;
		int 					 len = SerialExchange(self, DBGCMD_STATE, 0, 0, frame);

		if(len > 0 && PVMDBG_FrameParse(frame, len, &replyCmd, &payload, &payloadLen) &&
			 replyCmd == DBGCMD_STATE && payloadLen >= 8){
			silence = 0;
			if(payload[6] != DBGMODE_RUNNING) return 1;
		}
		else if(++silence >= SETTLE_SILENCE_LIMIT){
			/* stderr, because stdout may be carrying a machine protocol */
			fprintf(stderr, "  the board has stopped answering\n");
			return 1;
		}

		if(budgetMs > 0 && Elapsed(start, (DWORD)budgetMs))
			return 0; /* still going -- the caller wanted the wheel back */

		Sleep((DWORD)waitMs);
		if(waitMs < SETTLE_POLL_MAX_MS) waitMs *= 2;
	}

	return 1; /* interrupted: the session pauses the board itself */
}

static void SerialClose(Transport_s* self){
	Serial_s* serial = (Serial_s*)self;

	CloseHandle(serial->handle);
	free(serial);
}

Transport_s* TransportSerialOpen(const char* portName){
	char 					path[64];
	Serial_s* 		serial;
	HANDLE 				handle;
	DCB 					dcb;
	COMMTIMEOUTS 	timeouts;

	/* The \\.\ form is required for COM10 and above, and works for COM1..9 too. */
	snprintf(path, sizeof(path), "\\\\.\\%s", portName);

	handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	if(handle == INVALID_HANDLE_VALUE){
		fprintf(stderr, "pvmdbg: cannot open %s (error %lu) -- wrong port, or something else has it open\n",
						portName, (unsigned long)GetLastError());
		return 0;
	}

	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);

	if(!GetCommState(handle, &dcb)){
		fprintf(stderr, "pvmdbg: %s is not a serial port\n", portName);
		CloseHandle(handle);
		return 0;
	}

	dcb.BaudRate 		 = BAUD_RATE;
	dcb.ByteSize 		 = 8;
	dcb.Parity 			 = NOPARITY;
	dcb.StopBits 		 = ONESTOPBIT;
	dcb.fBinary 		 = TRUE;
	dcb.fParity 		 = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl  = DTR_CONTROL_ENABLE;
	dcb.fRtsControl  = RTS_CONTROL_ENABLE;
	dcb.fOutX 			 = FALSE;
	dcb.fInX 				 = FALSE;

	if(!SetCommState(handle, &dcb)){
		fprintf(stderr, "pvmdbg: cannot configure %s (error %lu)\n",
						portName, (unsigned long)GetLastError());
		CloseHandle(handle);
		return 0;
	}

	/* Return from ReadFile promptly with whatever has arrived: the frame reader
	 * above is what decides when enough of it is there, not the driver. */
	timeouts.ReadIntervalTimeout 				 = 10;
	timeouts.ReadTotalTimeoutMultiplier  = 0;
	timeouts.ReadTotalTimeoutConstant 	 = 25;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant 	 = 200;
	SetCommTimeouts(handle, &timeouts);

	/* Anything already sitting in the port's buffers was sent before this session
	 * existed and answers nothing we asked. Left there, a single stale frame is
	 * read as the reply to the first command, and every reply after it is one
	 * behind -- a fault that looks like the board talking nonsense. */
	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

	serial = calloc(1, sizeof(Serial_s));
	if(!serial){ CloseHandle(handle); return 0; }

	serial->handle 					= handle;
	serial->base.exchange 	= SerialExchange;
	serial->base.settle 		= SerialSettle;
	serial->base.close 			= SerialClose;
	snprintf(serial->base.description, sizeof(serial->base.description),
					 "%s at %d baud", portName, (int)BAUD_RATE);

	return &serial->base;
}

#else /* !_WIN32 */

Transport_s* TransportSerialOpen(const char* portName){
	(void)portName;
	fprintf(stderr, "pvmdbg: the serial transport is only implemented for Windows\n");
	return 0;
}

#endif
