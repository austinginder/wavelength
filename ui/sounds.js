/* Sounds: tune a song's instruments by ear before an agent writes its parts. Each track gets an instrument and a
 * preset, and its knobs (the plugin's parameters, in its own modules) turn while it plays: CLAP and VST3 instruments
 * hear a turn at once (a parameter event to the live worker), others on the next note. Everything saves to the
 * song's sounds.json as it changes (only the knobs that differ from the preset, in the plugin's own text where it
 * reads back the same), which every render applies on top of job.json. The brief and a prompt to copy hand the
 * song to an agent. Riffs recorded here (a count-in and click at the song's tempo, then the keys, a MIDI keyboard or
 * the on-screen piano; quantized, reviewed in a piano roll) go to riffs.json for the agent to build the song around.
 *
 * The Riffs page is this page on the riff library (riffs.hpp, the song "_riffs"): instruments instead of tracks, and
 * riffs recorded with no project or name, filed in groups, searched and sorted, then handed to an agent in a prompt
 * that copies them into a new song (`wavelength riffs use`). A song's Riffs card copies them in from the library too.
 *
 * api/sounds lists and changes the sounds; api/knobs loads an instrument's knobs in a __knobs worker, and
 * api/knobs/ask turns values into the plugin's text ("917 Hz") and back.
 *
 * Uses the page's globals: $, esc, state, postJson, loadSongs, go.
 */
