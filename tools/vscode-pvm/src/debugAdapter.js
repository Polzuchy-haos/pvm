'use strict';

const fs = require('fs');
const path = require('path');

const {
	LoggingDebugSession, InitializedEvent, StoppedEvent, TerminatedEvent, OutputEvent,
	Thread, StackFrame, Source, Scope
} = require('@vscode/debugadapter');

const { PvmdbgClient } = require('./pvmdbgClient');
const { SourceMap } = require('./sourceMap');

/*
 * Translates between VS Code's Debug Adapter Protocol and "pvmdbg --machine".
 *
 * Deliberately thin. Everything that is hard or dangerous -- the frame format,
 * the CRC, the serial port, resynchronising a byte stream, the breakpoint
 * bookkeeping on the device -- lives in the C tool, which is already tested and
 * already proven against a board. This file sends lines and shapes the answers
 * into what an editor wants to show.
 */

/* PVMDbgStop_e in pvm/pvmdbg.h */
const STOP_HOST = 1;
const STOP_STEP = 2;
const STOP_BREAKPOINT = 3;
const STOP_HALTED = 4;

/* A line of source is a handful of instructions; a runaway loop inside one is a
 * bug somewhere, and stepping forever with the editor frozen would hide it. */
const MAX_STEP_INSTRUCTIONS = 2000;

const THREAD_ID = 1;
const SCOPE_REGISTERS = 1;
const SCOPE_STACK = 2;
const SCOPE_GLOBALS = 3;
/* A frame's locals are SCOPE_LOCALS + its index in the call stack. */
const SCOPE_LOCALS = 1000;

class PvmDebugSession extends LoggingDebugSession {
	constructor() {
		super('pvmdbg.log');

		this.client = new PvmdbgClient();
		this.map = new SourceMap();
		this.lastPc = 0;
		/* Frames in the last stack trace that came with symbols; 0 without. */
		this.frameCount = 0;
		this.stopRequested = false;
		this.running = false;
		/* Every breakpoint VS Code has asked for, across all files: the device is
		 * told the whole set each time, because setBreakPoints arrives per file and
		 * says nothing about the others. */
		this.wanted = new Map();

		this.setDebuggerLinesStartAt1(true);
		this.setDebuggerColumnsStartAt1(true);
	}

	/* ---- lifecycle ------------------------------------------------------ */

	initializeRequest(response) {
		response.body = response.body || {};
		response.body.supportsConfigurationDoneRequest = true;
		response.body.supportsSteppingGranularity = false;
		response.body.supportsTerminateRequest = true;

		this.sendResponse(response);
		this.sendEvent(new InitializedEvent());
	}

	async launchRequest(response, args) {
		const exe = args.pvmdbg || 'pvmdbg';
		const spawnArgs = [];

		const mapPath = args.map || (args.program ? args.program.replace(/\.[^.\\/]*$/, '.dbgmap') : null);

		if (args.port) spawnArgs.push('--port', args.port);
		if (args.port && args.baud) spawnArgs.push('--baud', String(args.baud));
		else if (args.tcp) spawnArgs.push('--tcp', args.tcp);
		else if (args.program) spawnArgs.push(args.program);
		/* pvmdbg reads the map's symbols itself and answers "frames"/"vars" --
		 * the frame walking stays in C, next to the protocol it is built on. */
		if (mapPath && fs.existsSync(mapPath)) spawnArgs.push('--map', mapPath);
		spawnArgs.push('--machine');

		this.client.onStderr = (text) => this.sendEvent(new OutputEvent(text, 'stderr'));
		this.client.onExit = () => this.sendEvent(new TerminatedEvent());
		this.client.start(exe, spawnArgs);

		if (mapPath) {
			try {
				this.map.load(mapPath);
			} catch (err) {
				/* Not fatal: without a map the session still steps and shows the
				 * program counter, it just cannot talk about source lines. */
				this.sendEvent(new OutputEvent(`pvmdbg: no debug map (${err.message})\n`, 'console'));
			}
		}

		const state = await this._state();
		if (!state) {
			this.sendErrorResponse(response, 1001, 'pvmdbg is not answering');
			return;
		}

		this.sendResponse(response);

		if (args.stopOnEntry === false) await this._continue();
		else this.sendEvent(new StoppedEvent('entry', THREAD_ID));
	}

	configurationDoneRequest(response) {
		this.sendResponse(response);
	}

