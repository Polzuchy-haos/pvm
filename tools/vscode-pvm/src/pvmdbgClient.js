'use strict';

const { spawn } = require('child_process');

/*
 * Speaks the line protocol of "pvmdbg --machine" (see debugger/machine.h).
 *
 * That process is where all the real work happens -- frames, CRCs, the serial
 * port, resynchronising a byte stream -- and it is the same binary already
 * tested against a board. Nothing about the wire format is reimplemented here;
 * this only sends lines and reads lines back.
 */

/* "ok pc=39 sp=11" -> { ok: true, pc: 39, sp: 11 };  "error refused" -> { ok: false, error: 'refused' } */
function parseReply(line) {
	const parts = line.trim().split(/\s+/);

	if (parts[0] === 'error') return { ok: false, error: parts[1] || 'unknown' };
	if (parts[0] !== 'ok') return { ok: false, error: `unparsable reply: ${line}` };

	const reply = { ok: true };
	for (const part of parts.slice(1)) {
		const eq = part.indexOf('=');
		if (eq > 0) reply[part.slice(0, eq)] = Number(part.slice(eq + 1));
	}
	return reply;
}

class PvmdbgClient {
	constructor() {
		this.proc = null;
		this.buffer = '';
		/* Replies come back strictly in the order the commands went out, so a queue
		 * of resolvers is all the correlation this needs -- no request ids. */
		this.pending = [];
		this.onStderr = null;
		this.onExit = null;
	}

	start(exePath, args) {
		this.proc = spawn(exePath, args, { stdio: ['pipe', 'pipe', 'pipe'] });

		this.proc.stdout.on('data', (chunk) => this._consume(chunk.toString()));
		this.proc.stderr.on('data', (chunk) => {
			if (this.onStderr) this.onStderr(chunk.toString());
		});

		this.proc.on('error', (err) => this._abort(err.message));
		this.proc.on('exit', (code) => {
			this._abort(`pvmdbg exited (${code})`);
			if (this.onExit) this.onExit(code);
		});
	}

	_consume(text) {
		this.buffer += text;

		let newline;
		while ((newline = this.buffer.indexOf('\n')) >= 0) {
			const line = this.buffer.slice(0, newline).replace(/\r$/, '');
			this.buffer = this.buffer.slice(newline + 1);

			const waiting = this.pending.shift();
			if (waiting) waiting.resolve(parseReply(line));
		}
	}

	/* Nobody is going to answer any more: fail the callers still waiting rather
	 * than leaving their promises hanging for the rest of the session. */
	_abort(reason) {
		const stranded = this.pending;
		this.pending = [];
		this.proc = null;
		for (const waiting of stranded) waiting.resolve({ ok: false, error: reason });
	}

	command(text) {
		return new Promise((resolve) => {
			if (!this.proc) {
				resolve({ ok: false, error: 'pvmdbg is not running' });
				return;
			}
			this.pending.push({ resolve });
			this.proc.stdin.write(text + '\n');
		});
	}

	async stop() {
		if (!this.proc) return;

		await this.command('quit');
		if (this.proc) this.proc.kill();
		this.proc = null;
	}
}

module.exports = { PvmdbgClient, parseReply };
