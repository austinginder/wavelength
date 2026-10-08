/* Sounds: tune a song's instruments by ear before an agent writes its parts. Each track gets an instrument and a
 * preset, and its knobs (the plugin's parameters, in its own modules) turn while it plays: CLAP and VST3 instruments
 * hear a turn at once (a parameter event to the live worker), others on the next note. Everything saves to the
 * song's sounds.json as it changes (only the knobs that differ from the preset, in the plugin's own text where it
 * reads back the same), which every render applies on top of job.json. The brief and a prompt to copy hand the
 * song to an agent.
 *
 * api/sounds lists and changes the sounds; api/knobs loads an instrument's knobs in a __knobs worker, and
 * api/knobs/ask turns values into the plugin's text ("917 Hz") and back.
 *
 * Uses the page's globals: $, esc, state, postJson, loadSongs, go.
 */
(() => {
	const S = { slug: '', data: null, sel: '', K: null, vals: new Map(), base: new Map(), text: new Map(), q: '', changedOnly: false, open: new Set(), octave: 4, vel: .85, len: 4, chord: 'single', tracks: {} };
	try { Object.assign(S, JSON.parse(localStorage.getItem('wl-sounds') || '{}'), { data: null, K: null, vals: new Map(), base: new Map(), text: new Map(), open: new Set() }); } catch {}
	const keep = () => { try { const { slug, octave, vel, len, chord, tracks, changedOnly } = S; localStorage.setItem('wl-sounds', JSON.stringify({ slug, octave, vel, len, chord, tracks, changedOnly })); } catch {} };
	const ROLES = ['Lead', 'Bass', 'Pad', 'Pluck', 'Arp', 'Keys', 'Stab', 'Chords', 'Sub', 'FX'];
	let root, keys, built = false, saveTimer = 0, saving = Promise.resolve(), textTimer = 0;
	const pendingText = new Map();

	function build() {
		root = $('#soundsview');
		root.innerHTML = `
		<div class="sd-head">
			<div class="sd-title"><h1>Sounds</h1>
				<select id="sd-song" title="The song (project) whose sounds these are"></select>
				<button class="ed-open" id="sd-new">New project</button>
				<button class="ed-open" id="sd-opensong" title="The song's page: arrangement, renders, the agent's work">Song page</button></div>
			<p>Give each part an instrument and preset, then turn its knobs while you play it. Changes save to the song's <code>sounds.json</code> as you go, and every render keeps them; then hand the song to an agent with the prompt below.</p>
		</div>
		<div class="sd-grid">
			<section class="card sd-tracks">
				<h2>Tracks <button class="ed-open" id="sd-add">Add</button></h2>
				<div class="sd-tlist" id="sd-tlist"></div>
				<label class="sd-tempo">Tempo <input type="number" id="sd-tempo" min="20" max="400" step="1"> BPM</label>
			</section>
			<section class="sd-main">
				<div class="card sd-inst" id="sd-inst"></div>
				<div class="sd-modules" id="sd-modules"></div>
			</section>
		</div>
		<section class="card sd-keys"><div id="sd-keys"></div></section>
		<section class="card sd-brief">
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
		keys = WLKeys.create({ page: root, mount: root.querySelector('#sd-keys'), current, settings: S, save: keep });
		built = true;
	}

	/* ---------- the song and its tracks ---------- */
	async function open(slug) {
		if (!slug) { S.slug = ''; S.data = null; render(); return; }
		if (slug !== S.slug) { keys?.releaseAll(); keys?.close(); S.K = null; }
		S.slug = slug; keep();
		history.replaceState(null, '', '?view=sounds&song=' + encodeURIComponent(slug));
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
		const sel = root.querySelector('#sd-song');
		sel.innerHTML = '<option value="">Pick a song…</option>' + state.songs.map(s => `<option value="${esc(s.slug)}">${esc(s.title || s.slug)}</option>`).join('');
		sel.value = S.slug;
		const d = S.data;
		root.querySelector('#sd-tempo').value = d && typeof d.tempo === 'number' ? d.tempo : '';
		root.querySelector('#sd-tempo').disabled = !d?.hasJob || (d.tempo != null && typeof d.tempo !== 'number');
		root.querySelector('#sd-add').disabled = !d;
		root.querySelector('#sd-opensong').disabled = !S.slug;
		const brief = root.querySelector('#sd-brief');
		if (document.activeElement !== brief) brief.value = d?.brief || '';
		root.querySelector('#sd-tlist').innerHTML = !d ? '<div class="empty">Pick a song above, or start a new project.</div>'
			: d.tracks.map(t => `<button class="pg-item ${t.name === S.sel ? 'sel' : ''}" data-t="${esc(t.name)}">
				<span class="n">${esc(t.name)}${t.custom ? ' <i class="sd-dot" title="set here (sounds.json)"></i>' : ''}</span>
				<span class="v">${esc(soundLine(t))}</span>
				<span class="f">${t.notes ? t.notes + ' notes' : t.clips ? 'clips' : ''}</span></button>`).join('')
			+ (d.tracks.length ? '' : '<div class="empty">No tracks yet. Add the first instrument.</div>')
			+ (d.generator ? '<div class="sd-hint">A script writes this song\'s job.json. Sounds set here stay in sounds.json, which every render applies on top.</div>' : '');
		prompt();
	}
	const plainName = p => String(p || '').replace(/^(clap|vst3|vst2|au):/, '');
	const soundLine = t => [plainName(t.plugin), t.preset ? String(t.preset).split('/').pop() : '', t.params && Object.keys(t.params).length ? Object.keys(t.params).length + ' set' : ''].filter(Boolean).join(' · ');
	const row = name => S.data?.tracks.find(t => t.name === name);

	async function pick(name) {
		S.sel = name; S.tracks[S.slug] = name; keep();
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
	function set(id, v, { redraw = true } = {}) {
		const p = S.byId.get(id);
		v = Math.min(Math.max(v, Math.min(p.min, p.max)), Math.max(p.min, p.max));
		if (p.stepped) v = Math.round(v);
		if (v === S.vals.get(id)) return;
		S.vals.set(id, v);
		keys.param(id, v);   // heard at once when it plays live
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
		let spec = plugin || '', chosen = preset || '', presets = [];
		const specOf = g => g.formats[0].format === 'clap' && g.formats.filter(f => f.format === 'clap').length === 1 ? g.name : g.formats[0].spec;
		const groupOf = s => groups.find(g => g.formats.some(f => f.spec === s) || g.name.toLowerCase() === plainName(s).toLowerCase());
		let out = null;
		await WLUI.modal({ title, cls: 'sd-dlg', body: `
			${name != null ? `<label class="wl-field"><span>Track name</span><input name="name" value="${esc(name)}" list="sd-roles" autocomplete="off" spellcheck="false"><datalist id="sd-roles">${ROLES.map(r => `<option value="${r}">`).join('')}</datalist></label>` : ''}
			<div class="sd-pickgrid ${presetOnly ? 'one' : ''}">
				${presetOnly ? '' : `<div><input type="search" data-q="i" placeholder="Search instruments" autocomplete="off" spellcheck="false"><div class="pg-list" data-l="i"></div></div>`}
				<div><input type="search" data-q="p" placeholder="Search presets" autocomplete="off" spellcheck="false"><div class="pg-list" data-l="p"></div></div>
			</div>`,
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
					const hits = presets.filter(p => !q || (p.name + ' ' + (p.category || '')).toLowerCase().includes(q));
					const rowP = (n, sub) => `<button type="button" class="pg-item ${n === chosen ? 'sel' : ''}" data-p="${esc(n)}"><span class="n">${esc(n || 'Default sound')}</span><span class="v">${esc(sub || '')}</span></button>`;
					d.querySelector('[data-l="p"]').innerHTML = !spec ? '<div class="empty">Pick an instrument.</div>'
						: rowP('', 'what the plugin loads with') + hits.slice(0, 400).map(p => rowP(p.name, p.category)).join('') + (hits.length > 400 ? `<div class="empty">${hits.length - 400} more: search to narrow them down.</div>` : '');
				};
				const loadP = async () => { presets = []; drawP(); if (!spec) return; const s = spec; const list = await presetsOf(s); if (s === spec) { presets = list; drawP(); } };
				iq?.addEventListener('input', drawI);
				pq.addEventListener('input', drawP);
				d.querySelector('[data-l="i"]')?.addEventListener('click', e => { const b = e.target.closest('[data-g]'); if (!b) return; spec = specOf(groups[+b.dataset.g]); chosen = ''; drawI(); loadP(); });
				d.querySelector('[data-l="p"]').addEventListener('click', e => { const b = e.target.closest('[data-p]'); if (!b) return; chosen = b.dataset.p; drawP(); });
				d.querySelector('[data-l="p"]').addEventListener('dblclick', e => { if (e.target.closest('[data-p]')) d.querySelector('button[value="ok"]').click(); });
				drawI(); loadP();
				(d.querySelector('input[name="name"]') || iq || pq).focus();
			} });
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
${typeof d.tempo === 'number' ? `\nTempo: ${d.tempo} BPM.\n` : ''}${brief ? `\nBrief (also in brief.md):\n${brief}\n` : ''}`;
	}
	async function copyPrompt() {
		const box = root.querySelector('#sd-prompt'), b = root.querySelector('#sd-copy');
		try { await navigator.clipboard.writeText(box.value); }
		catch { box.select(); document.execCommand('copy'); }
		b.textContent = 'Copied'; setTimeout(() => b.textContent = 'Copy', 1500);
	}

	function current() {
		const t = row(S.sel);
		return t && S.slug ? { song: S.slug, track: t.name, label: `${t.name} (${plainName(t.plugin)}${t.preset ? ' · ' + t.preset : ''})`, id: 'sd|' + S.slug + '|' + t.name + '|' + soundKey(t) } : null;
	}

	window.WLSounds = {
		show(slug) {
			if (!built) build();
			document.title = 'Sounds · Wavelength';
			const want = slug || S.slug || state.slug || '';
			if (want !== S.slug || !S.data) open(want); else { render(); keys.ensure(); }
		},
		hide() { if (built) { save(); keys.releaseAll(); keys.close(); } },
		songsChanged() { if (built && !root.hidden) render(); },
		stats: () => keys?.stats() ?? null,
	};
	addEventListener('pagehide', () => { if (built) { save(); keys.close(); } });
})();