	async disconnectRequest(response) {
		this.running = false;
		await this.client.stop();
		this.sendResponse(response);
	}

	/* ---- breakpoints ---------------------------------------------------- */

	async setBreakPointsRequest(response, args) {
		const file = args.source.path || '';
		const lines = (args.breakpoints || []).map((b) => b.line);

		this.wanted.set(file, lines);

		const placed = await this._syncBreakpoints();

		response.body = {
			breakpoints: lines.map((line) => {
				const address = this.map.addressFor(file, line);
				return { verified: address !== null && placed.has(address), line };
			})
		};
		this.sendResponse(response);
	}

	/* The device holds a handful of slots and knows nothing about files, so the
	 * whole set is rebuilt rather than diffed: clear everything, then arm what is
	 * still wanted. */
	async _syncBreakpoints() {
		const current = await this.client.command('breaks');
		if (current.ok) {
			for (let i = 0; i < current.n; i++)
				await this.client.command(`clear ${current[`a${i}`]}`);
		}

		const placed = new Set();
		for (const [file, lines] of this.wanted) {
			for (const line of lines) {
				const address = this.map.addressFor(file, line);
				if (address === null) continue;

				const reply = await this.client.command(`break ${address}`);
				if (reply.ok) placed.add(address);
			}
		}

		return placed;
	}

	/* ---- execution ------------------------------------------------------ */

	async continueRequest(response) {
		this.sendResponse(response);
		await this._continue();
	}

	async nextRequest(response) {
		this.sendResponse(response);
		await this._stepLine(true);
	}

	async stepInRequest(response) {
		this.sendResponse(response);
		await this._stepLine(false);
	}

	async stepOutRequest(response) {
		this.sendResponse(response);
		await this._stepLine(true);
	}

	pauseRequest(response) {
		this.stopRequested = true;
		this.sendResponse(response);
	}

	async _continue() {
		await this.client.command('run');
		await this._runUntilStopped();
	}

	/*
	 * "run" only sets the program going; this is what lets it make progress. The
	 * loop is here, in the adapter, rather than inside a blocking call in the
	 * tool, so that a pause can still be honoured while a program that never ends
	 * is running -- which is the normal shape of firmware.
	 */
	async _runUntilStopped() {
		this.running = true;
		this.stopRequested = false;

		for (;;) {
			if (this.stopRequested) {
				await this.client.command('pause');
				break;
			}

			const reply = await this.client.command('poll');
			if (!reply.ok) {
				this.running = false;
				this.sendEvent(new TerminatedEvent());
				return;
			}
			if (reply.running === 0) break;
		}

		this.running = false;
		await this._reportStop();
	}

	/*
	 * One source line, not one instruction: an editor's step buttons are about
	 * lines, and a single instruction usually leaves the highlight where it was.
	 *
	 * Stepping over keeps going while the frame pointer is deeper than it started
	 * -- that is what "do not descend into the call on this line" amounts to,
	 * since fp only grows on ENTER.
	 */
	async _stepLine(stepOver) {
		const start = await this._state();
		if (!start) { this.sendEvent(new TerminatedEvent()); return; }

		const from = this.map.locationFor(start.pc);

		for (let i = 0; i < MAX_STEP_INSTRUCTIONS; i++) {
			const stepped = await this.client.command('step');
			if (!stepped.ok) break;

			/* Locally the instruction only actually runs when the tool is given the
			 * time to tick; on a board it has already happened. */
			await this.client.command('poll');

			const now = await this._state();
			if (!now) { this.sendEvent(new TerminatedEvent()); return; }
			if (now.reason === STOP_HALTED) break;

			if (!from) break; /* no map: a step is a single instruction, and that is that */
			if (stepOver && now.fp > start.fp) continue;

			const here = this.map.locationFor(now.pc);
			if (!here) break;
			if (here.line !== from.line || here.file !== from.file) break;
		}

		await this._reportStop();
	}

	async _reportStop() {
		const state = await this._state();
		if (!state) { this.sendEvent(new TerminatedEvent()); return; }

		if (state.reason === STOP_HALTED) {
			this.sendEvent(new OutputEvent('pvmdbg: program finished (HALT)\n', 'console'));
			this.sendEvent(new StoppedEvent('pause', THREAD_ID));
			return;
		}

		const reasons = {
			[STOP_HOST]: 'pause',
			[STOP_STEP]: 'step',
			[STOP_BREAKPOINT]: 'breakpoint'
		};
		this.sendEvent(new StoppedEvent(reasons[state.reason] || 'pause', THREAD_ID));
	}

