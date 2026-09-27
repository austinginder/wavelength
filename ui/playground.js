/* Playground: play any installed instrument, or a song's track, from an on-screen piano or the computer
 * keyboard. CLAP and VST3 instruments play live: a server worker keeps the plugin running and streams its
 * audio (api/live/stream, played by an AudioWorklet), keys send note-on and note-off, so a note holds
 * until you let go and ends with the instrument's own release. Other instruments (built-in, VST2, AU)
 * play note by note: each press renders a long note (api/play) that fades out when you let go.
 *
 * Uses the page's globals: $, esc, state, postHeaders, postJson.
 */
(() => {
	const ORDER = ['clap', 'vst3', 'vst2', 'au', 'builtin'];   // the format a name plays in, best first
	const NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B'];
	const keyName = k => NAMES[((k % 12) + 12) % 12] + (Math.floor(k / 12) - 1);
	const KEYMAP = { KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11, KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16 };
	const LETTER = Object.fromEntries(Object.entries(KEYMAP).map(([c, n]) => [n, c === 'Semicolon' ? ';' : c.slice(3)]));
	const CHORDS = { single: [0], octave: [0, 12], fifth: [0, 7], major: [0, 4, 7], minor: [0, 3, 7], maj7: [0, 4, 7, 11], min7: [0, 3, 7, 10], sus4: [0, 5, 7] };
	const P = { groups: [], inst: null, spec: '', presets: [], preset: '', octave: 4, vel: .85, len: 4, chord: 'single', source: 'installed', song: '', track: '', loaded: new Set(), q: '', pq: '' };
	try { Object.assign(P, JSON.parse(localStorage.getItem('wl-playground') || '{}'), { groups: [], presets: [], loaded: new Set() }); } catch {}
	if (!(P.len >= 1)) P.len = 4;   // older saved settings had a short fixed length
	const keep = () => { try { const { spec, preset, octave, vel, len, chord, source, song, track } = P; localStorage.setItem('wl-playground', JSON.stringify({ spec, preset, octave, vel, len, chord, source, song, track })); } catch {} };
	let built = false, root;

	function build() {
		root = $('#playview');
		root.innerHTML = `
		<div class="pg-head"><h1>Playground</h1>
			<p>Play any installed instrument, or a track from one of your songs. Each key renders through the plugin itself, so it sounds a moment after you press it (the first note loads the instrument).</p></div>
		<div class="pg-grid">
			<section class="card pg-col">
				<h2>Instrument <span class="pg-seg"><button data-src="installed">Installed</button><button data-src="song">From a song</button></span></h2>
				<div id="pg-installed"><input type="search" id="pg-q" placeholder="Search 0 instruments" autocomplete="off" spellcheck="false"><div class="pg-list" id="pg-list"></div></div>
				<div id="pg-fromsong" hidden><select id="pg-song"></select><div class="pg-list" id="pg-tracks"></div></div>
			</section>
			<section class="card pg-col" id="pg-prescol">
				<h2>Preset <span class="r" id="pg-pcount"></span></h2>
				<input type="search" id="pg-pq" placeholder="Search presets" autocomplete="off" spellcheck="false">
				<div class="pg-list" id="pg-plist"><div class="empty">Pick an instrument.</div></div>
			</section>
			<section class="card pg-play">
				<h2><span id="pg-now">Pick an instrument</span><span class="r" id="pg-formats"></span></h2>
				<div class="pg-controls">
					<label>Octave <span class="pg-step"><button data-oct="-1" title="Z">−</button><b id="pg-oct"></b><button data-oct="1" title="X">+</button></span></label>
					<label>Velocity <input type="range" id="pg-vel" min="0.1" max="1" step="0.05"> <b id="pg-velv" class="mono"></b></label>
					<label id="pg-lenwrap" title="Only for instruments that play note by note">Max length <input type="range" id="pg-len" min="0.5" max="8" step="0.5"> <b id="pg-lenv" class="mono"></b></label>
					<label><input type="checkbox" id="pg-sus"> Sustain <span class="mono" style="color:var(--muted)">(Space)</span></label>
					<label>Play <select id="pg-chord">${Object.keys(CHORDS).map(c => `<option value="${c}">${c === 'single' ? 'single notes' : c}</option>`).join('')}</select></label>
				</div>
				<div class="pg-piano" id="pg-piano"></div>
				<div class="pg-status" id="pg-status">Keys: A-K are the white keys, W E T Y U O P the black ones; Z / X change the octave; hold Space for sustain. Hold keys (several at once) as long as you want the notes; click or drag across the piano too.</div>
			</section>
		</div>`;
		const on = (sel, ev, fn) => root.querySelector(sel).addEventListener(ev, fn);
		on('#pg-q', 'input', e => { P.q = e.target.value; renderInstruments(); });
		on('#pg-pq', 'input', e => { P.pq = e.target.value; renderPresets(); });
		on('#pg-list', 'click', e => { const b = e.target.closest('[data-g]'); if (b) pickInstrument(P.groups[+b.dataset.g]); });
		on('#pg-plist', 'click', e => { const b = e.target.closest('[data-p]'); if (b) { P.preset = b.dataset.p; keep(); renderPresets(); renderNow(); liveEnsure(); } });
		on('#pg-formats', 'click', e => { const b = e.target.closest('[data-spec]'); if (b) { P.spec = b.dataset.spec; P.preset = ''; keep(); loadPresets(); renderNow(); } });
		on('.pg-seg', 'click', e => { const b = e.target.closest('[data-src]'); if (b) { P.source = b.dataset.src; keep(); renderSource(); } });
		on('#pg-song', 'change', e => { P.song = e.target.value; P.track = ''; keep(); loadSongTracks(); });
		on('#pg-tracks', 'click', e => { const b = e.target.closest('[data-t]'); if (b) { P.track = b.dataset.t; keep(); renderSongTracks(); renderNow(); liveEnsure(); } });
		on('.pg-step', 'click', e => { const b = e.target.closest('[data-oct]'); if (b) octave(+b.dataset.oct); });
		const vel = root.querySelector('#pg-vel'), len = root.querySelector('#pg-len'), chord = root.querySelector('#pg-chord');
		vel.value = P.vel; len.value = P.len; chord.value = P.chord;
		vel.addEventListener('input', () => { P.vel = +vel.value; controls(); keep(); });
		len.addEventListener('input', () => { P.len = +len.value; controls(); keep(); });
		chord.addEventListener('change', () => { P.chord = chord.value; keep(); });
		// the piano: press to play
		const piano = root.querySelector('#pg-piano');
		// the mouse (or a finger): press a key, drag across keys (a glissando), let go
		let pointerKey = null;
		const keyAt = e => { const el = document.elementFromPoint(e.clientX, e.clientY)?.closest('[data-k]'); return el && piano.contains(el) ? +el.dataset.k : null; };
		piano.addEventListener('pointerdown', e => {
			const k = keyAt(e); if (k == null) return;
			e.preventDefault(); piano.setPointerCapture(e.pointerId);
			pointerKey = k; down('ptr', k);
		});
		piano.addEventListener('pointermove', e => {
			if (pointerKey == null) return;
			const k = keyAt(e);
			if (k != null && k !== pointerKey) { up('ptr'); pointerKey = k; down('ptr', k); }
		});
		const pointerUp = () => { if (pointerKey != null) { up('ptr'); pointerKey = null; } };
		piano.addEventListener('pointerup', pointerUp);
		piano.addEventListener('pointercancel', pointerUp);
		const typing = e => e.metaKey || e.ctrlKey || e.altKey || /INPUT|SELECT|TEXTAREA/.test(e.target.tagName) || document.querySelector('dialog[open]');
		document.addEventListener('keydown', e => {
			if (root.hidden || typing(e)) return;
			if (e.code in KEYMAP) { e.preventDefault(); if (!e.repeat) down(e.code, 12 * (P.octave + 1) + KEYMAP[e.code]); }
			else if (e.code === 'KeyZ' || e.code === 'KeyX') { e.preventDefault(); octave(e.code === 'KeyX' ? 1 : -1); }
			else if (e.code === 'Space') { e.preventDefault(); if (!e.repeat) sustain(true); }
		});
		document.addEventListener('keyup', e => {
			if (root.hidden) return;
			if (e.code in KEYMAP) up(e.code);
			else if (e.code === 'Space') sustain(false);
		});
		addEventListener('blur', releaseAll);   // keyups never come for keys held while the window loses focus
		root.querySelector('#pg-sus').addEventListener('change', e => sustain(e.target.checked, true));
		built = true;
		controls(); renderPiano(); renderSource();
		loadInstruments();
	}

	/* ---------- instruments ---------- */
	async function loadInstruments() {
		const list = root.querySelector('#pg-list');
		list.innerHTML = '<div class="empty">Reading the installed plugins…</div>';
		const r = await fetch('api/instruments').then(r => r.json()).catch(() => ({ instruments: [] }));
		const by = new Map();
		for (const p of r.instruments || []) {
			const key = p.name.toLowerCase();
			if (!by.has(key)) by.set(key, { name: p.name, vendor: p.vendor, formats: [] });
			by.get(key).formats.push({ format: p.format, spec: p.format === 'builtin' ? p.id : `${p.format}:${p.name}`, intel: p.arch === 'x86_64' });
		}
		P.groups = [...by.values()].sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
		P.groups.forEach(g => g.formats.sort((a, b) => a.intel - b.intel || ORDER.indexOf(a.format) - ORDER.indexOf(b.format)));
		root.querySelector('#pg-q').placeholder = `Search ${P.groups.length} instruments`;
		if (P.spec && !P.inst) P.inst = P.groups.find(g => g.formats.some(f => f.spec === P.spec)) || null;
		renderInstruments(); renderNow();
		if (P.inst && P.source === 'installed') loadPresets();
	}
	function renderInstruments() {
		const q = P.q.trim().toLowerCase();
		const shown = P.groups.map((g, i) => [g, i]).filter(([g]) => !q || (g.name + ' ' + g.vendor).toLowerCase().includes(q));
		root.querySelector('#pg-list').innerHTML = shown.map(([g, i]) => `<button class="pg-item ${g === P.inst ? 'sel' : ''}" data-g="${i}">
			<span class="n">${esc(g.name)}</span><span class="v">${esc(g.vendor || '')}</span><span class="f">${g.formats.map(f => f.format).join(' ')}</span></button>`).join('')
			|| '<div class="empty">No instrument matches.</div>';
	}
	function pickInstrument(g) {
		P.inst = g; P.spec = g.formats[0].spec; P.preset = ''; P.pq = ''; root.querySelector('#pg-pq').value = '';
		keep(); renderInstruments(); renderNow(); loadPresets();
	}
	async function loadPresets() {
		const list = root.querySelector('#pg-plist'), spec = P.spec;
		list.innerHTML = '<div class="empty">Reading presets…</div>';
		const r = await fetch('api/presets?plugin=' + encodeURIComponent(spec)).then(r => r.json()).catch(() => ({ presets: [] }));
		if (spec !== P.spec) return;
		P.presets = r.presets || [];
		renderPresets();
	}
	function renderPresets() {
		const q = P.pq.trim().toLowerCase(), all = P.presets;
		const hits = all.filter(p => !q || (p.name + ' ' + (p.category || '') + ' ' + (p.features || []).join(' ')).toLowerCase().includes(q));
		root.querySelector('#pg-pcount').textContent = all.length ? (q ? `${hits.length} of ${all.length}` : all.length) : '';
		const row = (name, sub) => `<button class="pg-item ${name === P.preset ? 'sel' : ''}" data-p="${esc(name)}"><span class="n">${esc(name || 'Default sound')}</span><span class="v">${esc(sub || '')}</span></button>`;
		root.querySelector('#pg-plist').innerHTML = row('', 'what the plugin loads with')
			+ hits.slice(0, 400).map(p => row(p.name, [p.category, ...(p.features || [])].filter(Boolean).join(' · '))).join('')
			+ (hits.length > 400 ? `<div class="empty">${hits.length - 400} more: search to narrow them down.</div>` : '')
			+ (!all.length ? '<div class="empty">No presets Wavelength can read for this one.</div>' : '');
	}

	/* ---------- a song's track ---------- */
	function renderSource() {
		root.querySelectorAll('.pg-seg button').forEach(b => b.classList.toggle('on', b.dataset.src === P.source));
		root.querySelector('#pg-installed').hidden = P.source !== 'installed';
		root.querySelector('#pg-fromsong').hidden = P.source !== 'song';
		root.querySelector('#pg-prescol').hidden = P.source !== 'installed';
		root.querySelector('.pg-grid').classList.toggle('two', P.source !== 'installed');
		if (P.source === 'song') {
			const sel = root.querySelector('#pg-song');
			sel.innerHTML = state.songs.map(s => `<option value="${esc(s.slug)}">${esc(s.title || s.slug)}</option>`).join('');
			if (!state.songs.some(s => s.slug === P.song)) P.song = state.slug || state.songs[0]?.slug || '';
			sel.value = P.song;
			loadSongTracks();
		}
		renderNow();
	}
	let songTracks = [];
	async function loadSongTracks() {
		const slug = P.song; if (!slug) return;
		const d = await fetch('api/song?song=' + encodeURIComponent(slug)).then(r => r.json()).catch(() => null);
		if (slug !== P.song) return;
		songTracks = (d?.job?.tracks || []).map(t => ({ name: t.name, sound: String(t.preset || t.plugin || '').split('/').pop(), plugin: t.plugin, lo: Math.min(...t.notes.map(n => n[2])), hi: Math.max(...t.notes.map(n => n[2])) }));
		renderSongTracks(); renderNow();
	}
	function renderSongTracks() {
		root.querySelector('#pg-tracks').innerHTML = songTracks.map(t => `<button class="pg-item ${t.name === P.track ? 'sel' : ''}" data-t="${esc(t.name)}">
			<span class="n">${esc(t.name)}</span><span class="v">${esc(t.sound)}</span><span class="f">${isFinite(t.lo) ? keyName(t.lo) + '-' + keyName(t.hi) : ''}</span></button>`).join('') || '<div class="empty">This song has no tracks yet.</div>';
	}

	/* ---------- playing ---------- */
	function current() {
		if (P.source === 'song') return P.song && P.track ? { song: P.song, track: P.track, label: `${P.track} (${P.song})`, id: 'song|' + P.song + '|' + P.track } : null;
		return P.spec ? { plugin: P.spec, preset: P.preset, label: (P.inst?.name || P.spec) + (P.preset ? ' · ' + P.preset : ''), id: P.spec + '|' + P.preset } : null;
	}
	function renderNow() {
		const c = current();
		root.querySelector('#pg-now').textContent = c ? c.label : 'Pick an instrument';
		root.querySelector('#pg-formats').innerHTML = P.source === 'installed' && P.inst ? P.inst.formats.map(f => `<button class="pg-fmt ${f.spec === P.spec ? 'on' : ''}" data-spec="${esc(f.spec)}" title="${f.intel ? 'Intel-only: may not play here' : 'Play this format'}">${f.format}${f.intel ? ' (Intel)' : ''}</button>`).join('') : '';
	}
	const status = (msg, err) => { const el = root.querySelector('#pg-status'); el.textContent = msg; el.classList.toggle('err', !!err); };
	const liveFormat = c => c && (c.song || /^(clap|vst3):/.test(c.plugin));   // song tracks: the worker says whether it can

	/* ---------- live: a plugin that keeps running, audio streamed as it plays ---------- */
	// the audio thread's side: ui/live-worklet.js (a ring buffer the stream fills and the output drains)
	let live = null;
	const session = 'pg' + Math.random().toString(36).slice(2, 10);
	// the live session for the current sound: started when a sound is picked, reused while it stays
	function liveEnsure() {
		const c = current();
		if (!c) return Promise.resolve(null);
		if (live?.key === c.id) return live.ready;
		liveClose();
		if (!liveFormat(c)) return Promise.resolve(null);
		const L = live = { key: c.id, id: session + '-' + Date.now().toString(36), q: Promise.resolve() };
		L.ready = (async () => {
			status(`Loading ${c.label}…`);
			let r;
			try { r = await postJson('api/live/start', { id: L.id, ...(c.song ? { song: c.song, track: c.track } : { plugin: c.plugin, preset: c.preset }) }); }
			catch (e) { r = { error: e.message }; }
			if (live !== L) return null;
			if (r.error) {   // not a live format after all (a song track on a built-in instrument): note by note
				L.failed = true;
				status(r.error === 'live' ? `${c.label} plays note by note: hold a key for up to ${P.len} s.` : `Could not load ${c.label}: ${r.error}`, r.error !== 'live');
				return null;
			}
			L.ctx = new AudioContext({ sampleRate: r.sampleRate || 48000, latencyHint: 'interactive' });
			await L.ctx.audioWorklet.addModule('ui/live-worklet.js');
			L.node = new AudioWorkletNode(L.ctx, 'wl-live', { outputChannelCount: [2] });
			L.node.connect(L.ctx.destination);
			L.abort = new AbortController();
			const resp = await fetch('api/live/stream?id=' + encodeURIComponent(L.id), { signal: L.abort.signal }).catch(() => null);
			if (!resp?.ok || live !== L) { if (live === L) { L.failed = true; status('The live stream did not start.', true); } return null; }
			pump(L, resp.body.getReader());
			const ms = Math.round(((L.ctx.baseLatency || 0) + (L.ctx.outputLatency || 0)) * 1000 + 35 + 25);
			status(`Live: ${r.plugin}${r.preset ? ' · ' + r.preset : ''} · about ${ms} ms from key to sound. Hold keys to sustain; several at once.`);
			root.querySelector('#pg-lenwrap').hidden = true;
			return L;
		})();
		root.querySelector('#pg-lenwrap').hidden = false;
		return L.ready;
	}
	// the stream: 16-bit stereo PCM in whatever chunks arrive, to floats for the worklet
	async function pump(L, reader) {
		let rest = new Uint8Array(0);
		try {
			for (;;) {
				const { value, done } = await reader.read();
				if (done || live !== L) break;
				let bytes = value;
				if (rest.length) { const m = new Uint8Array(rest.length + value.length); m.set(rest); m.set(value, rest.length); bytes = m; }
				const whole = bytes.length - (bytes.length % 4);
				rest = bytes.slice(whole);
				const pcm = new Int16Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + whole));
				const f = new Float32Array(pcm.length);
				let peak = 0;
				for (let i = 0; i < pcm.length; i++) { f[i] = pcm[i] / 32768; peak = Math.max(peak, Math.abs(f[i])); }
				L.frames = (L.frames || 0) + f.length / 2; L.peak = peak;   // for WLPlay.stats()
				L.node.port.postMessage(f, [f.buffer]);
			}
		} catch {}
		if (live === L) { live = null; status('The instrument stopped. Press a key to load it again.', true); }
	}
	function liveClose() {
		if (!live) return;
		const L = live; live = null;
		L.abort?.abort(); L.ctx?.close().catch(() => {});
		if (!L.failed) postJson('api/live/stop', { id: L.id }).catch(() => {});
	}
	// events go out one request at a time, in order: a quick tap's note-off never overtakes its note-on
	function send(L, events) {
		L.q = L.q.then(() => postJson('api/live/event', { id: L.id, events })).catch(() => {});
		if (L.ctx?.state === 'suspended') L.ctx.resume();
	}

	/* ---------- note by note (built-in, VST2, AU): a long note that fades out on release ---------- */
	let actx = null;
	async function clip(notes, handle) {
		const c = current();
		if (!c) return status('Pick an instrument first.', true);
		const id = c.id;
		if (!P.loaded.has(id)) status(`Loading ${c.label}…`);
		try {
			const body = { notes: notes.map(k => ({ key: k, vel: P.vel, start: 0, dur: P.len })), tail: 1, ...(c.song ? { song: c.song, track: c.track } : { plugin: c.plugin, preset: c.preset }) };
			const r = await fetch('api/play', { method: 'POST', headers: postHeaders(), body: JSON.stringify(body) });
			if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || 'the server answered ' + r.status); }
			const info = JSON.parse(r.headers.get('X-Wavelength-Play') || '{}');
			actx ??= new AudioContext();
			if (actx.state === 'suspended') await actx.resume();
			const src = actx.createBufferSource(), gain = actx.createGain();
			src.buffer = await actx.decodeAudioData(await r.arrayBuffer());
			src.connect(gain).connect(actx.destination);
			P.loaded.add(id);
			if (handle.released) return;   // let go before it arrived
			src.start(); handle.src = src; handle.gain = gain;
			status(`${notes.map(keyName).join(' ')} · ${info.plugin}${info.preset ? ' · ' + info.preset : ''} · note by note, up to ${P.len} s · ${info.loaded ? 'loaded in ' : ''}${info.ms} ms`);
		} catch (e) { status(`Could not play: ${e.message}`, true); }
	}
	function fade(handle) {
		handle.released = true;
		if (!handle.gain) return;
		const t = actx.currentTime;
		handle.gain.gain.setValueAtTime(handle.gain.gain.value, t);
		handle.gain.gain.linearRampToValueAtTime(0, t + 0.18);
		handle.src.stop(t + 0.2);
	}

	/* ---------- keys: down, up, sustain ---------- */
	const held = new Map();   // source (a key code, the pointer) -> {notes, clip}
	let sustained = false, pedalOn = false;
	const pedalled = new Set();   // notes let go while the pedal is down (live)
	const pedalClips = [];
	async function down(src, k) {
		if (held.has(src)) return;
		const notes = [...new Set(CHORDS[P.chord].map(i => Math.min(127, k + i)))];
		const h = { notes };
		held.set(src, h);
		mark(notes, true);
		const L = await liveEnsure();
		if (held.get(src) !== h) return;   // already let go while it loaded
		if (L && live === L) {
			notes.forEach(n => pedalled.delete(n));
			send(L, notes.map(n => ({ on: n, vel: P.vel })));
		} else clip(notes, h.clip = {});
	}
	function up(src) {
		const h = held.get(src);
		if (!h) return;
		held.delete(src);
		mark(h.notes, false);
		if (h.clip) { if (sustained) pedalClips.push(h.clip); else fade(h.clip); return; }
		if (!live || live.failed || !live.node) return;
		const still = new Set([...held.values()].flatMap(x => x.notes));   // another key may hold the same note (chords)
		const off = h.notes.filter(n => !still.has(n));
		if (sustained) { off.forEach(n => pedalled.add(n)); return; }
		if (off.length) send(live, off.map(n => ({ off: n })));
	}
	function sustain(on, fromBox) {
		pedalOn = fromBox ? on : pedalOn;
		const want = on || pedalOn;
		if (!fromBox) root.querySelector('#pg-sus').checked = want;
		if (want === sustained) return;
		sustained = want;
		if (!sustained) {   // pedal up: what was let go stops now
			const still = new Set([...held.values()].flatMap(x => x.notes));
			const off = [...pedalled].filter(n => !still.has(n));
			pedalled.clear();
			if (off.length && live?.node) send(live, off.map(n => ({ off: n })));
			pedalClips.splice(0).forEach(fade);
		}
	}
	function releaseAll() {
		for (const src of [...held.keys()]) up(src);
		sustained = pedalOn = false; root.querySelector('#pg-sus').checked = false;
		pedalled.clear(); pedalClips.splice(0).forEach(fade);
		if (live?.node) send(live, [{ allOff: true }]);
	}
	function mark(notes, on) {
		for (const n of notes) root.querySelector(`[data-k="${n}"]`)?.classList.toggle('down', on || [...held.values()].some(x => x.notes.includes(n)));
	}

	/* ---------- controls ---------- */
	function controls() {
		root.querySelector('#pg-oct').textContent = P.octave;
		root.querySelector('#pg-velv').textContent = Math.round(P.vel * 127);
		root.querySelector('#pg-lenv').textContent = P.len.toFixed(1) + ' s';
	}
	function octave(d) { P.octave = Math.max(0, Math.min(8, P.octave + d)); keep(); controls(); renderPiano(); }
	function renderPiano() {
		const base = 12 * (P.octave + 1) - 12, whites = [], blacks = [];   // an octave below the keyboard's, then two up
		let w = 0;
		for (let k = base; k <= base + 36; k++) {
			const pc = k % 12, black = [1, 3, 6, 8, 10].includes(pc), rel = k - 12 * (P.octave + 1);
			const label = LETTER[rel] ?? '';
			if (black) blacks.push(`<div class="pg-k b" data-k="${k}" style="left:calc(${w} * var(--w) - var(--bw) / 2)"><span>${label}</span></div>`);
			else { whites.push(`<div class="pg-k w" data-k="${k}"><span>${label}</span>${pc === 0 ? `<i>${keyName(k)}</i>` : ''}</div>`); w++; }
		}
		root.querySelector('#pg-piano').innerHTML = `<div class="pg-keys" style="--n:${w}">${whites.join('')}${blacks.join('')}</div>`;
	}

	window.WLPlay = {
		show() { if (!built) build(); else if (P.source === 'song') renderSource(); document.title = 'Playground · Wavelength'; if (current()) liveEnsure(); },
		hide() { if (built) { releaseAll(); liveClose(); } },
		stats: () => live ? { live: !!live.node, frames: live.frames || 0, peak: live.peak || 0, state: live.ctx?.state } : null,
	};
	addEventListener('pagehide', () => liveClose());
})();
