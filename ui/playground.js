/* Playground: play any installed instrument, or a song's track, from an on-screen piano or the computer
 * keyboard. Each key renders through the plugin itself in a warm server worker (POST api/play), so it
 * sounds a few tens of milliseconds after the press; the first note of an instrument loads it.
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
	const P = { groups: [], inst: null, spec: '', presets: [], preset: '', octave: 4, vel: .85, len: .8, chord: 'single', source: 'installed', song: '', track: '', loaded: new Set(), q: '', pq: '' };
	try { Object.assign(P, JSON.parse(localStorage.getItem('wl-playground') || '{}'), { groups: [], presets: [], loaded: new Set() }); } catch {}
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
					<label>Length <input type="range" id="pg-len" min="0.1" max="4" step="0.1"> <b id="pg-lenv" class="mono"></b></label>
					<label>Play <select id="pg-chord">${Object.keys(CHORDS).map(c => `<option value="${c}">${c === 'single' ? 'single notes' : c}</option>`).join('')}</select></label>
				</div>
				<div class="pg-piano" id="pg-piano"></div>
				<div class="pg-status" id="pg-status">Keys: A-K are the white keys, W E T Y U O P the black ones; Z / X change the octave. Click the keys too.</div>
			</section>
		</div>`;
		const on = (sel, ev, fn) => root.querySelector(sel).addEventListener(ev, fn);
		on('#pg-q', 'input', e => { P.q = e.target.value; renderInstruments(); });
		on('#pg-pq', 'input', e => { P.pq = e.target.value; renderPresets(); });
		on('#pg-list', 'click', e => { const b = e.target.closest('[data-g]'); if (b) pickInstrument(P.groups[+b.dataset.g]); });
		on('#pg-plist', 'click', e => { const b = e.target.closest('[data-p]'); if (b) { P.preset = b.dataset.p; keep(); renderPresets(); renderNow(); preview(); } });
		on('#pg-formats', 'click', e => { const b = e.target.closest('[data-spec]'); if (b) { P.spec = b.dataset.spec; P.preset = ''; keep(); loadPresets(); renderNow(); } });
		on('.pg-seg', 'click', e => { const b = e.target.closest('[data-src]'); if (b) { P.source = b.dataset.src; keep(); renderSource(); } });
		on('#pg-song', 'change', e => { P.song = e.target.value; P.track = ''; keep(); loadSongTracks(); });
		on('#pg-tracks', 'click', e => { const b = e.target.closest('[data-t]'); if (b) { P.track = b.dataset.t; keep(); renderSongTracks(); renderNow(); preview(); } });
		on('.pg-step', 'click', e => { const b = e.target.closest('[data-oct]'); if (b) octave(+b.dataset.oct); });
		const vel = root.querySelector('#pg-vel'), len = root.querySelector('#pg-len'), chord = root.querySelector('#pg-chord');
		vel.value = P.vel; len.value = P.len; chord.value = P.chord;
		vel.addEventListener('input', () => { P.vel = +vel.value; controls(); keep(); });
		len.addEventListener('input', () => { P.len = +len.value; controls(); keep(); });
		chord.addEventListener('change', () => { P.chord = chord.value; keep(); });
		// the piano: press to play
		const piano = root.querySelector('#pg-piano');
		piano.addEventListener('pointerdown', e => { const k = e.target.closest('[data-k]'); if (k) { e.preventDefault(); press(+k.dataset.k); } });
		document.addEventListener('keydown', e => {
			if (root.hidden || e.metaKey || e.ctrlKey || e.altKey || /INPUT|SELECT|TEXTAREA/.test(e.target.tagName) || document.querySelector('dialog[open]')) return;
			if (e.code in KEYMAP) { e.preventDefault(); if (!e.repeat) press(12 * (P.octave + 1) + KEYMAP[e.code]); }
			else if (e.code === 'KeyZ' || e.code === 'KeyX') { e.preventDefault(); octave(e.code === 'KeyX' ? 1 : -1); }
		});
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
	let actx = null, inflight = false, queued = null;
	function status(msg, err) { const el = root.querySelector('#pg-status'); el.textContent = msg; el.classList.toggle('err', !!err); }
	async function play(notes) {
		const c = current();
		if (!c) return status('Pick an instrument first.', true);
		if (inflight) { queued = notes; return; }
		inflight = true;
		if (!P.loaded.has(c.id)) status(`Loading ${c.label}…`);
		try {
			const body = { notes, tail: 1.2, ...(c.song ? { song: c.song, track: c.track } : { plugin: c.plugin, preset: c.preset }) };
			let r = await fetch('api/play', { method: 'POST', headers: postHeaders(), body: JSON.stringify(body) });
			if (r.status === 403) { await postJson('api/preview/cancel', { song: '' }).catch(() => {}); r = await fetch('api/play', { method: 'POST', headers: postHeaders(), body: JSON.stringify(body) }); }
			if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || 'the server answered ' + r.status); }
			const info = JSON.parse(r.headers.get('X-Wavelength-Play') || '{}');
			actx ??= new (window.AudioContext || window.webkitAudioContext)();
			if (actx.state === 'suspended') await actx.resume();
			const src = actx.createBufferSource();
			src.buffer = await actx.decodeAudioData(await r.arrayBuffer());
			src.connect(actx.destination); src.start();
			P.loaded.add(c.id);
			status(`${notes.map(n => keyName(n.key)).join(' ')} · ${info.plugin}${info.preset ? ' · ' + info.preset : ''} · ${info.loaded ? 'loaded in ' : ''}${info.ms} ms`);
		} catch (e) { status(`Could not play: ${e.message}`, true); }
		finally { inflight = false; if (queued) { const q = queued; queued = null; play(q); } }
	}
	function press(k) {
		const notes = CHORDS[P.chord].map(i => ({ key: Math.min(127, k + i), vel: P.vel, start: 0, dur: P.len }));
		const el = root.querySelector(`[data-k="${k}"]`);
		if (el) { el.classList.add('down'); setTimeout(() => el.classList.remove('down'), 180); }
		play(notes);
	}
	const preview = () => press(12 * (P.octave + 1));   // a C when a sound is picked

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
		show() { if (!built) build(); else if (P.source === 'song') renderSource(); document.title = 'Playground · Wavelength'; },
	};
})();
