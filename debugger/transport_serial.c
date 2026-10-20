/*
 * transport_serial.c
 *
 * Debugging a board over a serial port. Only what is particular to a COM port
 * lives here -- opening and configuring it, reading and writing bytes; picking
 * frames out of the byte stream and polling a running board is shared with the
 * TCP transport, in transport_stream.c.
 *
 * Only Windows is implemented; every other platform gets an honest refusal
 * rather than silence.
 */
#include "transport_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

#define BAUD_RATE CBR_115200

typedef struct{
	StreamTransport_s stream;
	HANDLE 						handle;
} Serial_s;

static int SerialRead(StreamTransport_s* self, char* buf, int max){
	Serial_s* serial = (Serial_s*)self;
	DWORD 		got 	 = 0;

	if(!ReadFile(serial->handle, buf, (DWORD)max, &got, 0)) return -1;
	return (int)got;
}

static int SerialWrite(StreamTransport_s* self, const char* buf, int len){
	Serial_s* serial 	= (Serial_s*)self;
	DWORD 		written = 0;

	if(!WriteFile(serial->handle, buf, (DWORD)len, &written, 0) || written != (DWORD)len) return -1;
	return len;
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

	/* Return from ReadFile the moment anything has arrived, or after 25 ms of
	 * nothing: the frame reader is what decides when enough of it is there, not
	 * the driver. MAXDWORD/MAXDWORD/constant is the one combination Windows
	 * documents as "return as soon as a byte is in"; an interval timeout instead
	 * made every reply wait it out on top -- some 30 ms per exchange, which
	 * --host, at an exchange or two per instruction, cannot afford. */
	timeouts.ReadIntervalTimeout 				 = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier  = MAXDWORD;
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

	serial->handle 							= handle;
	serial->stream.read 				= SerialRead;
	serial->stream.write 				= SerialWrite;
	serial->stream.base.close 	= SerialClose;
	StreamTransportInit(&serial->stream);
	snprintf(serial->stream.base.description, sizeof(serial->stream.base.description),
					 "%s at %d baud", portName, (int)BAUD_RATE);

	return &serial->stream.base;
}

#else /* !_WIN32 */

Transport_s* TransportSerialOpen(const char* portName){
	(void)portName;
	fprintf(stderr, "pvmdbg: the serial transport is only implemented for Windows\n");
	return 0;
}

#endif
