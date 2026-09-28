/* Arrangement editor for the Wavelength live viewer.
 *
 * You look, listen, loop and select, then leave comments pinned to bars, tracks and notes (review.json,
 * for the agent). Notes can be moved, deleted and added: the draft is heard through the track's own
 * instrument and saved to the song's edits.json, which the engine applies on top of whatever makes
 * job.json (a make-job.py rewrites job.json, never the edits); the Render button renders the result.
 *
 * Uses the page's globals: state, audio, tempoFn, hue, esc, fmtTime, stemFor, toggleSolo, setSrc.
 */
(() => {
	const HEAD = 210, RULER = 24, SECT = 20, CHORD = 24, TOPH = RULER + SECT + CHORD, LANE = 42, LANE_OPEN = 170;
	const NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B'];
	const keyName = k => NAMES[((k % 12) + 12) % 12] + (Math.floor(k / 12) - 1);
	// canvas colours: a theme token (hex) at some opacity (canvas fillStyle can't be trusted with color-mix())
	const alpha = (hex, a) => { const m = String(hex).trim().match(/^#([0-9a-f]{6})$/i); if (!m) return hex; const n = parseInt(m[1], 16); return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a})`; };
	const E = {
		zoom: 26, dirty: true, data: null, harmony: null, comments: [], expanded: new Set(), hover: null,
		sel: { b0: null, b1: null, tracks: new Set(), notes: [] }, loop: false, drag: null, userScrollAt: 0, follow: true,
		edits: new Map(),   // draft changes of existing notes, "t:n" -> {dk, db, del}: heard, drawn, saved to edits.json
		adds: [],           // draft new notes: {t, beat, key, dur, vel}
		keys: { on: false, octave: 4, track: null },
	};
	try { E.hear = localStorage.getItem('wl-hear') !== '0'; } catch { E.hear = true; }
	let dlg, scroller, canvas, space, tip;
	const HINT = 'Saved to review.json in the song folder with the bars, tracks, notes and time you selected.';

	/* ---------- markup ---------- */
	function build() {
		dlg = document.createElement('dialog');
		dlg.className = 'ed';
		dlg.innerHTML = `
		<div class="ed-grid">
			<div class="ed-bar">
				<h3 id="ed-title"></h3>
				<button class="ed-btn play" id="ed-play" title="Play / pause (Space)"><svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor" id="ed-playicon"><path d="M6 4l14 8-14 8z"/></svg></button>
				<div class="ed-clock" id="ed-clock">0:00<small>bar 1.1</small></div>
				<button class="ed-btn" id="ed-loop" title="Loop the selected bars (L)">
					<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M17 2l4 4-4 4"/><path d="M3 11V9a3 3 0 0 1 3-3h15"/><path d="M7 22l-4-4 4-4"/><path d="M21 13v2a3 3 0 0 1-3 3H3"/></svg>Loop</button>
				<button class="ed-btn" id="ed-live" disabled title="Loop the selected bars live: change the mix and the notes and hear each change within a second or two (V)">● Live</button>
				<button class="ed-btn on" id="ed-follow" title="Keep the playhead in view">Follow</button>
				<span class="ed-group"><button class="ed-btn" id="ed-zout" title="Zoom out (-)">−</button><button class="ed-btn" id="ed-fit" title="Whole song (F)">Fit</button><button class="ed-btn" id="ed-zin" title="Zoom in (+)">+</button></span>
				<span class="ed-legend" id="ed-legend"></span>
				<button class="ed-btn" id="ed-render" title="Render the whole song again (job.json with edits.json) into its out folder">Render</button>
				<span class="ed-status" id="ed-status"></span>
				<button class="wl-cancel" id="ed-cancel" hidden title="Stop this render">Cancel</button>
				<span class="sp"></span>
				<select id="ed-audio" title="What plays"></select>
				<button class="ed-btn" id="ed-close" title="Close (Esc)">✕</button>
			</div>
			<div class="ed-main">
				<div class="ed-scroll" id="ed-scroll"><canvas id="ed-canvas"></canvas><div class="ed-space" id="ed-space"></div></div>
				<div class="ed-tip" id="ed-tip" hidden></div>
			</div>
			<div class="ed-side">
				<section class="ed-livemix" id="ed-livemix" hidden></section>
				<section>
					<h4>Selection <span class="r"><button class="ed-btn" id="ed-clear" style="height:22px;font-size:11px">Clear</button></span></h4>
					<div class="ed-ref none" id="ed-ref">Drag across the ruler to pick bars, click a track name, click or drag over notes.</div>
					<div class="ed-actions">
						<button class="ed-btn amber" id="ed-preview" disabled title="Render the selected bars through the mix and loop them (P)">▶ Preview these bars</button>
						<button class="ed-btn" id="ed-sloop" disabled>Loop these bars</button>
						<button class="ed-btn" id="ed-solo" disabled>Solo track</button>
						<button class="ed-btn" id="ed-copy" disabled>Copy reference</button>
					</div>
				</section>
				<section class="ed-live">
					<h4>Notes <span class="r"><label title="Click a note to hear it through its track's instrument"><input type="checkbox" id="ed-hear"> hear notes</label></span></h4>
					<div class="ed-hint" id="ed-hearinfo">Click a note to hear it through its track's instrument. ↑ ↓ move selected notes a semitone (Shift: an octave), ← → a 16th (Shift: a beat), Delete removes them; double-click an open piano roll to add one.</div>
					<div class="ed-actions"><button class="ed-btn" id="ed-keys" title="Play the selected track's instrument from the computer keyboard (A-K = C-C, W E T Y U = sharps, Z / X = octave)">Play keys</button></div>
					<div id="ed-edits" hidden>
						<div class="ed-ref" id="ed-editlist"></div>
						<div class="ed-actions"><button class="ed-btn primary" id="ed-edsave" title="Write them to edits.json in the song folder; Render to hear them in the mix">Save to song</button><button class="ed-btn" id="ed-edhear">▶ Hear</button><button class="ed-btn" id="ed-edagent" title="Leave them as a comment for the agent to make in the song's source instead">Send to agent</button><button class="ed-btn" id="ed-eddiscard">Discard</button></div>
					</div>
					<div id="ed-saved" class="ed-saved" hidden></div>
				</section>
				<section>
					<h4>Comment for the agent</h4>
					<textarea id="ed-text" placeholder="What should change here? (Cmd+Enter to save)"></textarea>
					<div class="ed-actions"><button class="ed-btn primary" id="ed-save">Save comment</button></div>
					<div class="ed-hint">${HINT}</div>
				</section>
				<h4 style="padding:12px 14px 0;margin:0">Comments <span class="r" id="ed-count"></span></h4>
				<div class="ed-list" id="ed-list"></div>
			</div>
		</div>`;
		document.body.appendChild(dlg);
		scroller = dlg.querySelector('#ed-scroll'); canvas = dlg.querySelector('#ed-canvas'); space = dlg.querySelector('#ed-space'); tip = dlg.querySelector('#ed-tip');
		const on = (id, ev, fn) => dlg.querySelector(id).addEventListener(ev, fn);
		on('#ed-close', 'click', () => dlg.close());
		on('#ed-play', 'click', () => { if (WLLive.active) { WLLive.toggle(); icon(); } else audio.paused ? audio.play() : audio.pause(); });
		on('#ed-live', 'click', toggleLive);
		WLLive.mount($d('#ed-livemix'));
		WLLive.onchange = () => { icon(); $d('#ed-live').classList.toggle('on', WLLive.active); dirty(); };
		WLLive.onsend = async (text, tracks, from, to) => {   // the live mixer's changes, as a comment for the agent
			const b0 = (from - 1) * E.data.bpb, b1 = (to - 1) * E.data.bpb;
			const res = await post({ op: 'add', text, ref: `bars ${from}-${to - 1} · live mix · ${tracks.join(', ')}`, report: state.song.reportPath || '',
				bars: [from, to - 1], beats: [b0, b1], time: [+E.data.toSec(b0).toFixed(2), +E.data.toSec(b1).toFixed(2)], tracks });
			if (res?.comments) { E.comments = res.comments; renderComments(); badge(); dirty(); return true; }
			return false;
		};
		on('#ed-loop', 'click', () => setLoop(!E.loop));
		on('#ed-sloop', 'click', () => { setLoop(true); seekBeat(E.sel.b0, true); });
		on('#ed-preview', 'click', previewSelection);
		on('#ed-follow', 'click', () => { E.follow = !E.follow; dlg.querySelector('#ed-follow').classList.toggle('on', E.follow); });
		on('#ed-zin', 'click', () => zoomAt(1.4));
		on('#ed-zout', 'click', () => zoomAt(1 / 1.4));
		on('#ed-fit', 'click', fit);
		on('#ed-clear', 'click', () => { clearSel(); });
		on('#ed-copy', 'click', () => navigator.clipboard?.writeText(reference().text));
		on('#ed-solo', 'click', () => {   // with bars selected: a quick preview of this track in those bars; else the whole track
			const i = [...E.sel.tracks][0] ?? E.sel.notes[0]?.t;
			if (i == null) return;
			if (E.sel.b0 != null) previewSelection([E.data.tracks[i].name]); else toggleSolo(i);
			syncAudioSelect(); dirty();
		});
		on('#ed-save', 'click', save);
		on('#ed-cancel', 'click', () => { if (state.rendering) cancelRender(); else cancelPreview(); syncAudioSelect(); dirty(); });
		canvas.addEventListener('contextmenu', contextMenu);
		$d('#ed-hear').checked = E.hear;
		on('#ed-hear', 'change', e => { E.hear = e.target.checked; try { localStorage.setItem('wl-hear', E.hear ? '1' : '0'); } catch {} });
		on('#ed-keys', 'click', () => setKeys(!E.keys.on));
		on('#ed-edhear', 'click', () => hearEdits());
		on('#ed-edsave', 'click', saveToSong);
		on('#ed-edagent', 'click', saveEdits);
		on('#ed-render', 'click', () => renderSong());
		on('#ed-saved', 'click', e => { const a = e.target.closest('[data-eop]')?.dataset.eop; if (a) editsOp(a); });
		on('#ed-eddiscard', 'click', () => { E.edits.clear(); E.adds = []; editsChanged(); });
		on('#ed-list', 'click', e => {   // a comment: jump to what it points at; its buttons change it
			const c = e.target.closest('.ed-c'); if (!c) return;
			const act = e.target.closest('button')?.dataset.act, cm = E.comments.find(x => x.id === c.dataset.id);
			if (!cm) return;
			if (act === 'status') return op({ op: 'status', id: cm.id, status: cm.status === 'done' ? 'open' : 'done' });
			if (act === 'delete') { if (confirm('Delete this comment?')) op({ op: 'delete', id: cm.id }); return; }
			const d = E.data;
			E.sel = { b0: cm.beats?.[0] ?? null, b1: cm.beats?.[1] ?? null, tracks: new Set((cm.tracks || []).map(n => d.tracks.findIndex(t => t.name === n)).filter(i => i >= 0)), notes: [] };
			(cm.notes || []).forEach(n => {
				const ti = d.tracks.findIndex(t => t.name === n.track); if (ti < 0) return;
				const ni = d.tracks[ti].beatsNotes.findIndex(x => Math.abs(x[0] - n.beat) < 1e-3 && x[2] === n.midi);
				if (ni >= 0) E.sel.notes.push({ t: ti, n: ni });
			});
			if (cm.beats) { scroller.scrollLeft = Math.max(0, cm.beats[0] * E.zoom - 120); seekBeat(cm.beats[0], false); }
			selChanged();
		});
		on('#ed-text', 'keydown', e => { if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) { e.preventDefault(); save(); } });
		on('#ed-audio', 'change', e => {
			const v = e.target.value;
			if (v.startsWith('stem:')) toggleSolo(+v.slice(5), 'dry');
			else if (v.startsWith('solo:')) toggleSolo(+v.slice(5), 'mix');
			else if (v !== 'preview') { $('#audiosel').value = v; backToMix(songSecNow(), !audio.paused); }
			dirty();
		});
		dlg.addEventListener('cancel', e => { if (E.sel.b0 != null || E.sel.tracks.size || E.sel.notes.length) { e.preventDefault(); clearSel(); } });
		dlg.addEventListener('close', () => { E.loop = false; WLLive.stop(); });
		scroller.addEventListener('scroll', () => { E.userScrollAt = performance.now(); dirty(); });
		scroller.addEventListener('wheel', e => {
			if (!(e.ctrlKey || e.metaKey)) return;
			e.preventDefault();
			zoomAt(e.deltaY < 0 ? 1.15 : 1 / 1.15, e.clientX - scroller.getBoundingClientRect().left);
		}, { passive: false });
		canvas.addEventListener('mousedown', down);
		addEventListener('mousemove', move);
		addEventListener('mouseup', up);
		canvas.addEventListener('mouseleave', () => { E.hover = null; tip.hidden = true; dirty(); });
		canvas.addEventListener('dblclick', e => {
			const p = pos(e);
			if (p.x > HEAD && p.y < TOPH) return seekBeat(beatAt(p.x), true);
			if (p.x <= HEAD || noteAt(p)) return;
			const lane = laneAt(p.y + scroller.scrollTop);   // an open piano roll: a new note at that key, snapped to a 16th
			if (!lane || !E.expanded.has(lane.i)) return hearInfo('Open the lane as a piano roll (▸ or right-click) to add notes there.');
			const t = E.data.tracks[lane.i], kg = keyGeom(t, lane.top - scroller.scrollTop, lane.h);
			for (let k = kg.lo; k <= kg.hi; k++) {
				const y = kg.y(k);
				if (p.y >= y - 1 && p.y <= y + kg.row + 1) {
					const a = { t: lane.i, beat: Math.max(0, Math.round(beatAt(p.x) * 4) / 4), key: k, dur: 1, vel: .8 };
					E.adds.push(a); editsChanged();
					hear(lane.i, [{ key: k, vel: .8, start: 0, dur: Math.min(2, E.data.toSec(a.beat + 1) - E.data.toSec(a.beat)) }]);
					return;
				}
			}
		});
		// on the document: focus can sit outside the dialog (after a menu or a click on the backdrop) while it is open
		document.addEventListener('keydown', e => { if (dlg.open && !document.querySelector('dialog.wl-dlg[open]') && !e.defaultPrevented) key(e); });
		new ResizeObserver(() => dirty()).observe(scroller);
		audio.addEventListener('play', icon); audio.addEventListener('pause', icon);
	}
	const $d = s => dlg.querySelector(s);
	function icon() { if (dlg) $d('#ed-playicon').innerHTML = !(WLLive.active ? WLLive.playing() : !audio.paused) ? '<path d="M6 4l14 8-14 8z"/>' : '<path d="M6 4h4v16H6zM14 4h4v16h-4z"/>'; }
	// Live: the selected bars loop through Web Audio and every change to the mix or the notes is heard
	function toggleLive() {
		if (WLLive.active) { WLLive.stop(); selChanged(); return; }
		if (E.sel.b0 == null) return;
		audio.pause(); setLoop(false);
		WLLive.start(state.slug, barOf(E.sel.b0), barOf(E.sel.b1 - 1e-6) + 1);
		hearInfo('Live: note edits save to edits.json as you make them and play on the next pass.');
		dirty();
	}

	/* ---------- data ---------- */
	function prepare() {
		const job = state.song?.job;
		if (!job) { E.data = null; return; }
		const toSec = tempoFn(job.tempo);
		const ts = job.timeSignature || [4, 4], bpb = ts[0] * 4 / ts[1];
		const secToBeat = s => { let lo = 0, hi = 1; while (toSec(hi) < s) hi *= 2; for (let i = 0; i < 50; i++) { const m = (lo + hi) / 2; toSec(m) < s ? lo = m : hi = m; } return (lo + hi) / 2; };
		const tracks = job.tracks.map((t, i) => {
			const notes = t.notes.map(n => n[4] ? [secToBeat(n[0]), secToBeat(n[0] + n[1]) - secToBeat(n[0]), n[2], n[3]] : [n[0], n[1], n[2], n[3]]);
			const keys = notes.map(n => n[2]);
			const lufs = state.song.report?.tracks?.find(r => r.name === t.name)?.lufs;
			return { ...t, idx: i, beatsNotes: notes, lo: keys.length ? Math.min(...keys) : 60, hi: keys.length ? Math.max(...keys) : 60, lufs };
		});
		const endBeat = Math.max(bpb, ...tracks.flatMap(t => t.beatsNotes.map(n => n[0] + n[1])));
		const leadIn = state.song.report?.leadIn ?? job.leadIn ?? 0;
		const markers = (job.markers || []).map(m => ({ name: m.name, beat: +m.beat || 0 })).sort((a, b) => a.beat - b.beat);
		E.data = { tracks, bpb, toSec, secToBeat, endBeat, bars: Math.ceil(endBeat / bpb), leadIn, markers };
	}
	async function loadExtras() {
		const slug = state.slug;
		const [h, r] = await Promise.all([
			fetch('api/harmony?song=' + encodeURIComponent(slug)).then(r => r.json()).catch(() => null),
			fetch('api/review?song=' + encodeURIComponent(slug)).then(r => r.json()).catch(() => ({ comments: [] })),
		]);
		if (slug !== state.slug) return;
		E.harmony = h && h.ok ? h : null;
		E.comments = r.comments || [];
		renderComments(); legend(); badge(); dirty();
	}

	/* ---------- draft edits ---------- */
	// a note as the draft has it: [start beat, length, key, vel]
	function eff(t, n) {
		const x = E.data.tracks[t].beatsNotes[n], ed = E.edits.get(t + ':' + n);
		return ed ? [Math.max(0, x[0] + ed.db), x[1], Math.max(0, Math.min(127, x[2] + ed.dk)), x[3]] : x;
	}

	/* ---------- geometry ---------- */
	const laneH = i => E.expanded.has(i) ? LANE_OPEN : LANE;
	function laneTop(i) { let y = TOPH; for (let k = 0; k < i; k++) y += laneH(k); return y; }
	function laneAt(y) { let top = TOPH; for (let i = 0; i < E.data.tracks.length; i++) { const h = laneH(i); if (y >= top && y < top + h) return { i, top, h }; top += h; } return null; }
	const X = b => HEAD + b * E.zoom - scroller.scrollLeft;
	const beatAt = x => Math.max(0, (x - HEAD + scroller.scrollLeft) / E.zoom);
	function pos(e) { const r = canvas.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top, cx: e.clientX, cy: e.clientY }; }
	// y of a key inside a lane: collapsed lanes squeeze the track's range, open lanes are a piano roll
	function keyGeom(t, top, h) {
		const open = E.expanded.has(t.idx);
		const lo = open ? t.lo - 2 : t.lo, hi = open ? t.hi + 2 : t.hi, range = Math.max(open ? 14 : 12, hi - lo + 1);
		const pad = open ? 6 : 5, ih = h - pad * 2, row = open ? ih / range : 3;
		const y = k => open ? top + pad + (hi - k) * row + (ih - (hi - lo + 1) * row) / 2 : top + pad + ih - ((k - lo + (range - (hi - lo)) / 2) / range) * (ih - 3) - 3;
		return { y, row: open ? Math.max(2, row - 1) : 3, lo, hi, open };
	}
	const barOf = b => Math.floor(b / E.data.bpb + 1e-9) + 1;
	// bar.beat, plus .sixteenth when off the beat (DAW style): 42.3 = bar 42 beat 3, 42.3.3 = its third 16th
	const barBeat = b => {
		const bar = barOf(b), inBar = Math.round((b - (bar - 1) * E.data.bpb) * 1000) / 1000, beat = Math.floor(inBar + 1e-9), frac = inBar - beat;
		if (frac < 1e-6) return `${bar}.${beat + 1}`;
		const six = frac * 4;
		return Math.abs(six - Math.round(six)) < 1e-6 ? `${bar}.${beat + 1}.${Math.round(six) + 1}` : `${bar}.${beat + 1}+${+frac.toFixed(3)}`;
	};
	const audioBeat = () => E.data.secToBeat(Math.max(0, songSecNow()));
	function seekBeat(b, play) { if (!audio.src) return; seekSongSec(E.data.toSec(b), play); dirty(); }

	/* ---------- drawing ---------- */
	function dirty() { E.dirty = true; }
	function sizeSpace() {
		const d = E.data; if (!d) return;
		let h = TOPH; d.tracks.forEach((t, i) => h += laneH(i));
		space.style.width = (HEAD + d.endBeat * E.zoom + 60) + 'px';
		space.style.height = (h + 30) + 'px';
	}
	function draw() {
		const d = E.data, w = scroller.clientWidth, h = scroller.clientHeight, dpr = devicePixelRatio || 1;
		if (canvas.width !== w * dpr || canvas.height !== h * dpr) { canvas.width = w * dpr; canvas.height = h * dpr; canvas.style.width = w + 'px'; canvas.style.height = h + 'px'; }
		const g = canvas.getContext('2d'), css = getComputedStyle(document.documentElement), v = n => css.getPropertyValue(n).trim();
		g.setTransform(dpr, 0, 0, dpr, 0, 0);
		g.fillStyle = v('--panel'); g.fillRect(0, 0, w, h);
		if (!d) { g.fillStyle = v('--muted'); g.font = '13px "Instrument Sans", sans-serif'; g.fillText('No job.json yet.', 20, 40); return; }
		const sy = scroller.scrollTop, bpb = d.bpb;
		const b0 = Math.max(0, beatAt(HEAD)), b1 = beatAt(w);
		const accent = v('--accent'), muted = v('--muted'), line = v('--line'), text = v('--text');

		// ---- lanes (clipped below the top rows and right of the headers) ----
		g.save(); g.beginPath(); g.rect(HEAD, TOPH, w - HEAD, h - TOPH); g.clip();
		let top = TOPH - sy;
		d.tracks.forEach((t, i) => {
			const lh = laneH(i);
			if (top + lh > TOPH && top < h) {
				const selT = E.sel.tracks.has(i);
				g.fillStyle = selT ? alpha(accent, 0.08) : i % 2 ? v('--lane') : 'transparent';
				g.fillRect(HEAD, top, w - HEAD, lh);
				const kg = keyGeom(t, top, lh);
				if (kg.open) {   // piano-roll rows: black keys shaded, C lines
					for (let k = kg.lo; k <= kg.hi; k++) {
						const y = kg.y(k), bk = [1, 3, 6, 8, 10].includes(((k % 12) + 12) % 12);
						if (bk) { g.fillStyle = v('--lane'); g.fillRect(HEAD, y, w - HEAD, kg.row + 1); }
						if (k % 12 === 0) { g.fillStyle = line; g.fillRect(HEAD, y + kg.row + 1, w - HEAD, 1); }
					}
				}
				const col = hue(i);
				const clampY = y => Math.max(top + 2, Math.min(top + lh - kg.row - 2, y));
				for (const a of E.adds) {   // draft new notes
					if (a.t !== i || a.beat + a.dur < b0 || a.beat > b1) continue;
					const x = X(a.beat), nw = Math.max(2, a.dur * E.zoom - 1), y = clampY(kg.y(a.key));
					g.globalAlpha = .85; g.fillStyle = col; g.fillRect(x, y, nw, kg.row);
					g.globalAlpha = 1; g.strokeStyle = v('--amber'); g.lineWidth = 1.5; g.strokeRect(x - .5, y - .5, nw + 1, kg.row + 1);
				}
				for (let n = 0; n < t.beatsNotes.length; n++) {
					const ed = E.edits.get(i + ':' + n), moved = !!ed && !ed.del, orig = t.beatsNotes[n];
					if (ed?.del) {   // deleted in the draft: a red dashed ghost
						if (orig[0] + orig[1] < b0 || orig[0] > b1) continue;
						g.globalAlpha = .8; g.strokeStyle = '#e5484d'; g.lineWidth = 1.2; g.setLineDash([3, 2]);
						g.strokeRect(X(orig[0]), clampY(kg.y(orig[2])), Math.max(2, orig[1] * E.zoom - 1), kg.row); g.setLineDash([]);
						continue;
					}
					const [s, len, k, vel] = moved ? eff(i, n) : orig;
					if (Math.max(s, orig[0]) + len < b0 || Math.min(s, orig[0]) > b1) continue;
					if (moved) {   // where it was: a faint outline
						g.globalAlpha = .5; g.strokeStyle = col; g.lineWidth = 1; g.setLineDash([3, 2]);
						g.strokeRect(X(orig[0]), clampY(kg.y(orig[2])), Math.max(2, len * E.zoom - 1), kg.row); g.setLineDash([]);
					}
					const x = X(s), nw = Math.max(2, len * E.zoom - 1), y = clampY(kg.y(k));
					const chosen = E.sel.notes.some(q => q.t === i && q.n === n);
					g.globalAlpha = t.mute ? .3 : .4 + .6 * Math.min(1, vel);
					g.fillStyle = col;
					g.fillRect(x, y, nw, kg.row);
					if (moved) { g.globalAlpha = 1; g.strokeStyle = v('--amber'); g.lineWidth = 1.5; g.strokeRect(x - .5, y - .5, nw + 1, kg.row + 1); }
					if (chosen) { g.globalAlpha = 1; g.strokeStyle = text; g.lineWidth = 1.5; g.strokeRect(x - 1, y - 1, nw + 2, kg.row + 2); }
					if (kg.open && nw > 26 && kg.row >= 8) { g.globalAlpha = .9; g.fillStyle = '#000'; g.font = '9px "Instrument Sans", sans-serif'; g.fillText(keyName(k), x + 2, y + kg.row - 1.5); }
				}
				g.globalAlpha = 1;
				g.fillStyle = line; g.fillRect(HEAD, top + lh - 1, w - HEAD, 1);
			}
			top += lh;
		});
		// grid over the lanes
		gridLines(g, b0, b1, TOPH, h, v);
		g.restore();

		// ---- selection band ----
		if (E.sel.b0 != null) {
			const x0 = Math.max(HEAD, X(E.sel.b0)), x1 = X(E.sel.b1);
			if (x1 > HEAD) {
				g.fillStyle = alpha(accent, 0.14); g.fillRect(x0, 0, x1 - x0, h);
				g.fillStyle = accent; g.fillRect(x0, 0, 1.5, h); g.fillRect(x1 - 1.5, 0, 1.5, h);
			}
		}

		// ---- top rows: ruler, sections, chords ----
		g.save(); g.beginPath(); g.rect(HEAD, 0, w - HEAD, TOPH); g.clip();
		g.fillStyle = v('--panel-2'); g.fillRect(HEAD, 0, w - HEAD, TOPH);
		const barPx = bpb * E.zoom, every = [1, 2, 4, 8, 16, 32].find(n => n * barPx >= 34) || 64;
		g.font = '11px "JetBrains Mono", monospace'; g.textBaseline = 'middle';
		for (let bar = Math.floor(b0 / bpb); bar * bpb <= b1; bar++) {
			const x = X(bar * bpb);
			g.fillStyle = line; g.fillRect(x, bar % every === 0 ? 4 : 14, 1, RULER - (bar % every === 0 ? 4 : 14));
			if (bar % every === 0) { g.fillStyle = muted; g.fillText(String(bar + 1), x + 4, RULER / 2 + 1); }
		}
		// sections
		d.markers.forEach((m, i) => {
			const x0 = X(m.beat), x1 = X(d.markers[i + 1]?.beat ?? d.endBeat);
			if (x1 < HEAD || x0 > w) return;
			g.fillStyle = i % 2 ? alpha(accent, 0.1) : alpha(accent, 0.18);
			g.fillRect(x0, RULER, x1 - x0, SECT);
			g.fillStyle = text; g.font = '600 11px "Instrument Sans", sans-serif';
			g.save(); g.beginPath(); g.rect(x0, RULER, x1 - x0, SECT); g.clip(); g.fillText(m.name, Math.max(x0, HEAD) + 5, RULER + SECT / 2 + 1); g.restore();
		});
		// chords from lint --harmony
		const hb = E.harmony?.bars || [];
		const bad = new Set(), rub = new Set();
		(E.harmony?.problems || []).forEach(p => { for (let b = p.bars[0]; b <= p.bars[1]; b++) bad.add(b); });
		(E.harmony?.rubs || []).forEach(r => r.bars.forEach(b => rub.add(b)));
		g.font = '11px "JetBrains Mono", monospace';
		for (const row of hb) {
			const x0 = X((row.bar - 1) * bpb), x1 = X(row.bar * bpb), y = RULER + SECT;
			if (x1 < HEAD || x0 > w) continue;
			if (bad.has(row.bar) || row.outside) { g.fillStyle = alpha('#e5484d', .3); g.fillRect(x0, y, x1 - x0, CHORD); }
			else if (rub.has(row.bar)) { g.fillStyle = alpha('#f2b33d', .32); g.fillRect(x0, y, x1 - x0, CHORD); }
			const label = row.halves ? row.halves.join(' ') : row.chord;
			if (g.measureText(label).width < x1 - x0 - 4) { g.fillStyle = text; g.fillText(label, x0 + 3, y + CHORD / 2 + 1); }
			g.fillStyle = line; g.fillRect(x0, y + 4, 1, CHORD - 8);
		}
		// comment pins
		E.comments.forEach(c => {
			if (!c.anchor?.beats) return;
			const x = X(c.anchor.beats[0]);
			if (x < HEAD - 6 || x > w + 6) return;
			g.fillStyle = c.status === 'done' ? muted : accent;
			g.beginPath(); g.moveTo(x, RULER - 2); g.lineTo(x - 5, 4); g.lineTo(x + 5, 4); g.closePath(); g.fill();
		});
		g.fillStyle = line; g.fillRect(HEAD, RULER, w - HEAD, 1); g.fillRect(HEAD, RULER + SECT, w - HEAD, 1); g.fillRect(HEAD, TOPH - 1, w - HEAD, 1);
		g.restore();

		// ---- playhead ----
		if (WLLive.active || (audio.src && audio.duration)) {
			const x = X(audioBeat());
			if (x >= HEAD) { g.fillStyle = v('--play'); g.fillRect(x - 1, 0, 2, h); }
		}
		if (E.loop && E.sel.b0 != null) {
			g.fillStyle = accent; g.font = '600 10px "Instrument Sans", sans-serif'; g.textBaseline = 'alphabetic';
			const x = Math.max(HEAD + 4, X(E.sel.b0) + 4); g.fillText('LOOP', x, TOPH - 5);
		}
		const lw = WLLive.window();
		if (lw) {   // the live loop's bars
			const x0 = Math.max(HEAD, X((lw.from - 1) * E.data.bpb)), x1 = X((lw.to - 1) * E.data.bpb);
			g.fillStyle = alpha(v('--warn'), 0.1); g.fillRect(x0, RULER, Math.max(0, x1 - x0), h - RULER);
			g.fillStyle = v('--warn'); g.font = '600 10px "Instrument Sans", sans-serif'; g.textBaseline = 'alphabetic';
			g.fillText('● LIVE', Math.max(HEAD + 4, x0 + 4), TOPH - 5);
		}

		// ---- frozen header column ----
		g.textBaseline = 'alphabetic';
		g.fillStyle = v('--panel'); g.fillRect(0, 0, HEAD, h);
		g.save(); g.beginPath(); g.rect(0, TOPH, HEAD, h - TOPH); g.clip();
		top = TOPH - sy;
		d.tracks.forEach((t, i) => {
			const lh = laneH(i);
			if (top + lh > TOPH && top < h) {
				const selT = E.sel.tracks.has(i), solo = state.soloIdx === i;
				if (selT) { g.fillStyle = alpha(accent, 0.12); g.fillRect(0, top, HEAD, lh); }
				g.fillStyle = hue(i); g.fillRect(0, top + 4, 3, lh - 8);
				g.fillStyle = muted; g.font = '11px "Instrument Sans", sans-serif'; g.fillText(E.expanded.has(i) ? '▾' : '▸', 9, top + 16);
				g.fillStyle = selT ? accent : text; g.font = '600 12.5px "Instrument Sans", sans-serif';
				clipText(g, (t.mute ? '(m) ' : '') + t.name, 22, top + 16, HEAD - 64);
				g.fillStyle = muted; g.font = '10.5px "Instrument Sans", sans-serif';
				clipText(g, soundName(t), 22, top + 31, HEAD - 104);
				if (t.lufs != null) {   // stem loudness, right-aligned left of the solo button
					g.font = '10.5px "JetBrains Mono", monospace'; g.textAlign = 'right';
					g.fillText(t.lufs <= -100 ? 'silent' : t.lufs.toFixed(1), HEAD - 38, top + 31); g.textAlign = 'left';
				}
				// solo button: filled = solo in the mix, outlined D = dry stem, amber dots = rendering
				const bx = HEAD - 32, by = top + 11, pend = state.pending?.i === i, dry = solo && state.soloKind === 'dry';
				g.fillStyle = pend ? alpha('#f2b33d', .25) : solo && !dry ? v('--button') : v('--panel-2');
				g.strokeStyle = pend ? '#f2b33d' : solo && !dry ? v('--button') : solo ? accent : line; g.lineWidth = 1;
				roundRect(g, bx, by, 24, 20, 5); g.fill(); g.stroke();
				g.fillStyle = pend ? '#f2b33d' : solo && !dry ? '#fff' : solo ? accent : text; g.font = '600 10.5px "Instrument Sans", sans-serif';
				g.textAlign = 'center';
				g.fillText(pend ? '•'.repeat(1 + Math.floor(performance.now() / 350) % 3) : dry ? 'D' : 'S', bx + 12, by + 14);
				g.textAlign = 'left';
				if (E.expanded.has(i)) {   // note names down the side of an open lane
					const kg = keyGeom(t, top, lh);
					g.font = '9.5px "JetBrains Mono", monospace'; g.fillStyle = muted;
					for (let k = kg.lo; k <= kg.hi; k++) {
						const y = kg.y(k) + kg.row;
						if (y > top + 40 && (k % 12 === 0 || (kg.row >= 9 && [4, 7].includes(k % 12)))) g.fillText(keyName(k), HEAD - 34, y);
					}
				}
				g.fillStyle = line; g.fillRect(0, top + lh - 1, HEAD, 1);
			}
			top += lh;
		});
		g.restore();
		// corner: key(s) of the song
		g.fillStyle = v('--panel-2'); g.fillRect(0, 0, HEAD, TOPH);
		g.fillStyle = muted; g.font = '10.5px "Instrument Sans", sans-serif';
		g.fillText('BAR', 12, 16); g.fillText('SECTION', 12, RULER + 14); g.fillText('CHORD', 12, RULER + SECT + 16);
		const keys = E.harmony?.keys || [];
		g.fillStyle = text; g.font = '600 11px "Instrument Sans", sans-serif';
		if (keys.length) clipText(g, keys.map(k => k.key).join(' → '), 70, RULER + SECT + 16, HEAD - 78);
		g.fillStyle = line; g.fillRect(HEAD - 1, 0, 1, h); g.fillRect(0, TOPH - 1, HEAD, 1);
		// drag rectangle
		if (E.drag?.kind === 'marquee' && E.drag.moved) {
			const r = E.drag; g.strokeStyle = accent; g.setLineDash([4, 3]); g.lineWidth = 1;
			g.strokeRect(Math.min(r.x0, r.x1), Math.min(r.y0, r.y1), Math.abs(r.x1 - r.x0), Math.abs(r.y1 - r.y0)); g.setLineDash([]);
		}
	}
	function gridLines(g, b0, b1, y0, y1, v) {
		const bpb = E.data.bpb, beats = E.zoom >= 10;
		for (let b = Math.floor(b0); b <= b1; b++) {
			const isBar = Math.abs(b / bpb - Math.round(b / bpb)) < 1e-6;
			if (!isBar && !beats) continue;
			g.fillStyle = isBar ? v('--line') : v('--grid');
			g.fillRect(X(b), y0, 1, y1 - y0);
		}
	}
	// the sound a track plays: preset or kit name, without folders, extensions or palette suffixes
	function soundName(t) {
		const raw = String(t.preset || t.plugin || '');
		return raw.split('/').pop().replace(/\.(vstpreset|clap-preset|state|fxb|fxp|vital|json)$/i, '').replace(/--.*$/, '') || t.plugin;
	}
	function clipText(g, s, x, y, max) {
		if (g.measureText(s).width <= max) { g.fillText(s, x, y); return; }
		while (s.length > 1 && g.measureText(s + '…').width > max) s = s.slice(0, -1);
		g.fillText(s + '…', x, y);
	}
	function roundRect(g, x, y, w, h, r) { g.beginPath(); g.moveTo(x + r, y); g.arcTo(x + w, y, x + w, y + h, r); g.arcTo(x + w, y + h, x, y + h, r); g.arcTo(x, y + h, x, y, r); g.arcTo(x, y, x + w, y, r); g.closePath(); }

	/* ---------- hit testing ---------- */
	function noteAt(p) {
		const lane = laneAt(p.y + scroller.scrollTop);
		if (!lane || p.x < HEAD || p.y < TOPH) return null;
		const t = E.data.tracks[lane.i], kg = keyGeom(t, lane.top - scroller.scrollTop, lane.h), b = beatAt(p.x);
		let best = null, bd = 1e9;
		t.beatsNotes.forEach((_, idx) => {
			const n = eff(lane.i, idx);
			if (b < n[0] - 2 / E.zoom || b > n[0] + n[1] + 2 / E.zoom) return;
			const y = kg.y(n[2]), dy = p.y < y ? y - p.y : p.y > y + kg.row ? p.y - y - kg.row : 0;
			if (dy < Math.max(4, kg.row) && dy < bd) { bd = dy; best = { t: lane.i, n: idx }; }
		});
		return best;
	}

	/* ---------- mouse ---------- */
	function down(e) {
		if (e.button !== 0) return;
		const p = pos(e), d = E.data; if (!d) return;
		if (p.x < HEAD && p.y >= TOPH) {   // header column
			const lane = laneAt(p.y + scroller.scrollTop); if (!lane) return;
			const i = lane.i, ly = p.y + scroller.scrollTop - lane.top;
			if (p.x >= HEAD - 34 && ly >= 8 && ly < 34) { toggleSolo(i, e.altKey ? 'dry' : 'mix'); syncAudioSelect(); dirty(); return; }
			if (p.x < 20) { E.expanded.has(i) ? E.expanded.delete(i) : E.expanded.add(i); sizeSpace(); dirty(); return; }
			if (E.expanded.has(i) && ly > 40 && p.x >= HEAD - 44) {   // the note names of a piano roll: a keyboard
				const kg = keyGeom(d.tracks[i], lane.top - scroller.scrollTop, lane.h);
				for (let k = kg.lo; k <= kg.hi; k++) { const y = kg.y(k); if (p.y >= y - 1 && p.y <= y + kg.row + 1) { hear(i, [{ key: k, vel: .85, start: 0, dur: .6 }]); E.keys.track = i; return; } }
			}
			if (e.shiftKey || e.metaKey) E.sel.tracks.has(i) ? E.sel.tracks.delete(i) : E.sel.tracks.add(i);
			else { E.sel.tracks = new Set([i]); }
			selChanged(); return;
		}
		if (p.x >= HEAD && p.y < TOPH) {   // ruler / sections / chords: drag = bars, click = seek
			E.drag = { kind: 'bars', b: beatAt(p.x), x0: p.x, moved: false };
			return;
		}
		if (p.x >= HEAD && p.y >= TOPH) {
			const hit = noteAt(p);
			if (hit) {
				const has = E.sel.notes.findIndex(q => q.t === hit.t && q.n === hit.n);
				if (e.shiftKey || e.metaKey) { has >= 0 ? E.sel.notes.splice(has, 1) : E.sel.notes.push(hit); }
				else E.sel.notes = [hit];
				barsFromNotes(); selChanged();
				if (E.hear && !(e.shiftKey || e.metaKey)) hearNotes([hit]);
				return;
			}
			E.drag = { kind: 'marquee', x0: p.x, y0: p.y, x1: p.x, y1: p.y, moved: false, add: e.shiftKey || e.metaKey };
		}
	}
	function move(e) {
		if (!dlg?.open || !E.data) return;
		const p = pos(e);
		if (E.drag) {
			if (Math.abs(p.x - (E.drag.x0)) > 3) E.drag.moved = true;
			if (E.drag.kind === 'bars' && E.drag.moved) {
				const a = E.drag.b, b = beatAt(p.x), bpb = E.data.bpb;
				E.sel.b0 = Math.floor(Math.min(a, b) / bpb) * bpb; E.sel.b1 = Math.ceil(Math.max(a, b) / bpb + 1e-9) * bpb;
				if (E.sel.b1 === E.sel.b0) E.sel.b1 += bpb;
				selChanged(false);
			}
			if (E.drag.kind === 'marquee') { E.drag.x1 = p.x; E.drag.y1 = p.y; if (Math.abs(p.y - E.drag.y0) > 3) E.drag.moved = true; dirty(); }
			autoscroll(p);
			return;
		}
		// hover tooltip
		const r = canvas.getBoundingClientRect();
		if (p.cx < r.left || p.cx > r.right || p.cy < r.top || p.cy > r.bottom) return;
		let html = '';
		if (p.x < HEAD && p.y >= TOPH) {
			const lane = laneAt(p.y + scroller.scrollTop);
			if (lane) {
				const ly = p.y + scroller.scrollTop - lane.top, t = E.data.tracks[lane.i];
				if (p.x >= HEAD - 34 && ly >= 8 && ly < 34) {
					const pend = state.pending?.i === lane.i;
					html = pend ? `Rendering ${esc(t.name)} through the mix (sends, buses, master)… ${Math.round((Date.now() - state.pending.since) / 1000)}s<br>it plays when ready; click again to cancel`
						: state.soloIdx === lane.i ? `Playing ${esc(t.name)} ${state.soloKind === 'dry' ? 'dry (its stem)' : 'in the mix'}; click to go back to the full mix`
						: `Solo ${esc(t.name)} in the mix, the whole song (renders once, then cached)<br>quicker: select bars and use Solo track or ▶ Preview<br>Alt+click: the dry stem, instantly`;
				}
			}
		} else if (p.x >= HEAD && p.y >= TOPH) {
			const hit = noteAt(p);
			if (hit) {
				const t = E.data.tracks[hit.t], n = t.beatsNotes[hit.n];
				html = `${esc(t.name)} · ${keyName(n[2])}<br>bar ${barBeat(n[0])} · ${+n[1].toFixed(3)} beats · vel ${Math.round(n[3] * 100)}`;
			} else html = `bar ${barBeat(beatAt(p.x))}`;
		} else if (p.x >= HEAD && p.y < TOPH) {
			const bar = barOf(beatAt(p.x)), row = E.harmony?.bars?.find(b => b.bar === bar);
			const probs = (E.harmony?.problems || []).filter(q => bar >= q.bars[0] && bar <= q.bars[1]);
			const rubs = (E.harmony?.rubs || []).filter(q => q.bars.includes(bar));
			const cs = E.comments.filter(c => c.anchor?.bars && bar >= c.anchor.bars[0] && bar <= c.anchor.bars[1]);
			html = `bar ${bar} · ${fmtTime(E.data.toSec((bar - 1) * E.data.bpb))}${row ? ' · ' + esc(row.chord) + (row.key ? ' in ' + esc(row.key) : '') : ''}`
				+ probs.map(q => '<br>⚠ ' + esc(q.detail.split(':')[0])).join('')
				+ rubs.map(q => '<br>rub: ' + esc(q.tracks.join(' / ')) + ' ' + esc(q.example)).join('')
				+ cs.map(c => '<br>💬 ' + esc(c.text.slice(0, 70))).join('');
		}
		if (html) {
			tip.innerHTML = html; tip.hidden = false;
			const mr = dlg.querySelector('.ed-main').getBoundingClientRect();
			tip.style.left = Math.min(p.cx - mr.left + 14, mr.width - tip.offsetWidth - 8) + 'px';
			tip.style.top = (p.cy - mr.top + 16) + 'px';
		} else tip.hidden = true;
	}
	function up(e) {
		if (!E.drag) return;
		const dr = E.drag; E.drag = null;
		if (dr.kind === 'bars' && !dr.moved) { seekBeat(dr.b, false); return; }
		if (dr.kind === 'marquee') {
			if (!dr.moved) {   // click on an empty spot: that bar, that track
				const lane = laneAt(dr.y0 + scroller.scrollTop), b = beatAt(dr.x0), bpb = E.data.bpb;
				E.sel = { b0: Math.floor(b / bpb) * bpb, b1: Math.floor(b / bpb) * bpb + bpb, tracks: new Set(lane ? [lane.i] : []), notes: [] };
				selChanged(); return;
			}
			const bx0 = beatAt(Math.min(dr.x0, dr.x1)), bx1 = beatAt(Math.max(dr.x0, dr.x1));
			const ya = Math.min(dr.y0, dr.y1), yb = Math.max(dr.y0, dr.y1);
			const picked = dr.add ? [...E.sel.notes] : [];
			E.data.tracks.forEach((t, i) => {
				const top = laneTop(i) - scroller.scrollTop, lh = laneH(i);
				if (top + lh < ya || top > yb) return;
				const kg = keyGeom(t, top, lh);
				t.beatsNotes.forEach((n, idx) => {
					const y = kg.y(n[2]);
					if (n[0] + n[1] > bx0 && n[0] < bx1 && y + kg.row >= ya && y <= yb && !picked.some(q => q.t === i && q.n === idx)) picked.push({ t: i, n: idx });
				});
			});
			E.sel.notes = picked; barsFromNotes(); selChanged();
		}
	}
	function autoscroll(p) {
		const w = scroller.clientWidth;
		if (p.x > w - 30) scroller.scrollLeft += 18; else if (p.x < HEAD + 10 && scroller.scrollLeft > 0) scroller.scrollLeft -= 18;
	}
	function barsFromNotes() {
		if (!E.sel.notes.length) return;
		const bpb = E.data.bpb, ns = E.sel.notes.map(q => E.data.tracks[q.t].beatsNotes[q.n]);
		E.sel.b0 = Math.floor(Math.min(...ns.map(n => n[0])) / bpb + 1e-9) * bpb;
		E.sel.b1 = Math.ceil(Math.max(...ns.map(n => n[0] + n[1])) / bpb - 1e-9) * bpb;
		if (E.sel.b1 <= E.sel.b0) E.sel.b1 = E.sel.b0 + bpb;
		E.sel.tracks = new Set(E.sel.notes.map(q => q.t));
	}
	function clearSel() { E.sel = { b0: null, b1: null, tracks: new Set(), notes: [] }; setLoop(false); selChanged(); }

	/* ---------- right-click menus ---------- */
	function contextMenu(e) {
		const p = pos(e), d = E.data;
		if (!d) return e.preventDefault();
		tip.hidden = true;
		const bpb = d.bpb, hasBars = E.sel.b0 != null, stop = state.pending ? { label: 'Cancel render', run: () => { cancelPreview(); syncAudioSelect(); dirty(); } } : null;
		const comment = () => { selChanged(); $d('#ed-text').focus(); };
		if (p.x < HEAD && p.y >= TOPH) {   // a track header
			const lane = laneAt(p.y + scroller.scrollTop); if (!lane) return e.preventDefault();
			const i = lane.i, t = d.tracks[i], solo = state.soloIdx === i, pend = state.pending?.i === i;
			return WLUI.menu(e, [
				pend ? { label: `Cancel rendering ${t.name}`, run: () => { cancelPreview(); dirty(); } }
					: { label: solo && state.soloKind === 'mix' ? 'Unsolo (back to the mix)' : `Solo ${t.name} in the mix`, disabled: !state.song.reportPath, run: () => { toggleSolo(i, 'mix'); syncAudioSelect(); dirty(); } },
				{ label: solo && state.soloKind === 'dry' ? 'Unsolo the dry stem' : 'Solo the dry stem', hint: 'Alt+click S', disabled: !stemFor(i), run: () => { toggleSolo(i, 'dry'); syncAudioSelect(); dirty(); } },
				hasBars && { label: `Preview ${t.name} in the selected bars`, run: () => { previewSelection([t.name]); syncAudioSelect(); } },
				'-',
				{ label: 'Select this track', run: () => { E.sel.tracks = new Set([i]); selChanged(); } },
				{ label: E.expanded.has(i) ? 'Collapse the lane' : 'Open as a piano roll', run: () => { E.expanded.has(i) ? E.expanded.delete(i) : E.expanded.add(i); sizeSpace(); dirty(); } },
				{ label: 'Comment on this track…', hint: 'C', run: () => { E.sel.tracks = new Set([i]); comment(); } },
				{ label: 'Copy track name', run: () => navigator.clipboard?.writeText(t.name) },
				'-',
				{ label: `Play ${t.name} from the keyboard`, hint: 'A-K', run: () => { E.keys.track = i; E.sel.tracks = new Set([i]); selChanged(); setKeys(true); } },
				{ label: 'Open as a piano roll and play it', run: () => { E.expanded.add(i); sizeSpace(); E.keys.track = i; hearInfo(`Click the note names on the left of ${t.name}'s piano roll to hear its instrument.`); dirty(); } },
			]);
		}
		if (p.x >= HEAD && p.y < TOPH) {   // ruler, sections, chords
			const b = beatAt(p.x), bar = barOf(b), bb = (bar - 1) * bpb;
			const inSel = hasBars && b >= E.sel.b0 && b < E.sel.b1;
			const pickBar = () => { if (!inSel) { E.sel.b0 = bb; E.sel.b1 = bb + bpb; E.sel.notes = []; selChanged(); } };
			return WLUI.menu(e, [
				{ label: 'Play from here', hint: 'bar ' + barOf(b), disabled: !audio.src, run: () => seekBeat(b, true) },
				{ label: 'Play from the start of bar ' + bar, disabled: !audio.src, run: () => seekBeat(bb, true) },
				'-',
				{ label: inSel ? 'Preview the selected bars' : `Preview bar ${bar}`, hint: 'P', run: () => { pickBar(); previewSelection(); syncAudioSelect(); } },
				{ label: inSel ? 'Loop the selected bars' : `Loop bar ${bar}`, hint: 'L', run: () => { pickBar(); setLoop(true); seekBeat(E.sel.b0, true); } },
				{ label: inSel ? 'Comment on the selected bars…' : `Comment on bar ${bar}…`, run: () => { pickBar(); comment(); } },
				stop,
			]);
		}
		if (p.x >= HEAD && p.y >= TOPH) {   // lanes: act on the note under the pointer (or the selection)
			const hit = noteAt(p), lane = laneAt(p.y + scroller.scrollTop);
			if (hit && !E.sel.notes.some(q => q.t === hit.t && q.n === hit.n)) { E.sel.notes = [hit]; barsFromNotes(); selChanged(); }
			if (!hit && !hasBars) {   // an empty spot: that bar, that track
				const b = beatAt(p.x);
				E.sel = { b0: Math.floor(b / bpb) * bpb, b1: Math.floor(b / bpb) * bpb + bpb, tracks: new Set(lane ? [lane.i] : []), notes: [] };
				selChanged();
			}
			const one = E.sel.tracks.size === 1 ? d.tracks[[...E.sel.tracks][0]] : null;
			return WLUI.menu(e, [
				{ label: 'Play from here', hint: 'bar ' + barOf(beatAt(p.x)), disabled: !audio.src, run: () => seekBeat(beatAt(p.x), true) },
				E.sel.notes.length && { label: E.sel.notes.length === 1 ? 'Hear this note' : 'Hear these notes', hint: 'its instrument', run: () => hearNotes(E.sel.notes) },
				E.sel.notes.length && { label: 'Move up a semitone', hint: '↑', run: () => key({ key: 'ArrowUp', code: 'ArrowUp', target: {}, preventDefault() {} }) },
				E.sel.notes.length && { label: 'Move down a semitone', hint: '↓', run: () => key({ key: 'ArrowDown', code: 'ArrowDown', target: {}, preventDefault() {} }) },
				E.edits.size && { label: 'Discard the note edits', run: () => { E.edits.clear(); editsChanged(); } },
				'-',
				{ label: one ? `Preview ${one.name} here` : 'Preview the selection', hint: 'P', run: () => { previewSelection(); syncAudioSelect(); } },
				one && E.sel.tracks.size && { label: 'Preview these bars, all tracks', run: () => { previewSelection([]); syncAudioSelect(); } },
				{ label: 'Loop these bars', hint: 'L', run: () => { setLoop(true); seekBeat(E.sel.b0, true); } },
				{ label: 'Comment on this…', hint: 'C', run: comment },
				{ label: 'Copy reference', run: () => navigator.clipboard?.writeText(reference().text) },
				{ label: 'Clear the selection', hint: 'Esc', run: clearSel },
				stop,
			]);
		}
		e.preventDefault();
	}

	/* ---------- selection summary / reference ---------- */
	function reference() {
		const d = E.data, s = E.sel, out = { text: '' };
		if (!d || (s.b0 == null && !s.tracks.size && !s.notes.length)) return out;
		const lines = [];
		if (s.b0 != null) {
			const a = barOf(s.b0), b = barOf(s.b1 - 1e-6);
			out.bars = [a, b]; out.beats = [s.b0, s.b1];
			out.time = [+(d.toSec(s.b0)).toFixed(2), +(d.toSec(s.b1)).toFixed(2)];
			const sec = d.markers.filter(m => m.beat < s.b1 && (d.markers[d.markers.indexOf(m) + 1]?.beat ?? 1e9) > s.b0).map(m => m.name);
			lines.push((a === b ? 'bar ' + a : 'bars ' + a + '-' + b) + ' · ' + fmtTime(out.time[0]) + '-' + fmtTime(out.time[1]) + (sec.length ? ' · ' + sec.join(', ') : ''));
			const chords = (E.harmony?.bars || []).filter(r => r.bar >= a && r.bar <= b).map(r => r.halves ? r.halves.join('>') : r.chord);
			if (chords.length && chords.length <= 16) lines.push('chords: ' + chords.join(' '));
		}
		if (s.tracks.size) { out.tracks = [...s.tracks].map(i => d.tracks[i].name); lines.push('tracks: ' + out.tracks.join(', ')); }
		if (s.notes.length) {
			out.notes = s.notes.slice(0, 64).map(q => { const t = d.tracks[q.t], n = t.beatsNotes[q.n]; return { track: t.name, key: keyName(n[2]), midi: n[2], bar: barBeat(n[0]), beat: n[0], dur: n[1], vel: n[3] }; });
			const shown = out.notes.slice(0, 8).map(n => `${n.track} ${n.key} @${n.bar}`);
			lines.push('notes: ' + shown.join(', ') + (s.notes.length > 8 ? ` (+${s.notes.length - 8} more)` : ''));
		}
		out.ref = lines[0] + (out.tracks ? ' · ' + out.tracks.join(', ') : '');
		out.text = state.slug + ' · ' + lines.join('\n');
		return out;
	}
	function selChanged(redrawSide = true) {
		const r = reference(), el = $d('#ed-ref'), any = !!r.text;
		el.classList.toggle('none', !any);
		el.textContent = any ? r.text.replace(state.slug + ' · ', '') : 'Drag across the ruler to pick bars, click a track name, click or drag over notes.';
		$d('#ed-sloop').disabled = E.sel.b0 == null;
		$d('#ed-live').disabled = E.sel.b0 == null && !WLLive.active;
		$d('#ed-preview').disabled = E.sel.b0 == null;
		$d('#ed-preview').textContent = E.sel.b0 == null ? '▶ Preview these bars' : E.sel.tracks.size ? `▶ Preview ${E.sel.tracks.size === 1 ? E.data.tracks[[...E.sel.tracks][0]].name : E.sel.tracks.size + ' tracks'} here` : '▶ Preview these bars';
		$d('#ed-solo').disabled = !(E.sel.tracks.size || E.sel.notes.length);
		$d('#ed-copy').disabled = !any;
		if (!any && E.loop) setLoop(false);
		dirty();
	}
	// render the selected bars (and the selected tracks, or `names`) through the mix, then loop them
	function previewSelection(names) {
		if (!E.data || E.sel.b0 == null) return;
		const a = barOf(E.sel.b0), b = barOf(E.sel.b1 - 1e-6);
		const tracks = Array.isArray(names) ? names : [...E.sel.tracks].map(i => E.data.tracks[i].name);
		const one = tracks.length === 1 ? E.data.tracks.findIndex(t => t.name === tracks[0]) : null;
		requestPreview({ tracks, from: a, to: b + 1, i: one ?? undefined,
			label: (tracks.length ? tracks.join(', ') + ' in ' : '') + (a === b ? 'bar ' + a : 'bars ' + a + '-' + b) });
		dirty();
	}
	function setLoop(on) {
		E.loop = on && E.sel.b0 != null;
		$d('#ed-loop').classList.toggle('on', E.loop);
		dirty();
	}

	/* ---------- comments ---------- */
	async function save() {
		const text = $d('#ed-text').value.trim();
		if (!text) { $d('#ed-text').focus(); return; }
		const r = reference();
		const body = { op: 'add', text, ref: r.ref || 'whole song', report: state.song.reportPath || '' };   // the render being heard
		for (const k of ['bars', 'beats', 'time', 'tracks', 'notes']) if (r[k]) body[k] = r[k];
		const res = await post(body);
		if (res?.comments) { E.comments = res.comments; $d('#ed-text').value = ''; renderComments(); badge(); dirty(); }
	}
	async function op(body) {
		const res = await post(body);
		if (res?.comments) { E.comments = res.comments; renderComments(); badge(); dirty(); }
	}
	// a change to review.json; a failure shows under the save button instead of vanishing (the text stays in the box)
	async function post(body) {
		const hint = $d('.ed-hint');
		try {
			const res = await postJson('api/review?song=' + encodeURIComponent(state.slug), body);
			hint.textContent = HINT; hint.classList.remove('err');
			return res;
		} catch (e) {
			hint.textContent = `Not saved: ${e.message}. Reload the page and save again.`; hint.classList.add('err');
			return null;
		}
	}
	function renderComments() {
		if (!dlg) return;
		const list = [...E.comments].sort((a, b) => (a.status === 'done') - (b.status === 'done') || (a.anchor?.beats?.[0] ?? 0) - (b.anchor?.beats?.[0] ?? 0));
		const open = E.comments.filter(c => c.status !== 'done').length;
		$d('#ed-count').textContent = E.comments.length ? `${open} open · ${E.comments.length - open} done` : '';
		$d('#ed-list').innerHTML = list.map(c => `
			<div class="ed-c ${c.status === 'done' ? 'done' : ''}" data-id="${esc(c.id)}">
				<div class="ref">${esc(c.anchor?.ref || 'whole song')}${c.anchor?.revision ? ` · r${esc(String(c.anchor.revision))}` : ''}${c.status !== 'done' && c.now?.outdated ? ` <span class="ed-changed" title="${esc((c.now.why || []).join('\n'))}">changed since</span>` : ''}</div>
				<p>${esc(c.text)}</p>${(c.replies || []).map(rp => `<p class="ed-reply">${esc(rp.author?.name || 'Agent')}${rp.revision ? ` (r${esc(String(rp.revision))})` : ''}: ${esc(rp.text)}</p>`).join('')}
				<div class="meta"><span>${new Date(c.created).toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })}</span><span class="sp"></span>
					<button data-act="status">${c.status === 'done' ? 'Reopen' : 'Mark done'}</button><button data-act="delete">Delete</button></div>
			</div>`).join('') || '<div class="ed-empty">No comments yet. Select something, write what should change, save.</div>';
	}
	function badge() {
		const b = document.getElementById('ed-badge'); if (!b) return;
		const open = E.comments.filter(c => c.status !== 'done').length;
		b.hidden = !open; b.textContent = open;
	}
	function legend() {
		if (!dlg) return;
		const h = E.harmony;
		const probs = h ? h.problems.length : 0, rubs = h ? h.rubs.reduce((n, r) => n + r.count, 0) : 0;
		$d('#ed-legend').innerHTML = h ? `<span><i style="background:color-mix(in srgb,#e5484d 45%,transparent)"></i>${probs} harmony problem${probs === 1 ? '' : 's'}</span><span><i style="background:color-mix(in srgb,#f2b33d 45%,transparent)"></i>${rubs} rub${rubs === 1 ? '' : 's'}</span>` : '';
	}

	/* ---------- live notes: the track's own instrument, loaded in a server worker ---------- */
	let actx = null, inflight = false, queued = null;
	const loadedTracks = new Set();
	function hearInfo(msg, err) { const el = $d('#ed-hearinfo'); el.textContent = msg; el.classList.toggle('err', !!err); }
	// notes: [{key, vel, start, dur}] in seconds from the first; the newest request wins while one renders
	async function hear(ti, notes) {
		const t = E.data?.tracks[ti]; if (!t || !notes.length) return;
		if (inflight) { queued = [ti, notes]; return; }
		inflight = true;
		const id = state.slug + '|' + t.name;
		if (!loadedTracks.has(id)) hearInfo(`Loading ${soundName(t)} for ${t.name}… (the first note takes a moment)`);
		try {
			const send = () => fetch('api/play', { method: 'POST', headers: postHeaders(), body: JSON.stringify({ song: state.slug, track: t.name, notes }) });
			let r = await send();
			if (r.status === 403) { await postJson('api/preview/cancel', { song: '' }).catch(() => {}); r = await send(); }   // postJson refreshes a stale token
			if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || 'the server answered ' + r.status); }
			const info = JSON.parse(r.headers.get('X-Wavelength-Play') || '{}');
			actx ??= new (window.AudioContext || window.webkitAudioContext)();
			if (actx.state === 'suspended') await actx.resume();
			const buf = await actx.decodeAudioData(await r.arrayBuffer());
			const src = actx.createBufferSource(); src.buffer = buf; src.connect(actx.destination); src.start();
			loadedTracks.add(id);
			hearInfo(`${t.name} · ${info.plugin}${info.preset ? ' · ' + info.preset : ''} · ${notes.length === 1 ? keyName(notes[0].key) : notes.length + ' notes'} · ${info.loaded ? 'loaded in ' : ''}${info.ms} ms`);
		} catch (err) { hearInfo(`Could not play ${t.name}: ${err.message}`, true); }
		finally {
			inflight = false;
			if (queued) { const q = queued; queued = null; hear(...q); }
		}
	}
	// selected notes (with their draft moves) of one track, as a phrase
	function hearNotes(list) {
		const ti = list[0]?.t; if (ti == null) return;
		const d = E.data, ns = list.filter(q => q.t === ti).slice(0, 64).map(q => eff(q.t, q.n));
		const t0 = Math.min(...ns.map(n => n[0]));
		if (d.toSec(Math.max(...ns.map(n => n[0]))) - d.toSec(t0) > 8) return hearInfo('Pick fewer notes to hear (8 seconds at most).', true);
		hear(ti, ns.map(n => ({ key: n[2], vel: n[3], start: d.toSec(n[0]) - d.toSec(t0), dur: Math.min(4, d.toSec(n[0] + n[1]) - d.toSec(n[0])) })));
	}
	function hearEdits() {
		hearNotes([...E.edits.keys()].map(k => { const [t, n] = k.split(':').map(Number); return { t, n }; }));
	}
	function playKey(k) {
		const ti = E.keys.track ?? [...E.sel.tracks][0] ?? E.sel.notes[0]?.t;
		if (ti == null) return hearInfo('Select a track (click its name) to play its instrument.', true);
		E.keys.track = ti;
		hear(ti, [{ key: k, vel: .85, start: 0, dur: .7 }]);
	}
	function keysInfo() {
		const ti = E.keys.track ?? [...E.sel.tracks][0];
		hearInfo(E.keys.on ? `Keys play ${ti != null ? E.data.tracks[ti].name : 'the selected track'} · octave ${E.keys.octave} (Z / X) · A-K = C-C, W E T Y U = sharps` : 'Keys off.');
	}
	function setKeys(on) {
		E.keys.on = on;
		if (on) E.keys.track = [...E.sel.tracks][0] ?? E.sel.notes[0]?.t ?? E.keys.track;
		$d('#ed-keys').classList.toggle('on', on);
		keysInfo();
	}
	function describeEdit(t, n, ed) {
		const tr = E.data.tracks[t], x = tr.beatsNotes[n], moved = eff(t, n), parts = [];
		if (ed.del) return { text: `${tr.name} ${keyName(x[2])} at bar ${barBeat(x[0])}: delete`, track: tr.name, t, n, del: true,
			note: { track: tr.name, key: keyName(x[2]), midi: x[2], bar: barBeat(x[0]), beat: x[0], dur: x[1], vel: x[3] } };
		if (ed.dk) parts.push(`${keyName(x[2])} → ${keyName(moved[2])}`);
		if (ed.db) parts.push(`${ed.db > 0 ? 'later' : 'earlier'} by ${Math.abs(ed.db)} beat${Math.abs(ed.db) === 1 ? '' : 's'} (to ${barBeat(moved[0])})`);
		return { text: `${tr.name} ${keyName(x[2])} at bar ${barBeat(x[0])}: ${parts.join(', ')}`, track: tr.name,
			note: { track: tr.name, key: keyName(x[2]), midi: x[2], bar: barBeat(x[0]), beat: x[0], dur: x[1], vel: x[3] } };
	}
	const editList = () => [...E.edits].map(([k, ed]) => { const [t, n] = k.split(':').map(Number); return { ...describeEdit(t, n, ed), ed, t, n }; })
		.concat(E.adds.map(a => { const tr = E.data.tracks[a.t]; return { text: `${tr.name}: add ${keyName(a.key)} at bar ${barBeat(a.beat)}`, track: tr.name, add: a,
			note: { track: tr.name, key: keyName(a.key), midi: a.key, bar: barBeat(a.beat), beat: a.beat, dur: a.dur, vel: a.vel } }; }));
	let liveSave = 0;
	function editsChanged() {
		const list = editList();
		if (WLLive.active && list.length) { clearTimeout(liveSave); liveSave = setTimeout(() => saveToSong(true), 300); }   // live: every edit is heard
		$d('#ed-edits').hidden = !list.length;
		$d('#ed-edhear').hidden = !E.edits.size;
		$d('#ed-editlist').textContent = list.slice(0, 12).map(x => x.text).join('\n') + (list.length > 12 ? `\n(+${list.length - 12} more)` : '');
		dirty();
	}
	// the draft goes to the agent as a comment: the song's source makes the job, so edits belong there
	async function saveEdits() {
		const list = editList();
		if (!list.length) return;
		const beats = list.map(x => x.note.beat), bpb = E.data.bpb;
		const b0 = Math.floor(Math.min(...beats) / bpb) * bpb, b1 = Math.floor(Math.max(...beats) / bpb) * bpb + bpb;
		const tracks = [...new Set(list.map(x => x.track))];
		const text = 'Note edits to make in the song (heard in the editor):\n' + list.map(x => '- ' + x.text).join('\n');
		const res = await post({ op: 'add', text, report: state.song.reportPath || '', ref: `${barOf(b0) === barOf(b1 - 1e-6) ? 'bar ' + barOf(b0) : 'bars ' + barOf(b0) + '-' + barOf(b1 - 1e-6)} · note edits · ${tracks.join(', ')}`,
			bars: [barOf(b0), barOf(b1 - 1e-6)], beats: [b0, b1], time: [+E.data.toSec(b0).toFixed(2), +E.data.toSec(b1).toFixed(2)], tracks,
			notes: list.slice(0, 64).map(x => x.note) });
		if (res?.comments) { E.comments = res.comments; E.edits.clear(); E.adds = []; editsChanged(); renderComments(); badge(); }
	}
	// the draft into the song: edits.json (note found by its beat and sounding key, as the timeline shows it)
	async function saveToSong(live) {
		const list = editList(), edits = [], skipped = [];
		for (const x of list) {
			const tr = E.data.tracks[x.add ? x.add.t : x.t];
			if (x.add) { edits.push({ track: tr.name, add: { beat: x.add.beat, dur: x.add.dur, key: x.add.key, vel: x.add.vel } }); continue; }
			if (tr.notes[x.n]?.[4]) { skipped.push(x.text); continue; }   // placed in seconds, not beats: send those to the agent
			const o = tr.beatsNotes[x.n], at = { beat: o[0], key: o[2] };
			if (x.ed.del) edits.push({ track: tr.name, at, delete: true });
			else {
				const to = {};
				if (x.ed.dk) to.key = o[2] + x.ed.dk;
				if (x.ed.db) to.beat = Math.max(0, Math.round((o[0] + x.ed.db) * 1000) / 1000);
				edits.push({ track: tr.name, at, to });
			}
		}
		if (!edits.length) return hearInfo(skipped.length ? 'These notes are placed in seconds; send them to the agent instead.' : 'Nothing to save.', true);
		try {
			await postJson('api/edits', { song: state.slug, op: 'add', edits });
			E.edits.clear(); E.adds = []; E.sel.notes = []; editsChanged(); selChanged();
			hearInfo(`Saved ${edits.length} edit${edits.length === 1 ? '' : 's'} to edits.json${skipped.length ? ` (${skipped.length} placed in seconds left out)` : ''}. ${live === true ? 'The live loop plays ' + (edits.length === 1 ? 'it' : 'them') + ' next.' : 'Render to hear ' + (edits.length === 1 ? 'it' : 'them') + ' in the mix.'}`);
			$d('#ed-render').classList.add('amber');
			await loadSong();
		} catch (err) { hearInfo('Not saved: ' + err.message, true); }
	}
	async function editsOp(op) {
		if (op === 'clear' && !(await WLUI.confirm({ title: 'Remove every note edit?', danger: true, ok: 'Remove them', body: 'edits.json goes away and the song plays its notes as make-job.py wrote them (after the next render).' }))) return;
		try { await postJson('api/edits', { song: state.slug, op }); $d('#ed-render').classList.add('amber'); await loadSong(); }
		catch (err) { hearInfo(err.message, true); }
	}
	function renderSaved() {
		const d = state.song, n = d?.edits?.length || 0, miss = d?.unmatchedEdits || [], el = $d('#ed-saved');
		el.hidden = !n && !miss.length;
		el.innerHTML = (n ? `<b>${n}</b> saved note edit${n === 1 ? '' : 's'} in <code>edits.json</code> · <button data-eop="undo">Undo last save</button> · <button data-eop="clear">Remove all</button>` : '')
			+ miss.map(m => `<div class="miss">⚠ ${esc(m.track)}: ${esc(m.why)}</div>`).join('');
	}
	async function renderSong() {
		try { await postJson('api/render', { song: state.slug }); $d('#ed-render').classList.remove('amber'); }
		catch (err) { hearInfo('Could not start the render: ' + err.message, true); }
	}

	/* ---------- zoom, keys, audio ---------- */
	function zoomAt(f, at) {
		const x = at ?? scroller.clientWidth / 2, b = beatAt(x);
		E.zoom = Math.max(1.5, Math.min(160, E.zoom * f));
		sizeSpace();
		scroller.scrollLeft = HEAD + b * E.zoom - x;
		dirty();
	}
	function fit() { if (!E.data) return; E.zoom = Math.max(1.5, (scroller.clientWidth - HEAD - 30) / E.data.endBeat); sizeSpace(); scroller.scrollLeft = 0; dirty(); }
	const KEYMAP = { KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11, KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16 };
	function key(e) {
		if (e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT') return;
		if (E.keys.on && !e.metaKey && !e.ctrlKey && !e.altKey) {   // the computer keyboard plays the instrument
			if (e.code in KEYMAP) { e.preventDefault(); if (!e.repeat) playKey(12 * (E.keys.octave + 1) + KEYMAP[e.code]); return; }
			if (e.code === 'KeyZ' || e.code === 'KeyX') { e.preventDefault(); E.keys.octave = Math.max(0, Math.min(8, E.keys.octave + (e.code === 'KeyX' ? 1 : -1))); keysInfo(); return; }
		}
		if ((e.key === 'Delete' || e.key === 'Backspace') && E.sel.notes.length) {   // delete the selected notes (a draft)
			e.preventDefault();
			for (const q of E.sel.notes) E.edits.set(q.t + ':' + q.n, { dk: 0, db: 0, del: true });
			E.sel.notes = []; selChanged(); editsChanged();
			return;
		}
		if (e.key.startsWith('Arrow') && E.sel.notes.length) {   // move the selected notes (a draft) and hear them
			e.preventDefault();
			const dk = e.key === 'ArrowUp' ? (e.shiftKey ? 12 : 1) : e.key === 'ArrowDown' ? (e.shiftKey ? -12 : -1) : 0;
			const db = e.key === 'ArrowRight' ? (e.shiftKey ? 1 : .25) : e.key === 'ArrowLeft' ? (e.shiftKey ? -1 : -.25) : 0;
			for (const q of E.sel.notes) {
				const id = q.t + ':' + q.n, ed = E.edits.get(id) || { dk: 0, db: 0 };
				if (ed.del) continue;
				ed.dk += dk; ed.db = Math.round((ed.db + db) * 1000) / 1000;
				if (!ed.dk && !ed.db) E.edits.delete(id); else E.edits.set(id, ed);
			}
			editsChanged();
			if (dk) hearNotes(E.sel.notes);
			return;
		}
		if (e.code === 'Space') { e.preventDefault(); $d('#ed-play').click(); }
		else if (e.key === 'l' || e.key === 'L') setLoop(!E.loop);
		else if (e.key === 'v' || e.key === 'V') toggleLive();
		else if (e.key === 'p' || e.key === 'P') previewSelection();
		else if (e.key === '+' || e.key === '=') zoomAt(1.4);
		else if (e.key === '-') zoomAt(1 / 1.4);
		else if (e.key === 'f' || e.key === 'F') fit();
		else if (e.key === 'c' || e.key === 'C') { e.preventDefault(); $d('#ed-text').focus(); }
	}
	function syncAudioSelect() {
		const sel = $d('#ed-audio'), d = state.song;
		const opts = (d.audio || []).map(p => `<option value="${esc(p)}">${esc(p.split('/').pop())}</option>`);
		if (d.reportPath) {
			opts.push('<optgroup label="Solo in the mix (sends, buses, master)">');
			(E.data?.tracks || []).forEach((t, i) => opts.push(`<option value="solo:${i}">${esc(t.name)}</option>`));
			opts.push('</optgroup>');
		}
		const dry = (E.data?.tracks || []).map((t, i) => stemFor(i) ? `<option value="stem:${i}">${esc(t.name)}</option>` : '').join('');
		if (dry) opts.push(`<optgroup label="Dry stems (before fader, sends, master)">${dry}</optgroup>`);
		sel.innerHTML = opts.join('') || '<option>no audio yet</option>';
		if (state.preview) opts.unshift(`<option value="preview">Preview: ${esc(state.preview.label)}</option>`);
		sel.innerHTML = opts.join('') || '<option>no audio yet</option>';
		sel.value = state.preview ? 'preview' : state.soloIdx >= 0 ? (state.soloKind === 'dry' ? 'stem:' : 'solo:') + state.soloIdx : (state.src || '');
	}

	/* ---------- frame loop: playhead, loop, follow ---------- */
	(function frame() {
		if (dlg?.open && E.data) {
			if (!audio.paused || WLLive.playing()) {
				const b = audioBeat();
				if (E.loop && !WLLive.active && !state.srcWindow && E.sel.b0 != null && (b >= E.sel.b1 || b < E.sel.b0 - E.data.bpb)) seekBeat(E.sel.b0, false);
				if (E.follow && performance.now() - E.userScrollAt > 2500 && !E.drag) {
					const x = X(b), w = scroller.clientWidth;
					if (x > w - 60 || x < HEAD) scroller.scrollLeft = HEAD + b * E.zoom - HEAD - (w - HEAD) * 0.15;
				}
				const s = songSecNow(), ab = audioBeat(), bar = barOf(ab), beat = Math.floor(ab - (bar - 1) * E.data.bpb + 1e-9) + 1;
				$d('#ed-clock').innerHTML = `${fmtTime(Math.max(0, s))} / ${fmtTime(E.data.toSec(E.data.endBeat))}<small>bar ${bar} · beat ${beat}${s < 0 ? ' (lead-in)' : WLLive.active ? ' · live' : state.srcWindow ? ' · preview' : ''}</small>`;
				E.dirty = true;
			}
			if (state.pending) E.dirty = true;   // the rendering dots on the solo button
			const st = $d('#ed-status'), msg = document.getElementById('solo').textContent;
			$d('#ed-cancel').hidden = !state.pending && !state.rendering;
			const rb = $d('#ed-render'), rl = state.rendering ? (state.rendering.status === 'queued' ? 'Waiting to render…' : `Rendering… ${state.rendering.seconds ?? 0}s`) : 'Render';
			if (rb.textContent !== rl) { rb.textContent = rl; rb.disabled = !!state.rendering; }
			if (st.textContent !== msg || st.classList.contains('busy') !== !!state.pending) { st.textContent = msg; st.title = msg + (state.srcWindow ? '\nclick the timeline outside the preview, or pick the mix, to go back' : ''); st.classList.toggle('busy', !!state.pending); }
			if (E.dirty) { E.dirty = false; draw(); }
		}
		requestAnimationFrame(frame);
	})();

	/* ---------- public ---------- */
	window.WLEditor = {
		open(atBeat) {
			if (!state.song?.job) return;
			if (!dlg) build();
			$d('#ed-title').textContent = state.slug;
			prepare(); sizeSpace(); syncAudioSelect(); renderComments(); selChanged(); icon(); renderSaved(); editsChanged();
			dlg.showModal();
			requestAnimationFrame(() => {
				if (E.fitted !== state.slug || E.zoom * E.data.endBeat < scroller.clientWidth - HEAD) { fit(); E.fitted = state.slug; }   // a new song opens showing all of it
				if (atBeat != null) scroller.scrollLeft = Math.max(0, atBeat * E.zoom - 200);
				dirty();
			});
			loadExtras();
		},
		refresh() {   // the song was re-rendered or its job changed
			if (dlg?.open) { prepare(); sizeSpace(); syncAudioSelect(); renderSaved(); dirty(); loadExtras(); WLLive.refresh(); }
			else fetch('api/review?song=' + encodeURIComponent(state.slug)).then(r => r.json()).then(r => { E.comments = r.comments || []; badge(); }).catch(() => {});
		},
		redraw() { dirty(); if (dlg?.open) syncAudioSelect(); },
		// select beats b0..b1 (whole bars), as dragging across the ruler does
		select(b0, b1) { if (!E.data) return; const bpb = E.data.bpb; E.sel = { b0: Math.floor(b0 / bpb) * bpb, b1: Math.ceil(b1 / bpb) * bpb, tracks: new Set(), notes: [] }; selChanged(); },
		reset() { E.sel = { b0: null, b1: null, tracks: new Set(), notes: [] }; E.expanded = new Set(); E.edits = new Map(); E.adds = []; E.keys.track = null; E.harmony = null; E.comments = []; E.loop = false; badge(); },
	};
})();
