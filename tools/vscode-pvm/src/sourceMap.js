'use strict';

const fs = require('fs');

/*
 * The "0xADDR path:line" map that pvmc writes with -g.
 *
 * Parsed here rather than asked of pvmdbg because VS Code works in (path, line)
 * pairs throughout -- it needs the file to know which editor to highlight and
 * which gutter a breakpoint was clicked in -- so the map has to be on this side
 * regardless. Unlike the wire protocol, the format is one line of text and
 * nothing about it can drift dangerously.
 */
class SourceMap {
	constructor() {
		this.entries = [];
	}

	static baseName(p) {
		const cut = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
		return cut < 0 ? p : p.slice(cut + 1);
	}

	load(path) {
		this.entries = [];

		for (const raw of fs.readFileSync(path, 'utf8').split('\n')) {
			const line = raw.trim();
			if (!line) continue;

			const space = line.indexOf(' ');
			if (space < 0) continue;

			const address = parseInt(line.slice(0, space), 16);
			const rest = line.slice(space + 1);

			/* The last colon, not the first: an absolute Windows path has one of its
			 * own right after the drive letter. */
			const colon = rest.lastIndexOf(':');
			if (colon < 0 || Number.isNaN(address)) continue;

			this.entries.push({
				address,
				file: rest.slice(0, colon),
				line: Number(rest.slice(colon + 1))
			});
		}

		this.entries.sort((a, b) => a.address - b.address);
		return this.entries.length > 0;
	}

	get loaded() {
		return this.entries.length > 0;
	}

	/*
	 * Where an address came from. Addresses between two entries belong to the
	 * earlier one: a source line usually compiles to several instructions and
	 * only the first of them carries a marker.
	 */
	locationFor(address) {
		let best = null;

		for (const entry of this.entries) {
			if (entry.address > address) break;
			best = entry;
		}

		return best;
	}

	/*
	 * The lowest address generated for a line, for putting a breakpoint on it.
	 * Files are matched on their last path component, because the map holds
	 * whatever path pvmcc was handed while VS Code hands over its own absolute
	 * one, and the two need not be spelled the same way.
	 */
	addressFor(file, line) {
		const wanted = SourceMap.baseName(file);
		let best = null;

		for (const entry of this.entries) {
			if (entry.line !== line) continue;
			if (SourceMap.baseName(entry.file) !== wanted) continue;
			if (best === null || entry.address < best) best = entry.address;
		}

		return best;
	}
}

module.exports = { SourceMap };
