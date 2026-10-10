/* Playground: play any installed instrument, or a song's track, from an on-screen piano or the computer
 * keyboard (ui/keys.js: live for CLAP and VST3, note by note for the rest).
 *
 * Uses the page's globals: $, esc, state.
 */
(() => {
	const keyName = WLKeys.keyName;
	const P = { groups: [], inst: null, spec: '', presets: [], preset: '', octave: 4, vel: .85, len: 4, chord: 'single', source: 'installed', song: '', track: '', q: '', pq: '' };
	try { Object.assign(P, JSON.parse(localStorage.getItem('wl-playground') || '{}'), { groups: [], presets: [] }); } catch {}
	const keep = () => { try { const { spec, preset, octave, vel, len, chord, source, song, track } = P; localStorage.setItem('wl-playground', JSON.stringify({ spec, preset, octave, vel, len, chord, source, song, track })); } catch {} };
	let built = false, root, keys;

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
				<div class="pt-body"><div class="pt-rail" id="pg-ptypes"></div><div class="pg-list" id="pg-plist"><div class="empty">Pick an instrument.</div></div></div>
			</section>
			<section class="card pg-play">
				<h2><span id="pg-now">Pick an instrument</span><span class="r" id="pg-formats"></span></h2>
				<div id="pg-keys"></div>
			</section>
		</div>`;
		const on = (sel, ev, fn) => root.querySelector(sel).addEventListener(ev, fn);
		on('#pg-q', 'input', e => { P.q = e.target.value; renderInstruments(); });
		on('#pg-pq', 'input', e => { P.pq = e.target.value; renderPresets(); });
		on('#pg-list', 'click', e => { const b = e.target.closest('[data-g]'); if (b) pickInstrument(P.groups[+b.dataset.g]); });
		on('#pg-ptypes', 'click', e => { const b = e.target.closest('[data-type]'); if (b) { P.ptype = b.dataset.type; renderPresets(); } });
		on('#pg-plist', 'click', e => { const b = e.target.closest('[data-p]'); if (b) { P.preset = b.dataset.p; keep(); renderPresets(); renderNow(); keys.ensure(); } });
		on('#pg-formats', 'click', e => { const b = e.target.closest('[data-spec]'); if (b) { P.spec = b.dataset.spec; P.preset = ''; keep(); loadPresets(); renderNow(); } });
		on('.pg-seg', 'click', e => { const b = e.target.closest('[data-src]'); if (b) { P.source = b.dataset.src; keep(); renderSource(); } });
		on('#pg-song', 'change', e => { P.song = e.target.value; P.track = ''; keep(); loadSongTracks(); });
		on('#pg-tracks', 'click', e => { const b = e.target.closest('[data-t]'); if (b) { P.track = b.dataset.t; keep(); renderSongTracks(); renderNow(); keys.ensure(); } });
		keys = WLKeys.create({ page: root, mount: root.querySelector('#pg-keys'), current, settings: P, save: keep });
		built = true;
		renderSource();
		loadInstruments();
	}

	/* ---------- instruments ---------- */
	async function loadInstruments() {
		const list = root.querySelector('#pg-list');
		list.innerHTML = '<div class="empty">Reading the installed plugins…</div>';
		P.groups = await WLKeys.instruments();
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
		P.presets = r.presets || []; P.types = WLPresets.classify(P.presets); P.ptype = '';
		renderPresets();
	}
	function renderPresets() {
		const q = P.pq.trim().toLowerCase(), all = P.presets;
		const types = P.types || [];
		const found = all.map((p, i) => i).filter(i => !q || (all[i].name + ' ' + (all[i].category || '') + ' ' + types[i] + ' ' + (all[i].features || []).join(' ')).toLowerCase().includes(q));
		const railHtml = WLPresets.rail(types, found, P.ptype || ''), railEl = root.querySelector('#pg-ptypes');
		railEl.innerHTML = railHtml; railEl.hidden = !railHtml; railEl.parentElement.classList.toggle('flat', !railHtml);
		const hits = found.filter(i => !P.ptype || types[i] === P.ptype).map(i => all[i]);
		root.querySelector('#pg-pcount').textContent = all.length ? (q || P.ptype ? `${hits.length} of ${all.length}` : all.length) : '';
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

	window.WLPlay = {
		show() { if (!built) build(); else if (P.source === 'song') renderSource(); document.title = 'Playground · Wavelength'; if (current()) keys.ensure(); },
		hide() { if (built) { keys.releaseAll(); keys.close(); } },
		stats: () => keys?.stats() ?? null,
	};
	addEventListener('pagehide', () => keys?.close());
})();