(() => {
	const LIB = '_riffs';   // the riff library, as the server names it
	const S = { slug: '', data: null, sel: '', K: null, vals: new Map(), base: new Map(), text: new Map(), q: '', changedOnly: false, open: new Set(), octave: 4, vel: .85, len: 4, chord: 'single', tracks: {},
		recBars: 2, countIn: true, click: true, quant: '1/16', rsort: 'newest', rgroup: -1, ubrief: '' };
	try { Object.assign(S, JSON.parse(localStorage.getItem('wl-sounds') || '{}'), { data: null, K: null, vals: new Map(), base: new Map(), text: new Map(), open: new Set() }); } catch {}
	Object.assign(S, { songSlug: S.slug === LIB ? '' : S.slug, lib: false, rq: '', rsel: new Set() });
	const keep = () => { try { const { songSlug: slug, octave, vel, len, chord, tracks, changedOnly, midi, recBars, countIn, click, quant, rsort, rgroup, ubrief } = S; localStorage.setItem('wl-sounds', JSON.stringify({ slug, octave, vel, len, chord, tracks, changedOnly, midi, recBars, countIn, click, quant, rsort, rgroup, ubrief })); } catch {} };
	const ROLES = ['Lead', 'Bass', 'Pad', 'Pluck', 'Arp', 'Keys', 'Stab', 'Chords', 'Sub', 'FX'];
	let root, keys, built = false, saveTimer = 0, saving = Promise.resolve(), textTimer = 0;
	const pendingText = new Map();

	function build() {
		root = $('#soundsview');
		root.innerHTML = `
		<div class="sd-head">
			<div class="sd-title"><h1 id="sd-h1">Sounds</h1>
				<select id="sd-song" class="song-only" title="The song (project) whose sounds these are"></select>
				<button class="ed-open song-only" id="sd-new">New project</button>
				<button class="ed-open song-only" id="sd-opensong" title="The song's page: arrangement, renders, the agent's work">Song page</button></div>
			<p class="lib-only">Record a riff whenever an idea comes: no project and no name needed. Each one plays on an instrument you tune here and keeps the take as you played it. File them in groups, then tick some (or pick a group) and copy the prompt at the bottom to have an agent write a song from them.</p>
			<p class="song-only">Give each part an instrument and preset, then turn its knobs while you play it. Changes save to the song's <code>sounds.json</code> as you go, and every render keeps them; then hand the song to an agent with the prompt below.</p>
		</div>
		<div class="sd-grid">
			<div class="sd-left">
			<section class="card sd-tracks">
				<h2><span id="sd-trackh">Tracks</span> <button class="ed-open" id="sd-add">Add</button></h2>
				<div class="sd-tlist" id="sd-tlist"></div>
				<label class="sd-tempo">Tempo <input type="number" id="sd-tempo" min="20" max="400" step="1"> BPM</label>
			</section>
			<section class="card sd-riffs" id="sd-riffcard">
				<h2>Riffs <span class="r" id="sd-riffcount"></span><button class="ed-open song-only" id="sd-fromlib" title="Copy riffs from your riff library into this song, with the instruments they play on">From library</button></h2>
				<div class="sd-libbar lib-only">
					<input type="search" id="sd-rq" placeholder="Search names, notes, groups, instruments" autocomplete="off" aria-label="Search riffs">
					<label>Sort <select id="sd-rsort"><option value="newest">Newest first</option><option value="oldest">Oldest first</option><option value="name">Name</option><option value="instrument">Instrument</option><option value="tempo">Tempo</option><option value="length">Length</option></select></label>
				</div>
				<div class="sd-groups lib-only" id="sd-groups"></div>
				<div class="sd-selbar lib-only" id="sd-selbar" hidden></div>
				<div class="sd-rlist" id="sd-rlist"></div>
			</section>
			</div>
			<section class="sd-main">
				<div class="card sd-inst" id="sd-inst"></div>
				<div class="sd-modules" id="sd-modules"></div>
			</section>
		</div>
		<section class="card sd-keys">
			<div class="sd-rec">
				<button class="sd-recbtn" id="sd-rec" title="Record a riff on this track: a count-in, then play (keys, MIDI keyboard or the piano)"><i></i><span>Record</span></button>
				<label title="The riff's tempo: the song's unless you change it here (record slower if that's easier; its notes are in beats either way)">Tempo <input type="number" id="sd-rtempo" min="20" max="400" step="1"> <button type="button" class="sd-tap" id="sd-tap" title="Tap the beat a few times">Tap</button><span class="sd-songtempo" id="sd-songtempo"></span></label>
				<label>Length <select id="sd-bars"><option value="1">1 bar</option><option value="2">2 bars</option><option value="4">4 bars</option><option value="8">8 bars</option><option value="0">free (Stop ends it)</option></select></label>
				<label><input type="checkbox" id="sd-countin"> Count-in</label>
				<label><input type="checkbox" id="sd-click"> Click</label>
				<label>Quantize <select id="sd-quant">${Object.keys(GRID).map(g => `<option value="${g}">${g === 'off' ? 'off (as played)' : g}</option>`).join('')}</select></label>
				<span class="sd-recpos mono" id="sd-recpos"></span>
			</div>
			<div id="sd-keys"></div></section>
		<section class="card sd-brief lib-only" id="sd-use">
			<h2>Write a song from riffs <span class="r" id="sd-usecount"></span></h2>
			<div class="sd-briefgrid">
				<label class="wl-field"><span>Brief: style, mood, length, structure</span><textarea id="sd-ubrief" rows="7" placeholder="Dark techno, 128 BPM. Open with the first riff alone on its bass, then bring the second in for the drop..."></textarea></label>
				<div class="wl-field"><span>Prompt for Claude Code <button class="ed-open" id="sd-ucopy">Copy</button></span><textarea id="sd-uprompt" rows="7" readonly class="mono" placeholder="Tick riffs above, or pick a group, and the prompt appears here."></textarea></div>
			</div>
		</section>
		<section class="card sd-brief song-only">
			<h2>Hand it to an agent <span class="r" id="sd-briefsaved"></span></h2>
			<div class="sd-briefgrid">
				<label class="wl-field"><span>Brief: style, mood, length, structure, references</span><textarea id="sd-brief" rows="7" placeholder="Uplifting trance, 138 BPM, F minor. 6 minutes: long breakdown with the pad alone, then the lead's hook over the full drop..."></textarea></label>
				<div class="wl-field"><span>Prompt for Claude Code <button class="ed-open" id="sd-copy">Copy</button></span><textarea id="sd-prompt" rows="7" readonly class="mono"></textarea></div>
			</div>
		</section>`;
		const on = (sel, ev, fn) => root.querySelector(sel).addEventListener(ev, fn);
		on('#sd-song', 'change', e => open(e.target.value));
		on('#sd-new', 'click', newProject);
		on('#sd-opensong', 'click', () => { if (S.slug) { $('#follow').checked = false; selectSong(S.slug, true); } });
		on('#sd-add', 'click', addTrack);
		on('#sd-tlist', 'click', e => { const b = e.target.closest('[data-t]'); if (b) pick(b.dataset.t); });
		on('#sd-tlist', 'contextmenu', e => { const b = e.target.closest('[data-t]'); if (b) trackMenu(e, b.dataset.t); });
		on('#sd-tempo', 'change', e => change({ op: 'tempo', tempo: +e.target.value }).then(prompt));
		on('#sd-brief', 'input', () => { prompt(); clearTimeout(S.briefTimer); S.briefTimer = setTimeout(saveBrief, 700); });
		on('#sd-copy', 'click', copyPrompt);
		on('#sd-inst', 'click', instClick);
		on('#sd-inst', 'input', instInput);
		const mods = root.querySelector('#sd-modules');
		mods.addEventListener('pointerdown', knobDown);
		mods.addEventListener('dblclick', knobReset);
		mods.addEventListener('wheel', knobWheel, { passive: false });
		mods.addEventListener('keydown', knobKey);
		mods.addEventListener('click', moduleClick);
		mods.addEventListener('change', switchChange);
		keys = WLKeys.create({ page: root, mount: root.querySelector('#sd-keys'), current, settings: S, save: keep, onNote: recNote });
		const bars = root.querySelector('#sd-bars'), cin = root.querySelector('#sd-countin'), clk = root.querySelector('#sd-click'), qn = root.querySelector('#sd-quant');
		bars.value = S.recBars; cin.checked = S.countIn; clk.checked = S.click; qn.value = S.quant;
		bars.addEventListener('change', () => { S.recBars = +bars.value; keep(); });
		cin.addEventListener('change', () => { S.countIn = cin.checked; keep(); });
		clk.addEventListener('change', () => { S.click = clk.checked; keep(); });
		qn.addEventListener('change', () => { S.quant = qn.value; keep(); });
		const rt = root.querySelector('#sd-rtempo');
		rt.addEventListener('change', () => { const v = Math.round(+rt.value); if (v >= 20 && v <= 400) S.recTempo = v === songTempo() ? null : v; rt.value = recTempo(); songTempoNote(); });
		const taps = [];
		on('#sd-tap', 'click', e => {   // tap tempo: the average of the last few intervals
			const t = e.timeStamp;
			if (taps.length && t - taps[taps.length - 1] > 2000) taps.length = 0;
			taps.push(t);
			if (taps.length > 5) taps.shift();
			if (taps.length < 2) return recStatus('Keep tapping…');
			const bpm = Math.round(60000 / ((taps[taps.length - 1] - taps[0]) / (taps.length - 1)));
			if (bpm >= 20 && bpm <= 400) { S.recTempo = bpm === songTempo() ? null : bpm; rt.value = bpm; songTempoNote(); recStatus(`${bpm} BPM from your taps`); }
		});
		on('#sd-rec', 'click', () => R.on ? stopRecording() : record());
		on('#sd-rlist', 'click', e => { const pl = e.target.closest('[data-play]'); if (pl) return playRiff(riffById(pl.dataset.play), pl); const b = e.target.closest('[data-riff]'); if (b) editRiff(riffById(b.dataset.riff)); });
		// the library: search, sort, groups (click to show, right-click to rename or delete, drop riffs on one to file them),
		// ticked riffs (move, delete), and the prompt to write a song from them
		on('#sd-fromlib', 'click', fromLibrary);
		on('#sd-rq', 'input', e => { S.rq = e.target.value; renderRiffs(); });
		const rs = root.querySelector('#sd-rsort');
		rs.value = S.rsort;
		rs.addEventListener('change', () => { S.rsort = rs.value; keep(); renderRiffs(); });
		on('#sd-groups', 'click', groupClick);
		on('#sd-groups', 'contextmenu', groupMenu);
		on('#sd-groups', 'dragover', e => { const c = e.target.closest('[data-gi]'); if (c && +c.dataset.gi !== -1) { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; c.classList.add('drop'); } });
		on('#sd-groups', 'dragleave', e => e.target.closest('[data-gi]')?.classList.remove('drop'));
		on('#sd-groups', 'drop', groupDrop);
		on('#sd-selbar', 'click', selClick);
		on('#sd-selbar', 'change', selChange);
		on('#sd-rlist', 'change', e => { const c = e.target.closest('[data-check]'); if (!c) return; c.checked ? S.rsel.add(c.dataset.check) : S.rsel.delete(c.dataset.check); renderRiffs(); });
		on('#sd-rlist', 'contextmenu', e => { const b = e.target.closest('[data-rid]'); if (b && S.lib) riffMenu(e, b.dataset.rid); });
		on('#sd-rlist', 'dragstart', e => {
			const b = e.target.closest('[data-rid]');
			if (!b) return;
			const ids = S.rsel.has(b.dataset.rid) ? [...S.rsel] : [b.dataset.rid];
			e.dataTransfer.setData('text/x-wl-riffs', JSON.stringify(ids));
			e.dataTransfer.effectAllowed = 'move';
		});
		const ub = root.querySelector('#sd-ubrief');
		ub.value = S.ubrief || '';
		ub.addEventListener('input', () => { S.ubrief = ub.value; keep(); usePrompt(); });
		on('#sd-ucopy', 'click', () => copyText('#sd-uprompt', '#sd-ucopy'));
		built = true;
	}

	/* ---------- the song and its tracks ---------- */
	async function open(slug, lib = false) {
		S.lib = lib;
		root.classList.toggle('lib', lib);
		if (!lib) S.songSlug = slug;
		if (!slug) { S.slug = ''; S.data = null; keep(); render(); return; }
		if (slug !== S.slug) { keys?.releaseAll(); keys?.close(); S.K = null; S.recTempo = null; S.rsel.clear(); }
		S.slug = slug; keep();
		history.replaceState(null, '', lib ? '?view=riffs' : '?view=sounds&song=' + encodeURIComponent(slug));
		await reload();
		const want = S.tracks[slug];
		const names = (S.data?.tracks || []).map(t => t.name);
		await pick(names.includes(want) ? want : names[0] || '');
	}
	async function reload() {
		const slug = S.slug;
		const d = await fetch('api/sounds?song=' + encodeURIComponent(slug)).then(r => r.json()).catch(() => null);
		if (slug !== S.slug) return;
		S.data = d && !d.error ? d : null;
		render();
	}
	function render() {
		const lib = S.lib;
		root.querySelector('#sd-h1').textContent = lib ? 'Riffs' : 'Sounds';
		root.querySelector('#sd-trackh').textContent = lib ? 'Instruments' : 'Tracks';
		// the riff list is the library's main column, and a side card of a song's
		const card = root.querySelector('#sd-riffcard'), home = root.querySelector(lib ? '.sd-main' : '.sd-left');
		if (card.parentElement !== home) lib ? home.prepend(card) : home.append(card);
		const sel = root.querySelector('#sd-song');
		sel.innerHTML = '<option value="">Pick a song…</option>' + state.songs.map(s => `<option value="${esc(s.slug)}">${esc(s.title || s.slug)}</option>`).join('');
		sel.value = S.slug;
		const d = S.data;
		root.querySelector('#sd-tempo').value = d && typeof d.tempo === 'number' ? d.tempo : '';
		if (document.activeElement !== root.querySelector('#sd-rtempo')) root.querySelector('#sd-rtempo').value = recTempo();
		songTempoNote();
		root.querySelector('#sd-tempo').disabled = !d?.hasJob || (d.tempo != null && typeof d.tempo !== 'number');
		root.querySelector('#sd-add').disabled = !d;
		root.querySelector('#sd-opensong').disabled = !S.slug;
		const brief = root.querySelector('#sd-brief');
		if (document.activeElement !== brief) brief.value = d?.brief || '';
		const onIt = name => (d?.riffs || []).filter(r => r.track === name).length;
		root.querySelector('#sd-tlist').innerHTML = !d ? `<div class="empty">${lib ? 'Loading the library…' : 'Pick a song above, or start a new project.'}</div>`
			: d.tracks.map(t => `<button class="pg-item ${t.name === S.sel ? 'sel' : ''}" data-t="${esc(t.name)}">
				<span class="n">${esc(t.name)}${t.custom && !lib ? ' <i class="sd-dot" title="set here (sounds.json)"></i>' : ''}</span>
				<span class="v">${esc(soundLine(t))}</span>
				<span class="f">${lib ? (onIt(t.name) ? onIt(t.name) + ' riff' + (onIt(t.name) === 1 ? '' : 's') : '') : t.notes ? t.notes + ' notes' : t.clips ? 'clips' : ''}</span></button>`).join('')
			+ (d.tracks.length ? '' : `<div class="empty">${lib ? 'Add an instrument to record riffs on: a synth and a preset, tuned by ear.' : 'No tracks yet. Add the first instrument.'}</div>`)
			+ (d.generator && !lib ? '<div class="sd-hint">A script writes this song\'s job.json. Sounds set here stay in sounds.json, which every render applies on top.</div>' : '');
		renderRiffs();
		prompt();
	}
	const plainName = p => String(p || '').replace(/^(clap|vst3|vst2|au):/, '');
	const soundLine = t => [plainName(t.plugin), t.preset ? String(t.preset).split('/').pop() : '', t.params && Object.keys(t.params).length ? Object.keys(t.params).length + ' set' : ''].filter(Boolean).join(' · ');
	const row = name => S.data?.tracks.find(t => t.name === name);

	async function pick(name) {
		S.sel = name; S.tracks[S.slug] = name; keep();
		clearInterval(S.winPoll); S.win = null;
		keys?.releaseAll();
		render();
		await loadKnobs();
		keys?.ensure();
	}

	/* ---------- knobs: the instrument's parameters ---------- */
	async function loadKnobs() {
		const t = row(S.sel), box = root.querySelector('#sd-modules');
		S.K = null;
		renderInst();
		if (!t) { box.innerHTML = ''; return; }
		box.innerHTML = `<div class="card"><div class="empty">Loading ${esc(plainName(t.plugin))}${t.preset ? ' · ' + esc(t.preset) : ''}…</div></div>`;
		const want = S.sel + '|' + soundKey(t);
		let K;
		try { K = await postJson('api/knobs', { song: S.slug, track: S.sel }); } catch (e) { K = { error: e.message }; }
		if (want !== S.sel + '|' + soundKey(row(S.sel))) return;   // another track or sound since
		if (K.error) { box.innerHTML = `<div class="card"><div class="empty">${esc(K.error)}</div></div>`; return; }
		S.K = K; S.vals.clear(); S.base.clear(); S.text.clear();
		S.baseText = new Map(K.params.map(p => [p.id, p.display]));
		for (const p of K.params) { S.vals.set(p.id, p.value); S.base.set(p.id, p.value); S.text.set(p.id, p.display); }
		for (const c of K.current || []) { S.vals.set(c.id, c.value); S.text.set(c.id, c.display); }
		// the key each parameter saves under: its name when that's unique, else Module/Name, else #id
		const count = new Map();
		for (const p of K.params) { const n = p.name.toLowerCase(); count.set(n, (count.get(n) || 0) + 1); const m = (p.module + '/' + p.name).toLowerCase(); count.set(m, (count.get(m) || 0) + 1); }
		for (const p of K.params) p.key = count.get(p.name.toLowerCase()) === 1 ? p.name : p.module && count.get((p.module + '/' + p.name).toLowerCase()) === 1 ? p.module + '/' + p.name : '#' + p.id;
		S.byId = new Map(K.params.map(p => [p.id, p]));
		S.groups = groupParams(K.params);
		if (S.groups.length > 1 && K.params.length > 300) S.open = new Set(); else S.open = new Set(S.groups.map(g => g.name));
		renderInst(); renderModules();
	}
	const soundKey = t => t ? [t.plugin, t.preset || '', JSON.stringify(t.state || '')].join('|') : '';
	// modules: the plugin's own, else the words its parameters' names share ("A Filter 1" for "A Filter 1 Cutoff")
	function groupParams(params) {
		const groups = new Map();
		const add = (name, p, label) => { if (!groups.has(name)) groups.set(name, { name, params: [] }); groups.get(name).params.push({ p, label }); };
		if (params.some(p => p.module)) {
			for (const p of params) {
				const m = p.module || 'Other';
				add(m, p, p.name.toLowerCase().startsWith(m.toLowerCase() + ' ') ? p.name.slice(m.length + 1) : p.name);
			}
		} else {
			const words = params.map(p => p.name.split(/\s+/));
			const prefixCount = new Map();
			for (const w of words) for (let n = 1; n < Math.min(4, w.length); n++) { const k = w.slice(0, n).join(' '); prefixCount.set(k, (prefixCount.get(k) || 0) + 1); }
			params.forEach((p, i) => {
				const w = words[i];
				let best = '';
				for (let n = Math.min(3, w.length - 1); n >= 1; n--) { const k = w.slice(0, n).join(' '); if (prefixCount.get(k) >= 3) { best = k; break; } }
				add(best || 'Parameters', p, best ? p.name.slice(best.length + 1) : p.name);
			});
		}
		return [...groups.values()];
	}
	const changed = id => {
		const p = S.byId.get(id), v = S.vals.get(id), b = S.base.get(id);
		return p.stepped ? Math.round(v) !== Math.round(b) : Math.abs(v - b) > 1e-6 * Math.max(1e-9, Math.abs(p.max - p.min));
	};
	const norm = (p, v) => {
		if (p.curve === 'exp' && p.min > 0) return Math.log(v / p.min) / Math.log(p.max / p.min);
		return p.max !== p.min ? (v - p.min) / (p.max - p.min) : 0;
	};
	const fromNorm = (p, n) => {
		n = Math.min(1, Math.max(0, n));
		let v = p.curve === 'exp' && p.min > 0 ? p.min * Math.pow(p.max / p.min, n) : p.min + n * (p.max - p.min);
		return p.stepped ? Math.round(v) : v;
	};
	// a switch (two steps named off/on), a row of buttons (a few short names), a menu, or a knob
	function kind(p) {
		const l = p.labels;
		if (!p.stepped || !l) return 'knob';
		if (l.length === 2 && (/\bon$/i.test(p.name) || /^(off|on)$/i.test(l[0]) && /^(off|on)$/i.test(l[1]))) return 'switch';
		if (l.length <= 5 && l.every(x => x.length <= 9)) return 'seg';
		return 'menu';
	}
	function knobSvg(p) {
		const v = S.vals.get(p.id), n = norm(p, v), b = norm(p, S.base.get(p.id));
		const a0 = 135, span = 270, r = 17, c = 22;
		const pt = (deg, rad = r) => { const t = deg * Math.PI / 180; return [(c + rad * Math.cos(t)).toFixed(2), (c + rad * Math.sin(t)).toFixed(2)]; };
		const arc = (from, to) => { const [x0, y0] = pt(from), [x1, y1] = pt(to); return `M${x0} ${y0}A${r} ${r} 0 ${to - from > 180 ? 1 : 0} 1 ${x1} ${y1}`; };
		const bipolar = p.min < 0 && p.max > 0 && !p.stepped, zero = bipolar ? a0 + span * norm(p, 0) : a0;
		const at = a0 + span * n, from = Math.min(zero, at), to = Math.max(zero, at);
		const [px, py] = pt(at, 11), [bx, by] = pt(a0 + span * b, r + 4.5);
		return `<svg viewBox="0 0 44 44" aria-hidden="true"><path class="tr" d="${arc(a0, a0 + span)}"/>${to - from > .5 ? `<path class="va" d="${arc(from, to)}"/>` : ''}
			<circle class="cap" cx="22" cy="22" r="12"/><line class="ptr" x1="22" y1="22" x2="${px}" y2="${py}"/><circle class="pre" cx="${bx}" cy="${by}" r="1.6"/></svg>`;
	}
	function control(p, label) {
		const ch = changed(p.id), k = kind(p), title = esc(`${p.name}${p.about ? ': ' + p.about : ''} (preset: ${S.baseText.get(p.id) || ''})`);
		const v = Math.round(S.vals.get(p.id) - Math.min(p.min, p.max));
		if (k === 'switch') return `<label class="sd-sw ${ch ? 'ch' : ''}" data-id="${p.id}" title="${esc(p.name)}"><input type="checkbox" data-sw="${p.id}" ${v ? 'checked' : ''}><span>${esc(label)}</span></label>`;
		if (k === 'seg') return `<div class="sd-seg ${ch ? 'ch' : ''}" data-id="${p.id}"><div class="b">${p.labels.map((l, i) => `<button data-step="${i}" class="${i === v ? 'on' : ''}">${esc(l)}</button>`).join('')}</div><span class="l">${esc(label)}</span></div>`;
		if (k === 'menu') return `<label class="sd-menu ${ch ? 'ch' : ''}" data-id="${p.id}"><select data-menu="${p.id}">${p.labels.map((l, i) => `<option value="${i}" ${i === v ? 'selected' : ''}>${esc(l)}</option>`).join('')}</select><span class="l">${esc(label)}</span></label>`;
		return `<div class="sd-knob ${ch ? 'ch' : ''}" data-id="${p.id}" tabindex="0" title="${title}. Drag, Shift for fine steps, double-click for the preset's value, click the value to type one">${knobSvg(p)}<span class="l">${esc(label)}</span><span class="v">${esc(S.text.get(p.id) ?? '')}</span></div>`;
	}
	function renderModules() {
		const box = root.querySelector('#sd-modules');
		if (!S.K) return;
		const q = S.q.trim().toLowerCase();
		const html = [];
		for (const g of S.groups) {
			let items = g.params.filter(({ p }) => (!q || (g.name + ' ' + p.name).toLowerCase().includes(q)) && (!S.changedOnly || changed(p.id)));
			if (!items.length) continue;
			// the module's own on switch goes in its header ("Osc A On", "Delay On")
			const power = items.find(({ p }) => kind(p) === 'switch' && p.name.toLowerCase() === (g.name + ' on').toLowerCase());
			if (power) items = items.filter(x => x !== power);
			const nChanged = g.params.filter(({ p }) => changed(p.id)).length, isOpen = S.open.has(g.name) || !!q || S.changedOnly;
			const off = power && !Math.round(S.vals.get(power.p.id) - Math.min(power.p.min, power.p.max));
			html.push(`<section class="card sd-mod ${off ? 'off' : ''} ${isOpen ? '' : 'shut'} ${isOpen && items.length > 9 ? 'wide' : ''}" data-mod="${esc(g.name)}">
				<h3>${power ? `<input type="checkbox" class="sd-power" data-sw="${power.p.id}" ${off ? '' : 'checked'} title="${esc(power.p.name)}">` : ''}<span class="t" data-fold>${esc(g.name)}</span>${nChanged ? `<span class="n" title="changed from the preset">${nChanged}</span>` : ''}<span class="c" data-fold>${g.params.length}</span></h3>
				${isOpen ? `<div class="sd-ctls">${items.map(({ p, label }) => control(p, label || p.name)).join('')}</div>` : ''}</section>`);
		}
		box.innerHTML = html.join('') || '<div class="card"><div class="empty">No knob matches.</div></div>';
	}
	function redrawKnob(id) {
		const p = S.byId.get(id), el = root.querySelector(`#sd-modules [data-id="${id}"]`);
		if (!el) return;
		el.classList.toggle('ch', changed(id));
		if (el.classList.contains('sd-knob')) { el.querySelector('svg').outerHTML = knobSvg(p); el.querySelector('.v').textContent = S.text.get(id) ?? ''; }
		const mod = el.closest('.sd-mod'), g = S.groups.find(x => x.name === mod?.dataset.mod);
		if (g) {
			const n = g.params.filter(({ p }) => changed(p.id)).length;
			let badge = mod.querySelector('h3 .n');
			if (!n) badge?.remove();
			else { if (!badge) { badge = document.createElement('span'); badge.className = 'n'; mod.querySelector('h3 .t').after(badge); } badge.textContent = n; }
		}
		renderCount();
	}

	/* ---------- turning them ---------- */
	function set(id, v, { redraw = true, send = true } = {}) {
		const p = S.byId.get(id);
		v = Math.min(Math.max(v, Math.min(p.min, p.max)), Math.max(p.min, p.max));
		if (p.stepped) v = Math.round(v);
		if (v === S.vals.get(id)) return;
		S.vals.set(id, v);
		if (send) keys.param(id, v);   // heard at once when it plays live (not back to the window it came from)
		pendingText.set(id, v);
		if (!textTimer) textTimer = setTimeout(askText, 50);
		if (redraw) redrawKnob(id);
		clearTimeout(saveTimer);
		saveTimer = setTimeout(save, 450);
		status('Unsaved changes…');
	}
	async function askText() {
		textTimer = 0;
		const K = S.K, list = [...pendingText];
		pendingText.clear();
		if (!K || !list.length) return;
		try {
			const r = await postJson('api/knobs/ask', { key: K.key, text: list });
			if (S.K !== K) return;
			list.forEach(([id], i) => { if (r.text?.[i] != null) { S.text.set(id, r.text[i]); redrawKnob(id); } });
		} catch (e) { if (/gone|410/.test(e.message)) loadKnobs(); }
	}
	let drag = null;
	function knobDown(e) {
		const el = e.target.closest('.sd-knob');
		if (!el || e.button !== 0) return;
		e.preventDefault();
		el.focus();
		el.setPointerCapture(e.pointerId);
		const id = +el.dataset.id, p = S.byId.get(id);
		drag = { id, p, y: e.clientY, n: norm(p, S.vals.get(id)) };
		el.classList.add('drag');
		const move = ev => {
			const dy = drag.y - ev.clientY;
			drag.y = ev.clientY;
			drag.n = Math.min(1, Math.max(0, drag.n + dy / (ev.shiftKey ? 1200 : 220)));
			set(id, fromNorm(p, drag.n));
		};
		const up = () => { el.classList.remove('drag'); el.removeEventListener('pointermove', move); drag = null; };
		el.addEventListener('pointermove', move);
		el.addEventListener('pointerup', up, { once: true });
		el.addEventListener('pointercancel', up, { once: true });
	}
	function knobReset(e) {
		const el = e.target.closest('.sd-knob');
		if (el) set(+el.dataset.id, S.base.get(+el.dataset.id));
	}
	function nudge(id, dir, fine) {
		const p = S.byId.get(id);
		if (p.stepped) return set(id, S.vals.get(id) + dir);
		set(id, fromNorm(p, norm(p, S.vals.get(id)) + dir * (fine ? .002 : .01)));
	}
	function knobWheel(e) {
		const el = e.target.closest('.sd-knob');
		if (!el) return;
		e.preventDefault();
		nudge(+el.dataset.id, e.deltaY < 0 ? 1 : -1, e.shiftKey);
	}
	function knobKey(e) {
		const el = e.target.closest('.sd-knob');
		if (!el) return;
		const d = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
		if (d) { e.preventDefault(); e.stopPropagation(); nudge(+el.dataset.id, d, e.shiftKey); }
		else if (e.key === 'Enter') { e.preventDefault(); typeValue(+el.dataset.id); }
	}
	function moduleClick(e) {
		const step = e.target.closest('[data-step]');
		if (step) { const id = +step.closest('[data-id]').dataset.id, p = S.byId.get(id); set(id, Math.min(p.min, p.max) + +step.dataset.step, { redraw: false }); renderModules(); return; }
		const fold = e.target.closest('[data-fold]');
		if (fold && !S.q && !S.changedOnly) { const m = fold.closest('.sd-mod').dataset.mod; S.open.has(m) ? S.open.delete(m) : S.open.add(m); renderModules(); }
		const v = e.target.closest('.sd-knob .v');
		if (v) typeValue(+v.closest('.sd-knob').dataset.id);
	}
	function switchChange(e) {
		const sw = e.target.closest('[data-sw]'), menu = e.target.closest('[data-menu]');
		if (sw) { const id = +sw.dataset.sw, p = S.byId.get(id); set(id, Math.min(p.min, p.max) + (sw.checked ? 1 : 0), { redraw: false }); renderModules(); }
		if (menu) { const id = +menu.dataset.menu, p = S.byId.get(id); set(id, Math.min(p.min, p.max) + +menu.value, { redraw: false }); renderModules(); }
	}
	// type a value in the plugin's own terms ("2 kHz", "-6 dB", "1/16")
	async function typeValue(id) {
		const p = S.byId.get(id), K = S.K;
		const text = await WLUI.prompt({ title: p.name, body: `In ${esc(plainName(K.plugin))}'s own terms, as it shows them (now <b>${esc(S.text.get(id) || '')}</b>).`, label: 'Value', value: S.text.get(id) || '', ok: 'Set' });
		if (text == null || S.K !== K) return;
		try {
			const r = await postJson('api/knobs/ask', { key: K.key, parse: id, text });
			if (r.error) return status(r.error, true);
			set(id, r.value);
		} catch (e) { status(e.message, true); }
	}

	/* ---------- saving: sounds.json ---------- */
	function sound(t, params) {
		const s = { plugin: t.plugin };
		if (t.preset) s.preset = t.preset;
		if (t.state) s.state = t.state;
		if (params && Object.keys(params).length) s.params = params;
		return s;
	}
	function save() {
		clearTimeout(saveTimer);
		const K = S.K, t = row(S.sel), track = S.sel;
		if (!K || !t) return saving;
		saving = saving.then(async () => {
			const ids = K.params.filter(p => changed(p.id)).map(p => p.id);
			let params = {};
			if (ids.length) {   // the plugin's text where it reads back the same ("917 Hz"), else the number
				let stored = [];
				try { stored = (await postJson('api/knobs/ask', { key: K.key, store: ids.map(id => [id, S.vals.get(id)]) })).store || []; } catch {}
				ids.forEach((id, i) => { params[S.byId.get(id).key] = stored[i] ?? +S.vals.get(id).toPrecision(6); });
			}
			await postJson('api/sounds', { song: S.slug, op: 'set', track, sound: sound(t, params) });
			t.params = params; t.custom = true;
			if (track === S.sel) { status(`Saved to sounds.json · ${ids.length} knob${ids.length === 1 ? '' : 's'} set`); render(); }
		}).catch(e => status('Could not save: ' + e.message, true));
		return saving;
	}
	const status = (msg, err) => { const el = root.querySelector('#sd-saved'); if (el) { el.textContent = msg; el.classList.toggle('err', !!err); } };
	function renderCount() {
		const el = root.querySelector('#sd-count');
		if (el && S.K) { const n = S.K.params.filter(p => changed(p.id)).length; el.textContent = `${S.K.params.length} knobs${n ? ` · ${n} changed` : ''}`; }
	}

	/* ---------- the instrument and its preset ---------- */
	function renderInst() {
		const box = root.querySelector('#sd-inst'), t = row(S.sel);
		if (!t) { box.innerHTML = `<div class="empty">${S.data ? 'Pick a track, or add one.' : ''}</div>`; return; }
		const K = S.K, live = /^(clap|vst3)$/.test(K?.format || '');
		box.innerHTML = `
			<h2><span class="sd-name">${esc(t.name)}</span>
				<button class="sd-pick" data-act="inst" title="Choose the instrument">${esc(plainName(t.plugin))}</button>
				<span class="sd-preset"><button data-act="prev" title="The preset before">‹</button><button class="sd-pick" data-act="preset" title="Choose a preset">${esc(t.preset || 'Default sound')}</button><button data-act="next" title="The preset after">›</button></span>
				${live ? `<button class="ed-open" data-act="window" title="Open ${esc(plainName(t.plugin))}'s own window: what you change there comes back here">${S.win ? 'Close window' : 'Plugin window'}</button>` : ''}
				<button class="ed-open" data-act="menu" title="Reset or remove">More</button></h2>
			<div class="sd-bar">
				<input type="text" class="sd-note" data-act="note" value="${esc(t.note || '')}" placeholder="What this part is for (the agent reads it): rolling offbeat bass, the hook in the drops…" maxlength="2000">
			</div>
			<div class="sd-bar">
				<input type="search" class="sd-q" data-act="q" value="${esc(S.q)}" placeholder="Search knobs" autocomplete="off" spellcheck="false">
				<label class="sd-only"><input type="checkbox" data-act="only" ${S.changedOnly ? 'checked' : ''}> Changed only</label>
				<span class="mono sd-count" id="sd-count"></span>
				<span class="sd-saved" id="sd-saved">${K ? (live ? 'Turn a knob while you hold a note: you hear it at once.' : 'Plays note by note: a knob change is heard on the next note.') : ''}</span>
			</div>
			${t.unapplied ? `<div class="sd-hint err">${esc(t.unapplied)}</div>` : ''}${K?.unread?.length ? `<div class="sd-hint err">${esc(K.unread.join('; '))}</div>` : ''}`;
		renderCount();
	}
	let noteTimer = 0;
	function instInput(e) {
		const a = e.target.dataset.act;
		if (a === 'q') { S.q = e.target.value; renderModules(); }
		if (a === 'only') { S.changedOnly = e.target.checked; keep(); renderModules(); }
		if (a === 'note') {
			clearTimeout(noteTimer);
			const track = S.sel, note = e.target.value;
			noteTimer = setTimeout(() => change({ op: 'note', track, note }).then(() => { const t = row(track); if (t) { t.note = note; t.custom = true; } prompt(); }), 600);
		}
	}
	async function instClick(e) {
		const b = e.target.closest('[data-act]');
		if (!b || b.tagName === 'INPUT') return;
		const a = b.dataset.act, t = row(S.sel);
		if (!t) return;
		if (a === 'inst') { const s = await chooseSound({ title: `Instrument for ${t.name}`, plugin: t.plugin, preset: t.preset }); if (s) setSound(s); }
		if (a === 'preset') { const s = await chooseSound({ title: `Preset for ${t.name}`, plugin: t.plugin, preset: t.preset, presetOnly: true }); if (s) setSound(s); }
		if (a === 'prev' || a === 'next') stepPreset(a === 'next' ? 1 : -1);
		if (a === 'menu') trackMenu(e, t.name);
		if (a === 'window') pluginWindow(!S.win);
	}
	// the plugin's own window, opened by its live worker on this computer; what changes there comes back as knobs
	async function pluginWindow(open) {
		const L = await keys.ensure();
		if (!L) return status('This instrument has no window here.', true);
		try { await postJson('api/live/editor', { id: L.id, open }); } catch (e) { return status(e.message, true); }
		clearInterval(S.winPoll);
		S.win = open ? { id: L.id, seq: 0, opened: false } : null;
		renderInst();
		if (!open) return;
		const W = S.win, K = S.K;
		S.winPoll = setInterval(async () => {
			let st;
			try { st = await fetch('api/live/edits?id=' + encodeURIComponent(W.id)).then(r => r.ok ? r.json() : null); } catch { st = null; }
			if (S.win !== W || S.K !== K) return clearInterval(S.winPoll);
			if (!st || st.error) return endWindow(W, st?.error);
			if (st.seq !== W.seq) {
				W.seq = st.seq;
				for (const [k, v] of Object.entries(st.values || {})) {
					const id = +k;
					if (S.byId.has(id) && Math.abs(v - S.vals.get(id)) > 1e-9) set(id, v, { send: false });
				}
			}
			if (st.open) { if (!W.opened) { W.opened = true; status('The plugin\'s window is open: what you change there comes back here and saves.'); } }
			else if (W.opened || st.error) endWindow(W, st.error);
		}, 300);
	}
	function endWindow(W, error) {
		clearInterval(S.winPoll);
		if (S.win !== W) return;
		S.win = null;
		renderInst();
		if (error) status(error, true);
	}
	async function stepPreset(d) {
		const t = row(S.sel);
		const list = await presetsOf(t.plugin);
		if (!list.length) return status('No presets Wavelength can read for this one.', true);
		const names = ['', ...list.map(p => p.name)];
		const i = names.indexOf(t.preset || '');
		setSound({ plugin: t.plugin, preset: names[(Math.max(0, i) + d + names.length) % names.length] });
	}
	// a new instrument or preset: its knobs start from the preset
	async function setSound(s) {
		const t = row(S.sel);
		await save();
		try { await change({ op: 'set', track: t.name, sound: { plugin: s.plugin, ...(s.preset ? { preset: s.preset } : {}) } }); }
		catch (e) { return status(e.message, true); }
		clearInterval(S.winPoll); S.win = null;   // a new sound: its live session (and window) starts again
		await reload();
		await loadKnobs();
		keys.ensure();
	}
	const presetCache = new Map();
	function presetsOf(plugin) {
		if (!presetCache.has(plugin)) presetCache.set(plugin, fetch('api/presets?plugin=' + encodeURIComponent(plugin)).then(r => r.json()).then(r => r.presets || []).catch(() => []));
		return presetCache.get(plugin);
	}
	// the picker: instruments (unless presetOnly) and their presets, searchable; resolves with {plugin, preset} or null
	async function chooseSound({ title, plugin, preset, presetOnly, name }) {
		const groups = presetOnly ? [] : await WLKeys.instruments();
		let spec = plugin || '', chosen = preset || '', presets = [], types = [], ptype = '';
		const specOf = g => g.formats[0].format === 'clap' && g.formats.filter(f => f.format === 'clap').length === 1 ? g.name : g.formats[0].spec;
		const groupOf = s => groups.find(g => g.formats.some(f => f.spec === s) || g.name.toLowerCase() === plainName(s).toLowerCase());
		let out = null;
		await WLUI.modal({ title, cls: 'sd-dlg', body: `
			${name != null ? `<label class="wl-field"><span>Track name</span><input name="name" value="${esc(name)}" list="sd-roles" autocomplete="off" spellcheck="false"><datalist id="sd-roles">${ROLES.map(r => `<option value="${r}">`).join('')}</datalist></label>` : ''}
			<div class="sd-pickgrid ${presetOnly ? 'one' : ''}">
				${presetOnly ? '' : `<div><input type="search" data-q="i" placeholder="Search instruments" autocomplete="off" spellcheck="false"><div class="pg-list" data-l="i"></div></div>`}
				<div><input type="search" data-q="p" placeholder="Search presets" autocomplete="off" spellcheck="false"><div class="pt-body"><div class="pt-rail" data-r="types"></div><div class="pg-list" data-l="p"></div></div></div>
			</div>
			<p class="sd-try" data-r="try">Click a preset, then play it on your computer keys (A to K, W E T Y U O P; Z and X change the octave) or a MIDI keyboard before you pick it. Up and down step through the list.</p>`,
			buttons: [{ value: 'cancel', label: 'Cancel' }, { value: 'ok', label: name != null ? 'Add' : 'Use it', cls: 'primary', submit: api => {
				const n = api.dlg.querySelector('input[name="name"]');
				if (n && !n.value.trim()) return api.error('Give the track a name.');
				if (n && S.data.tracks.some(t => t.name === n.value.trim())) return api.error('There is already a track named ' + n.value.trim() + '.');
				if (!spec) return api.error('Pick an instrument.');
				out = { plugin: spec, preset: chosen, name: n?.value.trim() };
				api.done('ok');
			} }],
			init: api => {
				const d = api.dlg, iq = d.querySelector('[data-q="i"]'), pq = d.querySelector('[data-q="p"]');
				const drawI = () => {
					if (!iq) return;
					const q = iq.value.trim().toLowerCase(), cur = groupOf(spec);
					d.querySelector('[data-l="i"]').innerHTML = groups.map((g, i) => [g, i]).filter(([g]) => !q || (g.name + ' ' + g.vendor).toLowerCase().includes(q)).slice(0, 300)
						.map(([g, i]) => `<button type="button" class="pg-item ${g === cur ? 'sel' : ''}" data-g="${i}"><span class="n">${esc(g.name)}</span><span class="v">${esc(g.vendor || '')}</span><span class="f">${g.formats.map(f => f.format).join(' ')}</span></button>`).join('') || '<div class="empty">No instrument matches.</div>';
				};
				const drawP = () => {
					const q = pq.value.trim().toLowerCase();
					// the search first, then the type picked in the side column (its counts follow the search)
					const found = presets.map((p, i) => i).filter(i => !q || (presets[i].name + ' ' + (presets[i].category || '') + ' ' + types[i]).toLowerCase().includes(q));
					const railHtml = WLPresets.rail(types, found, ptype), railEl = d.querySelector('[data-r="types"]');
					railEl.innerHTML = railHtml; railEl.hidden = !railHtml; railEl.parentElement.classList.toggle('flat', !railHtml);
					const hits = found.filter(i => !ptype || types[i] === ptype).map(i => presets[i]);
					const rowP = (n, sub) => `<button type="button" class="pg-item ${n === chosen ? 'sel' : ''}" data-p="${esc(n)}"><span class="n">${esc(n || 'Default sound')}</span><span class="v">${esc(sub || '')}</span></button>`;
					d.querySelector('[data-l="p"]').innerHTML = !spec ? '<div class="empty">Pick an instrument.</div>'
						: rowP('', 'what the plugin loads with') + hits.slice(0, 400).map(p => rowP(p.name, p.category)).join('') + (hits.length > 400 ? `<div class="empty">${hits.length - 400} more: search to narrow them down.</div>` : '');
				};
				const loadP = async () => { presets = []; types = []; ptype = ''; drawP(); if (!spec) return; const s = spec; const list = await presetsOf(s); if (s === spec) { presets = list; types = WLPresets.classify(list); drawP(); } };
				// trying a sound: the keys play it in the background (keys.js loads it); the line under the lists says how far it got
				let tryTimer = 0, tryN = 0;
				const tryLine = d.querySelector('[data-r="try"]');
				const trySound = () => {
					if (!spec) return;
					S.audition = { plugin: spec, preset: chosen };
					clearTimeout(tryTimer);
					const n = ++tryN, label = plainName(spec) + (chosen ? ' · ' + chosen : ' · its default sound');
					tryLine.textContent = `Loading ${label}…`;
					tryTimer = setTimeout(async () => {   // a quick run through the list loads only where it stops
						keys?.releaseAll();
						const L = await keys?.ensure();
						if (n !== tryN || !d.open) return;
						if (L) { tryLine.textContent = `Live: ${label}. Play it now on A to K or a MIDI keyboard.`; tryLine.classList.remove('err'); }   // else the keys' own line says why (mirrored below)
					}, 220);
				};
				d.dataset.keys = '';   // keys.js plays while this dialog is open
				// what the keys say about the sound being tried (note by note, or why it didn't load) shows here, not behind the dialog
				const keysLine = root.querySelector('#sd-keys [data-r="status"]');
				const mirror = new MutationObserver(() => { if (!S.audition || /^Live: /.test(keysLine.textContent)) return; tryLine.textContent = keysLine.textContent; tryLine.classList.toggle('err', keysLine.classList.contains('err')); });
				if (keysLine) mirror.observe(keysLine, { childList: true, characterData: true, subtree: true, attributes: true });
				d.addEventListener('close', () => mirror.disconnect());
				iq?.addEventListener('input', drawI);
				pq.addEventListener('input', drawP);
				d.querySelector('[data-l="i"]')?.addEventListener('click', e => { const b = e.target.closest('[data-g]'); if (!b) return; spec = specOf(groups[+b.dataset.g]); chosen = ''; drawI(); loadP(); trySound(); });
				const focusChosen = () => { const b = d.querySelector(`[data-l="p"] [data-p="${CSS.escape(chosen)}"]`); b?.focus(); b?.scrollIntoView({ block: 'nearest' }); };
				d.querySelector('[data-l="p"]').addEventListener('click', e => { const b = e.target.closest('[data-p]'); if (!b) return; chosen = b.dataset.p; drawP(); focusChosen(); trySound(); });
				// up and down step through the presets shown, each one tried as it is reached
				d.addEventListener('keydown', e => {
					if (e.target.tagName !== 'INPUT') keys?.keydown(e);   // the dialog keeps keydowns from the page: hand the notes to the keys
					if ((e.key !== 'ArrowDown' && e.key !== 'ArrowUp') || e.target.closest('[data-l="i"]') || (e.target.tagName === 'INPUT' && e.target !== pq)) return;
					const items = [...d.querySelectorAll('[data-l="p"] [data-p]')];
					if (!items.length) return;
					e.preventDefault();
					const at = items.findIndex(b => b.dataset.p === chosen);
					chosen = items[Math.max(0, Math.min(items.length - 1, at < 0 ? 0 : at + (e.key === 'ArrowDown' ? 1 : -1)))].dataset.p;
					drawP(); focusChosen(); trySound();
				});
				d.querySelector('[data-r="types"]').addEventListener('click', e => { const b = e.target.closest('[data-type]'); if (!b) return; ptype = b.dataset.type; drawP(); });
				d.querySelector('[data-l="p"]').addEventListener('dblclick', e => { if (e.target.closest('[data-p]')) d.querySelector('button[value="ok"]').click(); });
				drawI(); loadP();
				(d.querySelector('input[name="name"]') || iq || pq).focus();
			} });
		if (S.audition) {
			S.audition = null;
			keys?.releaseAll();
			if (!out) keys?.ensure();
		}
		return out;
	}

	/* ---------- tracks: add, reset, remove ---------- */
	async function addTrack() {
		if (!S.data) return;
		const used = new Set(S.data.tracks.map(t => t.name));
		const s = await chooseSound({ title: 'Add an instrument', name: ROLES.find(r => !used.has(r)) || '' });
		if (!s) return;
		try { await change({ op: 'add', track: s.name, sound: { plugin: s.plugin, ...(s.preset ? { preset: s.preset } : {}) }, tempo: 128 }); }
		catch (e) { return WLUI.confirm({ title: 'Could not add it', body: esc(e.message) }); }
		await reload();
		await pick(s.name);
	}
	function trackMenu(e, name) {
		const t = row(name);
		WLUI.menu(e, [
			{ label: 'Reset to the job\'s sound', hint: 'forget what was set here', disabled: !t?.custom || !t.inJob, run: async () => {
				if (!await WLUI.confirm({ title: 'Reset ' + name + '?', body: `Its instrument, preset and knobs go back to what <code>job.json</code> gives it, and its entry leaves <code>sounds.json</code>.`, ok: 'Reset' })) return;
				await change({ op: 'reset', track: name }); await reload(); if (name === S.sel) { await loadKnobs(); keys.close(); keys.ensure(); }
			} },
			{ label: 'Remove track', danger: true, disabled: !!t?.notes || !!t?.clips, hint: t?.notes ? 'it has notes' : '', run: async () => {
				if (!await WLUI.confirm({ title: 'Remove ' + name + '?', body: 'The track leaves the song (it has no notes yet).', ok: 'Remove', danger: true })) return;
				try { await change({ op: 'remove', track: name }); } catch (err) { return status(err.message, true); }
				await reload(); if (name === S.sel) pick(S.data?.tracks[0]?.name || '');
			} },
		]);
	}
	async function change(body) {
		const r = await postJson('api/sounds', { song: S.slug, ...body });
		if (r.error) throw new Error(r.error);
		return r;
	}

	/* ---------- a new project ---------- */
	async function newProject() {
		const title = await WLUI.prompt({ title: 'New project', body: 'A new song folder. Add its instruments here, then hand it to an agent to write the parts.', label: 'Name', value: '', ok: 'Create',
			check: v => v ? '' : 'Give it a name.' });
		if (!title) return;
		let r;
		try { r = await postJson('api/song/create', { title }); } catch (e) { return WLUI.confirm({ title: 'Could not create it', body: esc(e.message) }); }
		await loadSongs();
		await open(r.song);
		addTrack();
	}

	/* ---------- the hand-off ---------- */
	async function saveBrief() {
		const text = root.querySelector('#sd-brief').value;
		try { await change({ op: 'brief', text }); root.querySelector('#sd-briefsaved').textContent = 'saved to brief.md'; if (S.data) S.data.brief = text; }
		catch (e) { root.querySelector('#sd-briefsaved').textContent = e.message; }
	}
	function prompt() {
		const d = S.data, box = root?.querySelector('#sd-prompt');
		if (!box) return;
		if (!d) { box.value = ''; return; }
		const brief = root.querySelector('#sd-brief').value.trim();
		const lines = d.tracks.map(t => `- ${t.name}: ${plainName(t.plugin)}${t.preset ? `, preset "${t.preset}"` : ''}${t.params && Object.keys(t.params).length ? `, ${Object.keys(t.params).length} knob${Object.keys(t.params).length === 1 ? '' : 's'} set` : ''}${t.note ? `. ${t.note}` : ''}`);
		box.value = `/wavelength Write a song in ${d.path}.

I chose and tuned its instruments by ear: sounds.json in that folder gives each track its plugin, preset and knob settings, and every render applies it on top of job.json (docs/job-format.md, "Sounds"). Treat them as the starting point: write a track with each name and leave its sound to sounds.json. Change a sound only if the song really needs it (edit sounds.json, which wins over job.json) and tell me what you changed and why. Add drums, effects, gain, pans and automation as the song needs.

Tracks:
${lines.join('\n') || '- (none yet)'}
${riffLines(d)}${typeof d.tempo === 'number' ? `\nTempo: ${d.tempo} BPM.\n` : ''}${brief ? `\nBrief (also in brief.md):\n${brief}\n` : ''}`;
	}
	const copyPrompt = () => copyText('#sd-prompt', '#sd-copy');
	async function copyText(boxSel, btnSel) {
		const box = root.querySelector(boxSel), b = root.querySelector(btnSel);
		if (!box.value) return;
		try { await navigator.clipboard.writeText(box.value); }
		catch { box.select(); document.execCommand('copy'); }
		b.textContent = 'Copied'; setTimeout(() => b.textContent = 'Copy', 1500);
	}

	/* ---------- riffs: record, review, keep (riffs.json) ---------- */
	const GRID = { off: 0, '1/16': .25, '1/8': .5, '1/16T': 1 / 6, '1/8T': 1 / 3, '1/4': 1, '1/32': .125 };
	const NOTE_NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B'];
	const keyNum = k => {   // 60, "F#4", "Eb3"
		if (typeof k === 'number') return k;
		const m = /^([A-Ga-g])([#b]?)(-?\d+)$/.exec(String(k).trim());
		if (!m) return 60;
		return 12 * (+m[3] + 1) + { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 }[m[1].toLowerCase()] + (m[2] === '#' ? 1 : m[2] === 'b' ? -1 : 0);
	};
	const keyStr = k => NOTE_NAMES[k % 12] + (Math.floor(k / 12) - 1);
	const R = { on: false, ctx: null, notes: [], open: new Map() };
	const meter = () => S.data?.timeSignature?.length === 2 ? S.data.timeSignature : [4, 4];
	const songTempo = () => typeof S.data?.tempo === 'number' ? S.data.tempo : 120;
	const recTempo = () => S.recTempo || songTempo();
	const songTempoNote = () => { const el = root.querySelector('#sd-songtempo'); if (el) el.textContent = S.recTempo ? `song: ${songTempo()}` : ''; };
	const riffById = id => (S.data?.riffs || []).find(r => r.id === id);
	function audio() {
		R.ctx ??= new AudioContext({ latencyHint: 'interactive' });
		if (R.ctx.state === 'suspended') R.ctx.resume();
		return R.ctx;
	}
	// when a moment on the audio clock is heard, on the clock key presses carry (performance.now())
	function heard(ctxTime) {
		const c = R.ctx, ts = c.getOutputTimestamp?.();
		if (ts && ts.performanceTime > 0) return ts.performanceTime + (ctxTime - ts.contextTime) * 1000;
		return performance.now() + (ctxTime - c.currentTime + (c.outputLatency || c.baseLatency || 0)) * 1000;
	}
	function blip(at, accent) {
		const c = R.ctx, o = c.createOscillator(), g = c.createGain();
		o.frequency.value = accent ? 1760 : 1175;
		g.gain.setValueAtTime(0, at);
		g.gain.linearRampToValueAtTime(accent ? .32 : .2, at + .002);
		g.gain.exponentialRampToValueAtTime(.0001, at + .06);
		o.connect(g).connect(c.destination);
		o.start(at); o.stop(at + .07);
	}
	async function record() {
		const t = row(S.sel);
		if (!t) return recStatus('Pick a track to record on.');
		keys.releaseAll();
		recStatus('Loading the instrument…');
		await keys.ensure();   // so the first notes sound
		const c = audio(), [num, den] = meter(), bpb = num * 4 / den, spb = 60 / recTempo();
		const count = S.countIn ? Math.round(bpb) : 0, len = S.recBars ? S.recBars * bpb : 0;
		Object.assign(R, { on: true, track: t.name, notes: [], open: new Map(), bpb, spb, tempo: recTempo(), meter: [num, den], len, count,
			start: c.currentTime + .25, next: 0 });
		R.zero = R.start + count * spb;   // beat 0 on the audio clock
		const tick = () => {   // the click, scheduled a little ahead
			while (R.on && R.start + R.next * spb < c.currentTime + .3 && (!len || R.next < count + len)) {
				if (S.click || R.next < count) blip(R.start + R.next * spb, (R.next - count) % Math.round(bpb) === 0);
				R.next++;
			}
		};
		tick();
		R.timer = setInterval(tick, 40);
		if (len) R.stopper = setTimeout(() => R.on && stopRecording(), (R.zero + len * spb - c.currentTime) * 1000 + 30);
		const btn = root.querySelector('#sd-rec');
		btn.classList.add('on'); btn.querySelector('span').textContent = 'Stop';
		const show = () => {
			if (!R.on) return;
			const b = (c.currentTime - R.zero) / spb;
			recStatus(b < 0 ? `Count-in ${Math.min(count, Math.floor(b + count) + 1)} / ${count}` : `Recording ${t.name} · bar ${Math.floor(b / bpb) + 1}.${Math.floor(b % bpb) + 1}${len ? ' of ' + (len / bpb) : ''} · ${R.notes.length + R.open.size} notes`);
			requestAnimationFrame(show);
		};
		show();
	}
	const beatAt = tms => (tms - heard(R.zero)) / (R.spb * 1000);
	function recNote(e) {
		if (!R.on) return;
		const b = beatAt(e.t);
		if (e.on) {
			if (b < -.5 || (R.len && b >= R.len)) return;   // noodling in the count-in, or past the end
			R.open.set(e.key, { beat: Math.max(b, -.5), vel: e.vel });
		} else {
			const o = R.open.get(e.key);
			if (!o) return;
			R.open.delete(e.key);
			R.notes.push({ beat: o.beat, dur: Math.max(.02, b - o.beat), key: e.key, vel: o.vel });
		}
	}
	function stopRecording() {
		if (!R.on) return;
		const end = R.len || Math.max(R.bpb, Math.ceil(beatAt(performance.now()) / R.bpb - .05) * R.bpb);   // free: up to the bar
		for (const [key, o] of R.open) R.notes.push({ beat: o.beat, dur: Math.max(.02, end - o.beat), key, vel: o.vel });
		R.on = false; R.open.clear();
		clearInterval(R.timer); clearTimeout(R.stopper);
		const btn = root.querySelector('#sd-rec');
		btn.classList.remove('on'); btn.querySelector('span').textContent = 'Record';
		// as played: from the first downbeat, cut at the end
		const played = R.notes.map(n => {
			const b = Math.max(0, n.beat), e = Math.min(end, n.beat + n.dur);
			return { beat: b, dur: e - b, key: n.key, vel: n.vel };
		}).filter(n => n.dur > .01 && n.beat < end).sort((a, b) => a.beat - b.beat || a.key - b.key);
		if (!played.length) return recStatus('Nothing was played. Press Record and play after the count-in.');
		recStatus('');
		riffEditor({ track: R.track, tempo: R.tempo, timeSignature: R.meter, bars: +(end / R.bpb).toFixed(3), played, quantize: S.quant });
	}
	const recStatus = msg => { const el = root.querySelector('#sd-recpos'); if (el) el.textContent = msg; };
	// quantize: starts to the grid, ends to the grid (a step at least); two notes landing on one key at one beat keep the longer
	function quantize(played, q, beats) {
		const g = GRID[q] || 0, out = new Map();
		for (const n of played) {
			let b = n.beat, e = n.beat + n.dur;
			if (g) { b = Math.round(b / g) * g; e = Math.max(b + g, Math.round(e / g) * g); }
			if (b >= beats) continue;
			e = Math.min(e, beats);
			const k = b.toFixed(4) + '|' + n.key, x = { beat: +b.toFixed(4), dur: +(e - b).toFixed(4), key: n.key, vel: n.vel };
			if (!out.has(k) || out.get(k).dur < x.dur) out.set(k, x);
		}
		return [...out.values()].sort((a, b) => a.beat - b.beat || a.key - b.key);
	}
	// play notes (beats) through the track's own sound, looped: one render (api/play), then the audio repeats
	let playing = null;
	async function playNotes(track, notes, tempo, beats, btn) {
		stopPlaying();
		const spb = 60 / tempo, me = playing = { btn };
		if (btn) btn.classList.add('on');
		try {
			const r = await fetch('api/play', { method: 'POST', headers: postHeaders(), body: JSON.stringify({ song: S.slug, track,
				notes: notes.map(n => ({ key: n.key, vel: n.vel ?? .8, start: n.beat * spb, dur: n.dur * spb })), tail: 1.5, length: beats * spb + .05 }) });
			if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || 'the server answered ' + r.status); }
			const c = audio(), buf = await c.decodeAudioData(await r.arrayBuffer());
			if (playing !== me) return;
			const src = c.createBufferSource();
			src.buffer = buf; src.loop = true; src.loopEnd = Math.min(buf.duration, beats * spb);   // the notes' ring past the loop is cut
			src.connect(c.destination); src.start();
			Object.assign(me, { src, t0: c.currentTime, len: src.loopEnd, beats });
		} catch (e) { if (playing === me) { stopPlaying(); recStatus('Could not play it: ' + e.message); } }
	}
	function stopPlaying() {
		if (!playing) return;
		try { playing.src?.stop(); } catch {}
		playing.btn?.classList.remove('on');
		playing = null;
	}
	const riffNotes = r => (r.notes || []).map(n => ({ ...n, key: keyNum(n.key) }));
	function playRiff(r, btn) {
		if (!r) return;
		if (playing?.btn === btn) return stopPlaying();
		const [num, den] = r.timeSignature || [4, 4];
		playNotes(r.track, riffNotes(r), r.tempo, r.bars * num * 4 / den, btn);
	}
	// one note through the track's sound, to hear a pitch while editing (the newest request wins)
	let audReq = null, audBusy = false;
	async function audition(track, key, vel) {
		audReq = { track, key, vel };
		if (audBusy) return;
		audBusy = true;
		while (audReq) {
			const q = audReq;
			audReq = null;
			try {
				const r = await fetch('api/play', { method: 'POST', headers: postHeaders(), body: JSON.stringify({ song: S.slug, track: q.track, notes: [{ key: q.key, vel: q.vel, start: 0, dur: .3 }], tail: .4 }) });
				if (!r.ok) continue;
				const c = audio(), buf = await c.decodeAudioData(await r.arrayBuffer()), src = c.createBufferSource();
				src.buffer = buf; src.connect(c.destination); src.start();
			} catch {}
		}
		audBusy = false;
	}
	// the riff editor: a take (or a kept riff) in a piano roll to clean up, heard looped, named and kept
	async function riffEditor(take, existing) {
		const [num, den] = take.timeSignature, bpb = num * 4 / den;
		let bars = take.bars, beats = bars * bpb, tempo = take.tempo, uid = 0;
		const mk = n => ({ id: ++uid, beat: n.beat, dur: n.dur, key: n.key, vel: n.vel ?? .8 });
		let notes = (existing ? riffNotes(existing) : quantize(take.played, take.quantize, beats)).map(mk);
		let snapName = take.quantize && take.quantize !== 'off' ? take.quantize : '1/16';
		const sel = new Set(), hist = [], fut = [];
		let lastLen = GRID[snapName] || .25, lastVel = .8, lo = 48, hi = 72, dirty = !existing;
		const ROW = 12;
		const state = () => JSON.stringify({ notes, bars });
		const restore = st => { const o = JSON.parse(st); notes = o.notes; bars = o.bars; beats = bars * bpb; uid = Math.max(0, ...notes.map(n => n.id)); sel.clear(); };
		const remember = () => { hist.push(state()); if (hist.length > 200) hist.shift(); fut.length = 0; dirty = true; };
		const g = () => GRID[snapName] || 0;
		const snapD = d => g() ? Math.round(d / g()) * g() : d;
		const floorB = b => g() ? Math.floor(b / g() + 1e-6) * g() : b;
		let d, svg, replayTimer = 0, raf = 0;
		const range = () => {
			const ks = notes.map(n => n.key), mn = ks.length ? Math.min(...ks) : 60, mx = ks.length ? Math.max(...ks) : 72;
			lo = Math.max(0, mn - 5); hi = Math.min(127, mx + 5);
			if (hi - lo < 24) { const mid = Math.round((lo + hi) / 2); lo = Math.max(0, mid - 12); hi = Math.min(127, lo + 24); }
		};
		function draw() {
			const W = 1000, rows = hi - lo + 1, H = rows * ROW, x = b => b / beats * W, y = k => (hi - k) * ROW;
			let bg = '';
			for (let k = lo; k <= hi; k++) bg += `<rect class="${[1, 3, 6, 8, 10].includes(k % 12) ? 'bk' : 'wk'}" x="0" y="${y(k)}" width="${W}" height="${ROW}"/>`;
			const step = g() || .25;
			for (let b = 0; b <= beats + 1e-9; b += step) {
				const bar = Math.abs(b / bpb - Math.round(b / bpb)) < 1e-6, beat = Math.abs(b - Math.round(b)) < 1e-6;
				bg += `<line class="${bar ? 'bar' : beat ? 'bt' : 'sub'}" x1="${x(b).toFixed(2)}" x2="${x(b).toFixed(2)}" y1="0" y2="${H}"/>`;
			}
			const ghost = take.played && d.querySelector('[data-r="ghost"]')?.checked ? take.played.map(n => `<rect class="gh" x="${x(n.beat).toFixed(2)}" y="${y(n.key) + 1}" width="${Math.max(1.5, x(n.dur)).toFixed(2)}" height="${ROW - 2}" rx="1.5"/>`).join('') : '';
			const ns = notes.map(n => `<rect class="nt ${sel.has(n.id) ? 'sel' : ''}" data-id="${n.id}" x="${x(n.beat).toFixed(2)}" y="${y(n.key) + 1}" width="${Math.max(2, x(n.dur) - .8).toFixed(2)}" height="${ROW - 2}" rx="2" style="fill-opacity:${(.4 + .6 * n.vel).toFixed(2)}"><title>${keyStr(n.key)} · beat ${(n.beat + 1).toFixed(2)} · ${n.dur.toFixed(2)} beats · velocity ${Math.round(n.vel * 127)}</title></rect>`).join('');
			svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
			svg.style.height = H + 'px';
			svg.innerHTML = bg + ghost + ns + `<line class="ph" data-r="ph" x1="-10" x2="-10" y1="0" y2="${H}"/>`;
			let labels = '';
			for (let k = lo; k <= hi; k++) if (k % 12 === 0 || k === lo || k === hi) labels += `<span style="top:${y(k)}px">${keyStr(k)}</span>`;
			d.querySelector('[data-r="keys"]').innerHTML = labels;
			d.querySelector('[data-r="keys"]').style.height = H + 'px';
			d.querySelector('[data-r="count"]').textContent = `${notes.length} note${notes.length === 1 ? '' : 's'}${sel.size ? ` · ${sel.size} selected` : ''}`;
			d.querySelector('[data-r="undo"]').disabled = !hist.length;
			d.querySelector('[data-r="redo"]').disabled = !fut.length;
		}
		const changed = () => { draw(); if (playing?.editor) { clearTimeout(replayTimer); replayTimer = setTimeout(() => play(true), 350); } };
		function play(again) {
			const btn = d.querySelector('[data-r="play"]');
			if (!again && playing?.btn === btn) return stopPlaying();
			if (!notes.length) return;
			playNotes(take.track, notes, tempo, beats, btn).then(() => { if (playing?.btn === btn) playing.editor = true; });
			if (playing) playing.editor = true;
			cancelAnimationFrame(raf);
			const tick = () => {
				const ph = d.querySelector('[data-r="ph"]');
				if (!d.open) return;
				if (ph && playing?.btn === btn && playing.src) {
					const x = ((R.ctx.currentTime - playing.t0) % playing.len) / playing.len * Math.min(1, playing.len / (beats * 60 / tempo)) * 1000;
					ph.setAttribute('x1', x); ph.setAttribute('x2', x);
				} else if (ph) { ph.setAttribute('x1', -10); ph.setAttribute('x2', -10); }
				raf = requestAnimationFrame(tick);
			};
			tick();
		}
		// pointer: on a note, drag moves it (and the other selected ones); near its end, resizes; on empty space, adds one
		function pos(e) {
			const r = svg.getBoundingClientRect();
			return { beat: Math.min(beats, Math.max(0, (e.clientX - r.left) / r.width * beats)), key: Math.min(127, Math.max(0, hi - Math.floor((e.clientY - r.top) / ROW))), px: r.width / beats };
		}
		function down(e) {
			if (e.button !== 0) return;
			e.preventDefault();
			d.querySelector('[data-r="rollbox"]').focus();
			const p = pos(e), id = +(e.target.closest('[data-id]')?.dataset.id || 0);
			let n = notes.find(x => x.id === id), mode;
			if (n) {
				if (e.shiftKey) { sel.has(n.id) ? sel.delete(n.id) : sel.add(n.id); draw(); return; }
				if (!sel.has(n.id)) { sel.clear(); sel.add(n.id); }
				mode = (n.beat + n.dur - p.beat) * p.px < 8 ? 'resize' : 'move';
				audition(take.track, n.key, n.vel);
			} else {
				remember();
				n = mk({ beat: Math.min(floorB(p.beat), beats - (g() || .05)), dur: lastLen, key: p.key, vel: lastVel });
				n.dur = Math.min(n.dur, beats - n.beat);
				notes.push(n);
				sel.clear(); sel.add(n.id);
				mode = 'resize';
				audition(take.track, n.key, n.vel);
			}
			// moves are measured from where the drag started (the dialog may shift as its text changes)
			const start = { x: e.clientX, y: e.clientY, px: p.px }, orig = new Map(notes.filter(x => sel.has(x.id)).map(x => [x.id, { beat: x.beat, key: x.key, dur: x.dur }]));
			let moved = mode === 'resize' && !id, lastKey = n.key;
			svg.setPointerCapture(e.pointerId);
			const move = ev => {
				const dBeat = (ev.clientX - start.x) / start.px, db = snapD(dBeat), dk = -Math.round((ev.clientY - start.y) / ROW);
				if (!moved && (Math.abs(ev.clientX - start.x) > 3 || dk)) { if (id) remember(); moved = true; }
				if (!moved) return;
				if (mode === 'move') {
					const minB = Math.min(...[...orig.values()].map(o => o.beat)), maxE = Math.max(...[...orig.values()].map(o => o.beat + o.dur));
					const shift = Math.min(Math.max(db, -minB), beats - maxE);
					for (const x of notes) if (orig.has(x.id)) { const o = orig.get(x.id); x.beat = +(o.beat + shift).toFixed(4); x.key = Math.min(127, Math.max(0, o.key + dk)); }
					if (n.key !== lastKey) { lastKey = n.key; audition(take.track, n.key, n.vel); }
				} else {
					const o = orig.get(n.id), end = g() ? Math.round((o.beat + o.dur + dBeat) / g()) * g() : o.beat + o.dur + dBeat;
					n.dur = +Math.min(beats - n.beat, Math.max(g() || .05, end - n.beat)).toFixed(4);
				}
				draw();
			};
			const up = () => {
				svg.removeEventListener('pointermove', move);
				if (mode === 'resize') lastLen = n.dur;
				lastVel = n.vel;
				if (moved) { range(); changed(); } else draw();
			};
			svg.addEventListener('pointermove', move);
			svg.addEventListener('pointerup', up, { once: true });
			svg.addEventListener('pointercancel', up, { once: true });
		}
		const chosen = () => sel.size ? notes.filter(x => sel.has(x.id)) : notes;
		function key(e) {
			const mod = e.metaKey || e.ctrlKey;
			if (mod && e.key.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
			if (mod && e.key.toLowerCase() === 'a') { e.preventDefault(); notes.forEach(x => sel.add(x.id)); draw(); return; }
			if (e.key === 'Escape' && sel.size) { e.preventDefault(); sel.clear(); draw(); return; }
			if ((e.key === 'Delete' || e.key === 'Backspace') && sel.size) { e.preventDefault(); remember(); notes = notes.filter(x => !sel.has(x.id)); sel.clear(); changed(); return; }
			if (e.key === ' ') { e.preventDefault(); play(); return; }
			const arrows = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, 1], ArrowDown: [0, -1] }[e.key];
			if (!arrows || !sel.size) return;
			e.preventDefault();
			remember();
			const step = g() || .25, list = chosen();
			const db = arrows[0] * step, dk = arrows[1] * (e.shiftKey ? 12 : 1);
			if (list.every(x => x.beat + db >= -1e-9 && x.beat + x.dur + db <= beats + 1e-9 && x.key + dk >= 0 && x.key + dk <= 127))
				for (const x of list) { x.beat = +(x.beat + db).toFixed(4); x.key += dk; }
			if (dk && list.length) audition(take.track, list[0].key, list[0].vel);
			range(); changed();
		}
		function wheel(e) {
			const id = +(e.target.closest('[data-id]')?.dataset.id || 0);
			const list = id && !sel.has(id) ? notes.filter(x => x.id === id) : sel.size ? chosen() : [];
			if (!list.length) return;
			e.preventDefault();
			if (!wheel.at || performance.now() - wheel.at > 600) remember();
			wheel.at = performance.now();
			for (const x of list) x.vel = Math.round(Math.min(1, Math.max(.05, x.vel + (e.deltaY < 0 ? .04 : -.04))) * 100) / 100;
			lastVel = list[0].vel;
			changed();
		}
		function undo() { if (!hist.length) return; fut.push(state()); restore(hist.pop()); range(); changed(); }
		function redo() { if (!fut.length) return; hist.push(state()); restore(fut.pop()); range(); changed(); }
		function act(a) {
			if (a === 'quantize') {   // starts and ends to the snap grid
				if (!g()) return;
				remember();
				for (const x of chosen()) { const b = Math.min(Math.round(x.beat / g()) * g(), beats - g()), e = Math.max(b + g(), Math.round((x.beat + x.dur) / g()) * g()); x.beat = +b.toFixed(4); x.dur = +(Math.min(e, beats) - b).toFixed(4); }
				changed();
			}
			if (a === 'played' && take.played) { remember(); notes = take.played.filter(n => n.beat < beats).map(n => mk({ ...n, dur: Math.min(n.dur, beats - n.beat) })); sel.clear(); range(); changed(); }
			if (a === 'delete' && sel.size) { remember(); notes = notes.filter(x => !sel.has(x.id)); sel.clear(); changed(); }
			if (a === 'all') { notes.forEach(x => sel.add(x.id)); draw(); }
			if (a === 'undo') undo();
			if (a === 'redo') redo();
			if (a === 'play') play();
		}
		const barOpts = [...new Set([1, 2, 3, 4, 6, 8, 12, 16, bars])].sort((a, b) => a - b);
		const n = (S.data?.riffs?.length || 0) + 1;
		const groups = S.data?.groups || [], groupNow = existing ? existing.group || '' : S.rgroup >= 0 ? groups[S.rgroup] || '' : '';
		const res = await WLUI.modal({ title: existing ? existing.name : `New riff on ${take.track}`, cls: 'sd-dlg sd-riffdlg' + (S.lib ? ' lib' : ''), body: `
			<div class="sd-takebar">
				<button type="button" class="ed-btn" data-a="play" data-r="play" title="Space">Play (loops)</button>
				<label>Snap <select data-r="snap">${Object.keys(GRID).map(k => `<option value="${k}" ${k === snapName ? 'selected' : ''}>${k === 'off' ? 'off' : k}</option>`).join('')}</select></label>
				<button type="button" class="ed-btn" data-a="quantize" title="Starts and ends of the selected notes (or all) to the snap grid">Quantize</button>
				${take.played ? `<button type="button" class="ed-btn" data-a="played" title="Back to the notes as you played them">As played</button><label title="The take as you played it, faint behind the notes"><input type="checkbox" data-r="ghost"> Show the take</label>` : ''}
				<label>Bars <select data-r="bars">${barOpts.map(b => `<option value="${b}" ${b === bars ? 'selected' : ''}>${b}</option>`).join('')}</select></label>
				<label>Tempo <input type="number" data-r="tempo" min="20" max="400" step="1" value="${tempo}"></label>
				<span class="sp"></span>
				<button type="button" class="ed-btn" data-a="undo" data-r="undo" title="Cmd-Z">Undo</button>
				<button type="button" class="ed-btn" data-a="redo" data-r="redo" title="Shift-Cmd-Z">Redo</button>
			</div>
			<div class="sd-rollwrap"><div class="sd-rollkeys mono" data-r="keys"></div>
				<div class="sd-rollbox" data-r="rollbox" tabindex="0"><svg class="sd-roll" preserveAspectRatio="none" data-r="svg"></svg></div></div>
			<p class="sd-rollhelp">${esc(take.track)} · ${num}/${den} · <span data-r="count"></span> · click to add a note (drag to set its length), drag to move, drag its end to resize, Delete removes, the wheel sets velocity, arrows move (Shift: an octave), Shift-click selects more, Cmd-A all</p>
			<div class="sd-riffmeta">
				<label class="wl-field"><span>Name</span><input name="name" value="${esc(existing?.name || (S.lib ? nextRiffName() : take.track + ' riff ' + n))}" maxlength="80" autocomplete="off"></label>
				${S.lib ? `<label class="wl-field"><span>Group</span><select name="group"><option value="">No group</option>${groups.map(g => `<option ${g === groupNow ? 'selected' : ''}>${esc(g)}</option>`).join('')}</select></label>` : ''}
				<label class="wl-field"><span>What it's for (the agent reads it)</span><input name="note" value="${esc(existing?.note || '')}" placeholder="The hook for the drops; the intro's motif; a bassline under the breakdown…" maxlength="2000" autocomplete="off"></label>
			</div>`,
			buttons: [...(existing ? [{ value: 'delete', label: 'Delete riff', cls: 'danger' }] : []), { value: 'cancel', label: existing ? 'Close' : 'Discard' }, { value: 'ok', label: existing ? 'Save' : 'Keep', cls: 'primary', submit: async api => {
				if (!notes.length) return api.error('The riff has no notes.');
				api.busy(true);
				const asKey = x => ({ beat: +x.beat.toFixed(4), dur: +x.dur.toFixed(4), key: keyStr(x.key), vel: x.vel });
				const riff = { ...(existing ? { id: existing.id } : {}), name: d.querySelector('[name="name"]').value, note: d.querySelector('[name="note"]').value, track: take.track,
					...(S.lib ? { group: d.querySelector('[name="group"]').value } : {}),
					tempo, timeSignature: take.timeSignature, bars, quantize: snapName,
					notes: [...notes].sort((a, b) => a.beat - b.beat || a.key - b.key).map(asKey),
					...(take.played ? { played: take.played.map(asKey) } : {}) };
				try { await change({ op: 'riff', riff }); api.done('ok'); }
				catch (e) { api.busy(false); api.error(e.message); }
			} }],
			init: api => {
				d = api.dlg;
				svg = d.querySelector('[data-r="svg"]');
				const box = d.querySelector('[data-r="rollbox"]');
				svg.addEventListener('pointerdown', down);
				box.addEventListener('keydown', key);
				svg.addEventListener('wheel', wheel, { passive: false });
				d.querySelector('.sd-takebar').addEventListener('click', e => { const b = e.target.closest('[data-a]'); if (b) act(b.dataset.a); });
				d.querySelector('[data-r="snap"]').addEventListener('change', e => { snapName = e.target.value; lastLen = g() || lastLen; draw(); });
				d.querySelector('[data-r="ghost"]')?.addEventListener('change', draw);
				d.querySelector('[data-r="bars"]').addEventListener('change', e => {
					remember();
					bars = +e.target.value; beats = bars * bpb;
					notes = notes.filter(x => x.beat < beats - 1e-6);
					for (const x of notes) x.dur = Math.min(x.dur, beats - x.beat);
					changed();
				});
				d.querySelector('[data-r="tempo"]').addEventListener('change', e => { const v = +e.target.value; if (v >= 20 && v <= 400) { tempo = v; dirty = true; changed(); } });
				d.addEventListener('close', () => { stopPlaying(); cancelAnimationFrame(raf); clearTimeout(replayTimer); });
				d.addEventListener('cancel', e => { if (dirty && notes.length && !existing && !confirm('Discard this take?')) e.preventDefault(); });
				range(); draw();
				box.focus();
			} });
		if (res?.value === 'delete') {
			if (!await WLUI.confirm({ title: `Delete ${existing.name}?`, body: S.lib ? 'It leaves your riff library.' : 'It leaves riffs.json.', ok: 'Delete', danger: true })) return;
			try { await change({ op: 'riff-delete', id: existing.id }); } catch (e) { return recStatus(e.message); }
		}
		if (res) await reload();
	}
	function editRiff(r) {
		if (!r) return;
		const played = r.played ? r.played.map(n => ({ ...n, key: keyNum(n.key) })) : null;
		riffEditor({ track: r.track, tempo: r.tempo, timeSignature: r.timeSignature || [4, 4], bars: r.bars, played, quantize: r.quantize || 'off' }, r);
	}
	function renderRiffs() {
		if (S.lib) return renderLibrary();
		const list = S.data?.riffs || [], box = root.querySelector('#sd-rlist');
		root.querySelector('#sd-riffcard').hidden = !S.data;
		root.querySelector('#sd-riffcount').textContent = list.length || '';
		box.innerHTML = list.map(r => `<div class="sd-riff">
			<button class="sd-rplay" data-play="${esc(r.id)}" title="Play it through ${esc(r.track)}'s sound (loops; click again to stop)">▶</button>
			<button class="pg-item" data-riff="${esc(r.id)}" title="${esc(r.note || '')}"><span class="n">${esc(r.name)}</span><span class="v">${esc(r.track)} · ${r.bars} bar${r.bars === 1 ? '' : 's'} · ${(r.notes || []).length} notes${r.note ? ' · ' + esc(r.note) : ''}</span></button></div>`).join('')
			|| '<div class="sd-hint">Record one with the button by the keyboard: a count-in at the song\'s tempo, then play.</div>';
	}
	/* ---------- the riff library: groups, search, sort, ticked riffs, the prompt ---------- */
	const ALL = -1, NONE = -2;   // the group filter: all riffs, riffs in no group, else an index into the groups
	const riffLine = r => {   // "C2 G2 | F#2 G2 Bb2", a chord as "C4+Eb4+G4"
		const [num, den] = r.timeSignature || [4, 4], bpb = num * 4 / den;
		const ns = [...(r.notes || [])].sort((a, b) => a.beat - b.beat);
		let out = '', bar = 0, last = -1;
		for (const n of ns) {
			const b = Math.floor(n.beat / bpb + 1e-6);
			if (!out) out += '- | '.repeat(b);
			else if (Math.abs(n.beat - last) < 1e-6) out += '+';
			else out += (b > bar ? ' | ' : ' ') + '- | '.repeat(Math.max(0, b - bar - 1));
			out += keyStr(keyNum(n.key)); bar = b; last = n.beat;
		}
		return out;
	};
	function nextRiffName() {
		let n = 0;
		for (const r of S.data?.riffs || []) { const m = /^Riff (\d+)$/.exec(r.name); if (m) n = Math.max(n, +m[1]); }
		return 'Riff ' + Math.max(n + 1, (S.data?.riffs?.length || 0) + 1);
	}
	const groupOf = gi => gi >= 0 ? (S.data?.groups || [])[gi] : null;
	function libRiffs() {   // the library as shown: the group, the search, the sort
		const q = S.rq.trim().toLowerCase(), g = groupOf(S.rgroup);
		let list = (S.data?.riffs || []).filter(r => S.rgroup === ALL || (S.rgroup === NONE ? !r.group : r.group === g));
		if (q) list = list.filter(r => [r.name, r.note, r.group, r.track, riffLine(r)].join(' ').toLowerCase().includes(q));
		const num = (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
		const by = { newest: (a, b) => (b.created || '').localeCompare(a.created || ''), oldest: (a, b) => (a.created || '').localeCompare(b.created || ''), name: num,
			instrument: (a, b) => a.track.localeCompare(b.track) || num(a, b), tempo: (a, b) => a.tempo - b.tempo || num(a, b), length: (a, b) => a.bars - b.bars || num(a, b) };
		return list.sort(by[S.rsort] || by.newest);
	}
	function miniRoll(r) {   // the riff's shape at a glance
		const [num, den] = r.timeSignature || [4, 4], bpb = num * 4 / den, beats = (r.bars || 1) * bpb;
		const ns = riffNotes(r);
		if (!ns.length) return '<span class="sd-mini"></span>';
		let lo = Math.min(...ns.map(n => n.key)), hi = Math.max(...ns.map(n => n.key));
		if (hi - lo < 8) { const mid = (lo + hi) / 2; lo = mid - 4; hi = mid + 4; }
		const W = 132, H = 30, x = b => b / beats * W, y = k => 2 + (hi - k) / (hi - lo) * (H - 7);
		let bars = '';
		for (let b = bpb; b < beats - 1e-6; b += bpb) bars += `<line x1="${x(b).toFixed(1)}" x2="${x(b).toFixed(1)}" y1="0" y2="${H}"/>`;
		return `<svg class="sd-mini" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" aria-hidden="true">${bars}${ns.map(n => `<rect x="${x(n.beat).toFixed(1)}" y="${y(n.key).toFixed(1)}" width="${Math.max(1.5, x(n.dur) - 1).toFixed(1)}" height="3" rx="1"/>`).join('')}</svg>`;
	}
	const when = iso => {
		const t = iso ? new Date(iso) : null;
		if (!t || isNaN(t)) return '';
		return t.toDateString() === new Date().toDateString() ? t.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : t.toLocaleDateString([], { month: 'short', day: 'numeric' });
	};
	function renderLibrary() {
		const all = S.data?.riffs || [], groups = S.data?.groups || [], box = root.querySelector('#sd-rlist');
		if (S.rgroup >= groups.length) S.rgroup = ALL;
		for (const id of [...S.rsel]) if (!all.some(r => r.id === id)) S.rsel.delete(id);
		root.querySelector('#sd-riffcard').hidden = !S.data;
		root.querySelector('#sd-riffcount').textContent = all.length || '';
		const count = gi => gi === ALL ? all.length : all.filter(r => gi === NONE ? !r.group : r.group === groups[gi]).length;
		const chip = (gi, label, tip) => `<button class="sd-gchip ${S.rgroup === gi ? 'on' : ''}" data-gi="${gi}" title="${esc(tip)}">${esc(label)} <b>${count(gi)}</b></button>`;
		root.querySelector('#sd-groups').innerHTML = chip(ALL, 'All', 'Every riff')
			+ groups.map((g, i) => chip(i, g, 'Drop riffs here to file them. Right-click to rename or delete the group')).join('')
			+ (groups.length && all.some(r => !r.group) ? chip(NONE, 'No group', 'Riffs not filed yet. Drop riffs here to take them out of their group') : '')
			+ '<button class="sd-gadd" data-gadd title="A new group">+ Group</button>';
		box.innerHTML = libRiffs().map(r => `<div class="sd-riff lib ${S.rsel.has(r.id) ? 'sel' : ''}" data-rid="${esc(r.id)}" draggable="true">
			<input type="checkbox" data-check="${esc(r.id)}" ${S.rsel.has(r.id) ? 'checked' : ''} aria-label="Tick ${esc(r.name)}">
			<button class="sd-rplay" data-play="${esc(r.id)}" title="Play it on ${esc(r.track)} (loops; click again to stop)">▶</button>
			${miniRoll(r)}
			<button class="pg-item" data-riff="${esc(r.id)}" title="Edit ${esc(r.name)}${r.note ? ': ' + esc(r.note) : ''}"><span class="n">${esc(r.name)}${r.group && S.rgroup === ALL ? ` <i class="sd-gtag">${esc(r.group)}</i>` : ''}</span>
				<span class="v">${esc([r.track, `${r.bars} bar${r.bars === 1 ? '' : 's'}`, `${r.tempo} BPM`, riffLine(r), when(r.created), r.note].filter(Boolean).join(' · '))}</span></button></div>`).join('')
			|| `<div class="sd-hint">${all.length ? 'No riffs match.' : S.data?.tracks.length ? 'Press Record below and play: a count-in, then your riff. Keep it and it lands here.' : 'Add an instrument (left), then press Record below and play.'}</div>`;
		renderSelbar();
		usePrompt();
	}
	function renderSelbar() {
		const bar = root.querySelector('#sd-selbar'), n = S.rsel.size, groups = S.data?.groups || [];
		bar.hidden = !n;
		if (!n) return;
		bar.innerHTML = `<b>${n} ticked</b>
			<label>Move to <select data-sel="move"><option value="">a group…</option>${groups.map((g, i) => `<option value="${i}">${esc(g)}</option>`).join('')}<option value="none">No group</option><option value="new">New group…</option></select></label>
			<button type="button" class="ed-btn" data-sel="all">Tick all shown</button>
			<button type="button" class="ed-btn" data-sel="clear">Clear</button>
			<button type="button" class="ed-btn danger" data-sel="delete">Delete</button>`;
	}
	async function libChange(body, after) {
		try { await change(body); } catch (e) { return WLUI.confirm({ title: 'Could not do that', body: esc(e.message) }); }
		after?.();
		await reload();
	}
	const fileRiffs = (ids, group) => libChange({ op: 'riff-group', ids, group });
	async function newGroup() {
		const groups = S.data?.groups || [];
		return WLUI.prompt({ title: 'New group', body: 'A group for riffs that belong together: a mood, a song idea, a kind of part.', label: 'Name', value: '', ok: 'Create',
			check: v => !v.trim() ? 'Give it a name.' : groups.some(g => g.toLowerCase() === v.trim().toLowerCase()) ? 'There is already a group by that name.' : '' });
	}
	async function groupClick(e) {
		if (e.target.closest('[data-gadd]')) {
			const name = await newGroup();
			if (name) await libChange({ op: 'group-add', group: name.trim() }, () => { S.rgroup = (S.data?.groups || []).length; keep(); });
			return;
		}
		const c = e.target.closest('[data-gi]');
		if (!c) return;
		S.rgroup = +c.dataset.gi; keep(); renderRiffs();
	}
	function groupMenu(e) {
		const c = e.target.closest('[data-gi]'), g = c ? groupOf(+c.dataset.gi) : null;
		if (!g) return;
		WLUI.menu(e, [
			{ label: 'Tick its riffs', run: () => { for (const r of S.data.riffs) if (r.group === g) S.rsel.add(r.id); renderRiffs(); } },
			{ label: 'Rename…', run: async () => {
				const to = await WLUI.prompt({ title: 'Rename ' + g, label: 'Name', value: g, ok: 'Rename', check: v => v.trim() ? '' : 'Give it a name.' });
				if (to && to.trim() !== g) await libChange({ op: 'group-rename', group: g, to: to.trim() });
			} },
			'-',
			{ label: 'Delete group', danger: true, run: async () => {
				if (!await WLUI.confirm({ title: `Delete the group ${g}?`, body: 'Its riffs stay in the library, in no group.', ok: 'Delete group', danger: true })) return;
				await libChange({ op: 'group-delete', group: g }, () => { S.rgroup = ALL; keep(); });
			} },
		]);
	}
	function groupDrop(e) {
		const c = e.target.closest('[data-gi]');
		root.querySelectorAll('.sd-gchip.drop').forEach(x => x.classList.remove('drop'));
		if (!c) return;
		e.preventDefault();
		let ids;
		try { ids = JSON.parse(e.dataTransfer.getData('text/x-wl-riffs') || '[]'); } catch { return; }
		const gi = +c.dataset.gi;
		if (ids.length && gi !== ALL) fileRiffs(ids, gi === NONE ? '' : groupOf(gi));
	}
	async function selChange(e) {
		const s = e.target.closest('[data-sel="move"]');
		if (!s || !s.value) return;
		const g = s.value === 'none' ? '' : s.value === 'new' ? await newGroup() : groupOf(+s.value);
		if (g == null) { s.value = ''; return; }
		await fileRiffs([...S.rsel], g.trim());
	}
	async function selClick(e) {
		const b = e.target.closest('button[data-sel]');
		if (!b) return;
		if (b.dataset.sel === 'all') { for (const r of libRiffs()) S.rsel.add(r.id); return renderRiffs(); }
		if (b.dataset.sel === 'clear') { S.rsel.clear(); return renderRiffs(); }
		if (b.dataset.sel === 'delete') {
			const n = S.rsel.size;
			if (!await WLUI.confirm({ title: `Delete ${n} riff${n === 1 ? '' : 's'}?`, body: 'They leave your riff library. Songs they were copied into keep their copies.', ok: 'Delete', danger: true })) return;
			await libChange({ op: 'riff-delete', ids: [...S.rsel] }, () => S.rsel.clear());
		}
	}
	function riffMenu(e, id) {
		const r = riffById(id), groups = S.data?.groups || [];
		if (!r) return;
		WLUI.menu(e, [
			{ label: 'Edit…', run: () => editRiff(r) },
			{ label: S.rsel.has(id) ? 'Untick' : 'Tick', run: () => { S.rsel.has(id) ? S.rsel.delete(id) : S.rsel.add(id); renderRiffs(); } },
			'-',
			...groups.filter(g => g !== r.group).map(g => ({ label: 'Move to ' + g, run: () => fileRiffs([id], g) })),
			...(r.group ? [{ label: 'Take out of ' + r.group, run: () => fileRiffs([id], '') }] : []),
			{ label: 'Move to a new group…', run: async () => { const g = await newGroup(); if (g) fileRiffs([id], g.trim()); } },
			'-',
			{ label: 'Delete', danger: true, run: async () => {
				if (!await WLUI.confirm({ title: `Delete ${r.name}?`, body: 'It leaves your riff library. Songs it was copied into keep their copies.', ok: 'Delete', danger: true })) return;
				await libChange({ op: 'riff-delete', ids: [id] }, () => S.rsel.delete(id));
			} },
		]);
	}
	// the prompt: the ticked riffs, else the group shown; the agent copies them into a new song folder it names
	function usePrompt() {
		const box = root?.querySelector('#sd-uprompt');
		if (!box || !S.lib) return;
		const all = S.data?.riffs || [], g = groupOf(S.rgroup);
		let picked = all.filter(r => S.rsel.has(r.id)), refs = picked.map(r => r.id);
		if (!picked.length && g) { picked = all.filter(r => r.group === g); refs = ['group:' + g]; }
		root.querySelector('#sd-usecount').textContent = !picked.length ? '' : S.rsel.size ? `${picked.length} ticked` : `${picked.length} in ${g}`;
		if (!picked.length) { box.value = ''; return; }
		const q = v => /^[\w.:/@+-]+$/.test(v) ? v : `"${v.replace(/(["\\$`])/g, '\\$1')}"`;
		const dir = (S.data.songsDir || '').replace(/\/+$/, '');
		const brief = root.querySelector('#sd-ubrief').value.trim();
		box.value = `/wavelength Write a new song from riffs in my riff library.

Make a folder for it in ${dir || 'my songs folder'}, named after the song you write, and copy the riffs in first:

wavelength riffs use ${q((dir || '<songs folder>') + '/<song-folder>')} ${refs.map(q).join(' ')}

That puts them in the song's riffs.json, and the instruments I played them on, as I tuned them, in its sounds.json and job.json. Then build the song around them (AGENTS.md, "Starting from a riff"). \`wavelength riffs show <id>\` shows a riff in full.

Riffs:
${picked.map(r => `- ${r.id} "${r.name}": ${r.track} (${soundLine(row(r.track) || {})}), ${r.bars} bar${r.bars === 1 ? '' : 's'} at ${r.tempo} BPM: ${riffLine(r)}${r.note ? `. ${r.note}` : ''}`).join('\n')}
${brief ? `\nBrief:\n${brief}\n` : ''}`;
	}
	// a song's Riffs card: copy riffs in from the library, each with the instrument it plays on
	async function fromLibrary() {
		if (!S.data || S.lib) return;
		const lib = await fetch('api/sounds?song=' + LIB).then(r => r.json()).catch(() => null), riffs = lib?.riffs || [];
		if (!riffs.length) return WLUI.confirm({ title: 'Your riff library is empty', body: 'Record riffs on the Riffs page (in the menu on the left), then copy them into songs from here.', ok: 'OK' });
		const groups = [...(lib.groups || []), ''];
		const body = `<p>Tick the riffs to copy into ${esc(S.data.song)}. Each comes with the instrument it plays on, as a track in <code>sounds.json</code> and the job when the song has none like it.</p>
			<div class="sd-libpick">${groups.map(g => { const rs = riffs.filter(r => (r.group || '') === g); return rs.length ? `<div><h4>${esc(g || 'No group')}</h4>${rs.map(r =>
				`<label><input type="checkbox" value="${esc(r.id)}"> <b>${esc(r.name)}</b> <span>${esc(r.track)} · ${r.bars} bar${r.bars === 1 ? '' : 's'} · ${r.tempo} BPM · ${esc(riffLine(r))}</span></label>`).join('')}</div>` : ''; }).join('')}</div>`;
		const res = await WLUI.modal({ title: 'Riffs from your library', cls: 'sd-dlg', body, buttons: [{ value: 'cancel', label: 'Cancel' }, { value: 'ok', label: 'Copy into the song', cls: 'primary', submit: async api => {
			const ids = [...api.dlg.querySelectorAll('.sd-libpick input:checked')].map(x => x.value);
			if (!ids.length) return api.error('Tick a riff first.');
			api.busy(true);
			try { const r = await postJson('api/riffs/use', { song: S.slug, ids }); if (r.error) throw new Error(r.error); api.done('ok'); }
			catch (e) { api.busy(false); api.error(e.message); }
		} }] });
		if (res?.value === 'ok') await reload();
	}
	function riffLines(d) {
		const list = d.riffs || [];
		if (!list.length) return '';
		return `
Riffs I recorded (riffs.json; notes in beats from each riff's first downbeat): build the song around them. Keep each one recognisable where it matters (its rhythm and contour), and vary, transpose or extend it where the song needs:
${list.map(r => `- ${r.name}: ${r.track}, ${r.bars} bar${r.bars === 1 ? '' : 's'} at ${r.tempo} BPM, ${(r.notes || []).length} notes${r.note ? `. ${r.note}` : ''}`).join('\n')}
`;
	}

	function current() {
		if (S.audition) {   // the preset chooser is open: the preset clicked there plays, as it would be picked (no knobs)
			const a = S.audition;
			return { id: 'try|' + a.plugin + '|' + (a.preset || ''), label: plainName(a.plugin) + (a.preset ? ' · ' + a.preset : ''), plugin: a.plugin, preset: a.preset || '', live: !/^builtin:/.test(a.plugin) };
		}
		const t = row(S.sel);
		return t && S.slug ? { song: S.slug, track: t.name, label: `${t.name} (${plainName(t.plugin)}${t.preset ? ' · ' + t.preset : ''})`, id: 'sd|' + S.slug + '|' + t.name + '|' + soundKey(t) } : null;
	}

	window.WLSounds = {
		show(slug) {
			if (!built) build();
			document.title = 'Sounds · Wavelength';
			const want = slug || S.songSlug || state.slug || '';
			if (want !== S.slug || !S.data || S.lib) open(want); else { render(); keys.ensure(); }
		},
		showLibrary() {
			if (!built) build();
			document.title = 'Riffs · Wavelength';
			if (S.slug !== LIB || !S.data || !S.lib) open(LIB, true); else { render(); keys.ensure(); }
		},
		hide() { if (built) { save(); clearInterval(S.winPoll); S.win = null; if (R.on) stopRecording(); stopPlaying(); keys.releaseAll(); keys.close(); } },
		songsChanged() { if (built && !root.hidden) render(); },
		stats: () => keys?.stats() ?? null,
	};
	addEventListener('pagehide', () => { if (built) { save(); keys.close(); } });
})();
