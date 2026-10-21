'use strict';

/*
 * Drives the debug adapter with real Debug Adapter Protocol traffic, over a
 * pair of pipes, with no VS Code anywhere -- and a real pvmdbg on the other
 * side, so this exercises the whole chain down to the VM.
 *
 * The point is that the JavaScript is verifiable at all. An earlier attempt at
 * this extension went unrun until someone opened VS Code, which is how it came
 * to be thrown away.
 *
 *   node tools/vscode-pvm/test/dapSession.test.js <pvmdbg> <square.bin>
 */
const path = require('path');
const { PassThrough } = require('stream');

const { PvmDebugSession } = require('../src/debugAdapter');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const pvmdbg = process.argv[2] || path.join(repoRoot, 'build', 'debugger', 'pvmdbg.exe');
const program = process.argv[3] || path.join(repoRoot, 'build', 'bytecode', 'square.bin');
const source = path.join(repoRoot, 'example', 'square.hlc');

let checks = 0;
let failed = 0;

function check(condition, what) {
	checks++;
	if (!condition) {
		failed++;
		console.log(`FAIL: ${what}`);
	}
}

/* Speaks DAP the way VS Code would: Content-Length framing over a stream. */
class DapClient {
	constructor(toAdapter, fromAdapter) {
		this.toAdapter = toAdapter;
		this.seq = 1;
		this.pending = new Map();
		this.events = [];
		this.eventWaiters = [];
		this.buffer = Buffer.alloc(0);

		fromAdapter.on('data', (chunk) => this._consume(chunk));
	}

	_consume(chunk) {
		this.buffer = Buffer.concat([this.buffer, chunk]);

		for (;;) {
			const header = this.buffer.indexOf('\r\n\r\n');
			if (header < 0) return;

			const match = /Content-Length: (\d+)/i.exec(this.buffer.slice(0, header).toString());
			if (!match) return;

			const length = Number(match[1]);
			const start = header + 4;
			if (this.buffer.length < start + length) return;

			const message = JSON.parse(this.buffer.slice(start, start + length).toString());
			this.buffer = this.buffer.slice(start + length);
			this._dispatch(message);
		}
	}

	_dispatch(message) {
		if (message.type === 'response') {
			const waiting = this.pending.get(message.request_seq);
			if (waiting) {
				this.pending.delete(message.request_seq);
				waiting(message);
			}
			return;
		}

		if (message.type === 'event') {
			this.events.push(message);
			for (const waiter of this.eventWaiters.splice(0)) waiter();
		}
	}

	request(command, args) {
		const seq = this.seq++;
		const body = JSON.stringify({ seq, type: 'request', command, arguments: args || {} });

		return new Promise((resolve) => {
			this.pending.set(seq, resolve);
			this.toAdapter.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
		});
	}

	/* Events arrive on their own schedule, so this waits for one to show up
	 * rather than assuming it already has. */
	async waitForEvent(name, timeoutMs = 15000) {
		const deadline = Date.now() + timeoutMs;

		for (;;) {
			const found = this.events.findIndex((e) => e.event === name);
			if (found >= 0) return this.events.splice(found, 1)[0];
			if (Date.now() > deadline) throw new Error(`timed out waiting for '${name}'`);

			await new Promise((resolve) => {
				this.eventWaiters.push(resolve);
				setTimeout(resolve, 50);
			});
		}
	}
}

