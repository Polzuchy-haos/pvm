'use strict';

const vscode = require('vscode');

const { PvmDebugSession } = require('./debugAdapter');

/*
 * The adapter runs inside the extension host rather than as its own process:
 * it is a thin translator that spawns pvmdbg anyway, and a second process
 * between VS Code and that one would buy nothing but another thing to crash.
 */
class PvmDebugAdapterFactory {
	createDebugAdapterDescriptor() {
		return new vscode.DebugAdapterInlineImplementation(new PvmDebugSession());
	}
}

function activate(context) {
	context.subscriptions.push(
		vscode.debug.registerDebugAdapterDescriptorFactory('pvm', new PvmDebugAdapterFactory())
	);
}

function deactivate() {}

module.exports = { activate, deactivate };