	/* ---- inspection ----------------------------------------------------- */

	threadsRequest(response) {
		response.body = { threads: [new Thread(THREAD_ID, 'pvm')] };
		this.sendResponse(response);
	}

	async stackTraceRequest(response) {
		const frames = await this.client.command('frames');
		let stackFrames = [];

		/* With symbols, pvmdbg walks the real call stack; without them there is
		 * just where PC is. */
		if (frames.ok && frames.n > 0) {
			for (let i = 0; i < frames.n; i++) {
				const pc = frames[`pc${i}`];
				/* A caller's pc is the return address -- the instruction after its
				 * CALL; the line to show is the CALL's own. */
				stackFrames.push(this._frame(i + 1, frames[`fn${i}`], i === 0 ? pc : pc - 3, pc));
			}
			this.frameCount = frames.n;
		}
		else {
			const state = await this._state();
			const pc = state ? state.pc : this.lastPc;
			stackFrames = [this._frame(1, null, pc, pc)];
			this.frameCount = 0;
		}

		response.body = { stackFrames, totalFrames: stackFrames.length };
		this.sendResponse(response);
	}

	_frame(id, func, lineAddress, pc) {
		const where = this.map.locationFor(lineAddress);
		const hex = `0x${pc.toString(16).toUpperCase().padStart(4, '0')}`;
		const name = func ? `${func} (${hex})` : hex;

		return where
			? new StackFrame(id, name, new Source(path.basename(where.file), where.file), where.line)
			: new StackFrame(id, name);
	}

	scopesRequest(response, args) {
		const scopes = [];
		const index = (args.frameId || 1) - 1;

		if (this.frameCount > 0) {
			scopes.push(new Scope('Locals', SCOPE_LOCALS + index, false));
			scopes.push(new Scope('Globals', SCOPE_GLOBALS, false));
		}
		scopes.push(new Scope('Registers', SCOPE_REGISTERS, false));
		scopes.push(new Scope('Stack', SCOPE_STACK, false));

		response.body = { scopes };
		this.sendResponse(response);
	}

	async variablesRequest(response, args) {
		const ref = args.variablesReference;
		let variables = [];

		if (ref >= SCOPE_LOCALS) {
			const reply = await this.client.command(`vars ${ref - SCOPE_LOCALS}`);
			if (reply.ok) {
				for (let i = 0; i < reply.n; i++) {
					const value = reply[`val${i}`];
					const offset = reply[`off${i}`];
					variables.push({
						name: reply[`name${i}`],
						value: value === '?' ? 'not there yet' : String(value),
						type: `${reply[`kind${i}`]}, FP${offset >= 0 ? '+' : ''}${offset}`,
						variablesReference: 0
					});
				}
			}
		}
		else if (ref === SCOPE_GLOBALS) {
			const reply = await this.client.command('globals');
			if (reply.ok) {
				for (let i = 0; i < reply.n; i++) {
					const value = reply[`val${i}`];
					const size = reply[`size${i}`];
					variables.push({
						name: reply[`name${i}`],
						value: size > 1 ? `{${String(value).split(',').join(', ')}}` : String(value),
						type: `0x${reply[`addr${i}`].toString(16).toUpperCase().padStart(4, '0')}` +
							(size > 1 ? `, ${size} words` : ''),
						variablesReference: 0
					});
				}
			}
		}
		else if (ref === SCOPE_REGISTERS) {
			const state = await this._state();
			if (state) {
				variables = [
					{ name: 'pc', value: `0x${state.pc.toString(16).toUpperCase().padStart(4, '0')}`, variablesReference: 0 },
					{ name: 'sp', value: String(state.sp), variablesReference: 0 },
					{ name: 'fp', value: String(state.fp), variablesReference: 0 }
				];
			}
		}
		else if (ref === SCOPE_STACK) {
			const reply = await this.client.command('stack 8');
			if (reply.ok) {
				for (let i = 0; i < reply.n; i++)
					variables.push({
						name: i === 0 ? '[0] top' : `[${i}]`,
						value: String(reply[`v${i}`]),
						variablesReference: 0
					});
			}
		}

		response.body = { variables };
		this.sendResponse(response);
	}

	async _state() {
		const reply = await this.client.command('state');
		if (!reply.ok) return null;

		this.lastPc = reply.pc;
		return reply;
	}
}

module.exports = { PvmDebugSession };