async function main() {
	const toAdapter = new PassThrough();
	const fromAdapter = new PassThrough();

	const session = new PvmDebugSession();
	session.setRunAsServer(true);
	session.start(toAdapter, fromAdapter);

	const dap = new DapClient(toAdapter, fromAdapter);

	/* pathFormat is not optional: the base DebugSession refuses to initialise
	 * without it. VS Code always sends it, so this mirrors what it does. */
	const init = await dap.request('initialize', { adapterID: 'pvm', pathFormat: 'path' });
	check(init.success, 'initialize succeeds');
	check(init.body.supportsConfigurationDoneRequest === true, 'it asks for configurationDone');
	await dap.waitForEvent('initialized');

	const launch = await dap.request('launch', {
		program, pvmdbg, stopOnEntry: true
	});
	check(launch.success, 'launch starts pvmdbg and gets an answer out of it');

	const entry = await dap.waitForEvent('stopped');
	check(entry.body.reason === 'entry', 'the session begins stopped, not running');

	let stack = await dap.request('stackTrace', { threadId: 1 });
	check(stack.success && stack.body.stackFrames.length === 1, 'there is a frame to show');
	check(stack.body.stackFrames[0].line === 11,
		`the first frame is square.hlc:11, got ${stack.body.stackFrames[0].line}`);
	check(/square\.hlc$/.test(stack.body.stackFrames[0].source.path || ''),
		'and it names the source file VS Code should open');

	/* line 8 is "return x * x;", inside square() */
	const bp = await dap.request('setBreakpoints', {
		source: { path: source },
		breakpoints: [{ line: 8 }]
	});
	check(bp.success && bp.body.breakpoints[0].verified,
		'a breakpoint on a source line is accepted and armed');

	await dap.request('configurationDone', {});
	await dap.request('continue', { threadId: 1 });

	const hit = await dap.waitForEvent('stopped');
	check(hit.body.reason === 'breakpoint', `continue stops on the breakpoint, got '${hit.body.reason}'`);

	stack = await dap.request('stackTrace', { threadId: 1 });
	check(stack.body.stackFrames[0].line === 8,
		`and stops on the line asked for, got ${stack.body.stackFrames[0].line}`);
	check(stack.body.stackFrames.length === 2, `the call stack is square and main, got ${stack.body.stackFrames.length} frame(s)`);
	check(/^square/.test(stack.body.stackFrames[0].name), `frame 0 is named after its function, got '${stack.body.stackFrames[0].name}'`);
	check(stack.body.stackFrames[1] && stack.body.stackFrames[1].line === 12,
		'frame 1 is main, on the line that called square');

	const scopes = await dap.request('scopes', { frameId: 1 });
	const scope = (name) => scopes.body.scopes.find((s) => s.name === name);
	check(['Locals', 'Globals', 'Registers', 'Stack'].every((name) => scope(name)),
		`Locals, Globals, Registers and Stack are offered, got ${scopes.body.scopes.map((s) => s.name).join(', ')}`);

	const locals = await dap.request('variables', { variablesReference: scope('Locals').variablesReference });
	const x = locals.body.variables.find((v) => v.name === 'x');
	check(x && x.value === '7', `Locals shows square's parameter x = 7, got ${x ? x.value : 'nothing'}`);

	const globals = await dap.request('variables', { variablesReference: scope('Globals').variablesReference });
	check(globals.body.variables.some((v) => v.name === 'RESULT'), 'Globals shows RESULT');

	const registers = await dap.request('variables', { variablesReference: scope('Registers').variablesReference });
	check(registers.body.variables.some((v) => v.name === 'pc'), 'Registers shows the program counter');

	const stackVars = await dap.request('variables', { variablesReference: scope('Stack').variablesReference });
	check(stackVars.body.variables.length > 0, 'Stack shows what is on the VM stack');

	/* Stepping is by source line: one instruction would usually leave the
	 * highlight exactly where it was, which is not what F10 means. */
	await dap.request('next', { threadId: 1 });
	await dap.waitForEvent('stopped');

	stack = await dap.request('stackTrace', { threadId: 1 });
	check(stack.body.stackFrames[0].line !== 8,
		`a step moves off the line it started on, still on ${stack.body.stackFrames[0].line}`);

	await dap.request('disconnect', {});

	console.log(`${checks} check(s), ${failed} failed`);
	if (failed === 0) console.log('ALL OK');
	process.exit(failed === 0 ? 0 : 1);
}

main().catch((err) => {
	console.log(`FAIL: ${err.message}`);
	process.exit(1);
});
