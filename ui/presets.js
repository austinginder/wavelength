/* Preset types for the preset pickers (the Sounds and Riffs pages' chooser, the playground): each preset's kind of sound
 * (Bass, Lead, Pad, Bells, FX, Choir...) read from the way its plugin names it, in this order:
 *   a "Type - Name" prefix (Apricot's "Bass - Sure", Serum 2's "BA - 303 Die Treppe", "808 - Diesel"),
 *   a role code before the name (Vaporizer2's and Altitude's "BA Acid", the built-in synth's "PD Warm"),
 *   a category that names a type (Surge XT's "Basses", Altitude's "Factory Hard Leads", Serum 2's "Pad", Vaporizer2's "SY"),
 *   a type word in the name (Wavelength Trance's "Anthem Bass", Vital's "Cinema Bells"),
 * else Other. Plurals and codes fold into one name ("Basses", "BA" and "808" are Bass); a prefix word the table doesn't
 * know ("Kalimba - Soft") is the plugin's own type when two or more presets share it.
 *
 * WLPresets.classify(presets) -> a type per preset; WLPresets.rail(types, hits, current) -> the side column's HTML
 * ("" when the plugin's presets have fewer than two types, so there is nothing to filter by).
 */
(() => {
	const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
	const words = (type, list) => Object.fromEntries(list.split(' ').map(w => [w, type]));
	const ALIASES = {
		...words('Arp', 'arp arps arpeggio arpeggios arpeggiator'),
		...words('Bass', 'bass basses 808 808s sub subs reese'),
		...words('Bells', 'bell bells mallet mallets chime chimes'),
		...words('Brass', 'brass horn horns'),
		...words('Chords', 'chord chords'),
		...words('Drums', 'drum drums drumkit drumkits perc percs percussion kick kicks snare snares clap claps'),
		...words('FX', 'fx sfx effect effects'),
		...words('Guitar', 'guitar guitars'),
		...words('Keys', 'keys piano pianos epiano rhodes keyboard keyboards clav'),
		...words('Lead', 'lead leads hoover hoovers'),
		...words('Organ', 'organ organs'),
		...words('Pad', 'pad pads'),
		...words('Pluck', 'pluck plucks'),
		...words('Sequence', 'seq seqs sequence sequences'),
		...words('Strings', 'string strings'),
		...words('Synth', 'synth synths'),
		...words('Vocal', 'vocal vocals vox voice voices'),
		...words('Choir', 'choir choirs'),
		...words('Drone', 'drone drones'),
		...words('Texture', 'texture textures atmosphere atmospheres ambience ambiences soundscape soundscapes'),
		...words('Stab', 'stab stabs hit hits'),
		...words('FX', 'riser risers'),
		...words('Trance Gate', 'trancegate trancegates gate gates'),
		...words('Orchestral', 'orchestral orchestra'),
		...words('Woodwind', 'woodwind woodwinds wind winds flute flutes sax'),
		...words('Instrument', 'instrument instruments'),
		...words('Loop', 'loop loops'),
		...words('Init', 'init template templates'),
	};
	// the abbreviations plugins put before names or use as categories (Vaporizer2, Altitude, Serum 2, the built-in synth)
	const CODES = { AR: 'Arp', BA: 'Bass', BS: 'Bass', MID: 'Bass', BL: 'Bells', MA: 'Bells', MAL: 'Bells', BR: 'Brass', CH: 'Chords', DR: 'Drums', KIT: 'Drums',
		FX: 'FX', SFX: 'FX', SERUMFX: 'FX', RI: 'FX', GT: 'Guitar', GTR: 'Guitar', KY: 'Keys', KB: 'Keys', EP: 'Keys', PN: 'Keys', PNO: 'Keys', LD: 'Lead', HV: 'Lead',
		OR: 'Organ', PD: 'Pad', PL: 'Pluck', SQ: 'Sequence', ST: 'Strings', STR: 'Strings', SY: 'Synth', MDL: 'Synth', VX: 'Vocal', VO: 'Vocal', VC: 'Vocal',
		TX: 'Texture', AT: 'Texture', SC: 'Texture', HIT: 'Stab', TG: 'Trance Gate', ORCH: 'Orchestral', WW: 'Woodwind', WIND: 'Woodwind', PM: 'Instrument',
		INST: 'Instrument', LOOP: 'Loop', INIT: 'Init' };
	const known = w => ALIASES[w.toLowerCase()] || (/^\w{2,7}$/.test(w) && CODES[w.toUpperCase()]) || '';
	const title = w => w.replace(/\b\w/g, c => c.toUpperCase());

	function classify(presets) {
		const prefixOf = p => /^([^-]{1,24}?)\s+-\s+\S/.exec(String(p.name || '').trim())?.[1].trim() || '';
		// a prefix the table doesn't know counts when two or more of this plugin's presets share it, or when the plugin
		// names its presets that way (five or more with a prefix the table knows: "Harp - Hybrid" beside "Bass - Sure")
		const shared = new Map();
		let convention = 0;
		for (const p of presets) {
			const w = prefixOf(p);
			if (w && known(w)) convention++;
			else if (w && /^[A-Za-z]{3,14}( [A-Za-z]{3,14})?$/.test(w)) shared.set(w.toLowerCase(), (shared.get(w.toLowerCase()) || 0) + 1);
		}
		return presets.map(p => {
			const name = String(p.name || '').trim(), cat = String(p.category || '').trim();
			const pre = prefixOf(p);
			if (pre && known(pre)) return known(pre);
			if (pre && (shared.get(pre.toLowerCase()) >= 2 || (convention >= 5 && /^[A-Za-z]{3,14}$/.test(pre) && shared.has(pre.toLowerCase())))) return title(pre.toLowerCase());
			const code = /^([A-Z]{2,3})\s+\S/.exec(name)?.[1];
			if (code && CODES[code]) return CODES[code];
			if (/^\w{2,7}$/.test(cat) && CODES[cat.toUpperCase()]) return CODES[cat.toUpperCase()];
			const cw = cat.split(/[^A-Za-z0-9]+/).filter(Boolean);
			for (let i = cw.length - 1; i >= 0; i--) if (ALIASES[cw[i].toLowerCase()]) return ALIASES[cw[i].toLowerCase()];
			const nw = name.split(/[^A-Za-z0-9]+/).filter(w => w.length >= 3);
			for (let i = nw.length - 1; i >= 0; i--) if (ALIASES[nw[i].toLowerCase()]) return ALIASES[nw[i].toLowerCase()];
			return 'Other';
		});
	}
	// types: one per preset (classify); hits: the indexes shown now (the search); current: the type picked, '' for all
	function rail(types, hits, current) {
		const all = new Map();
		for (const t of types) all.set(t, 0);
		if ([...all.keys()].filter(t => t !== 'Other').length < 2) return '';
		for (const i of hits) all.set(types[i], all.get(types[i]) + 1);
		const names = [...all.keys()].sort((a, b) => (a === 'Other') - (b === 'Other') || a.localeCompare(b));
		const btn = (t, label, n) => `<button type="button" class="pt-type ${t === current ? 'on' : ''}" data-type="${esc(t)}" ${n ? '' : 'disabled'}>${esc(label)} <b>${n}</b></button>`;
		return btn('', 'All', hits.length) + names.map(t => btn(t, t, all.get(t))).join('');
	}
	window.WLPresets = { classify, rail };
})();
