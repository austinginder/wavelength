/* Live loop for the arrangement editor.
 *
 * The selected bars play as a seamless loop (Web Audio) while you change the mix and the notes. Each
 * change asks the server for the loop as it should sound now (render --loop --cache --mix: only tracks
 * whose sound changed render again) and the new loop crossfades in where the playhead is. The mixer is
 * a layer for listening: it never changes the song; note edits go to edits.json like any saved edit.
 *
 * Uses the page's globals: state, postJson, esc, hue.
 */
window.WLLive = (() => {
	const L = {
		active: false, song: null, from: 0, to: 0, win: null,
		ctx: null, out: null, cur: null, buf: null, playing: false, pausedAt: 0,
		mix: {}, solo: new Set(),            // name -> {gain (dB added to the song's fader), pan, mute}; soloed track names
		asked: 0, heard: 0, changedAt: 0,    // newest request, newest loop playing, when the change being waited for was made
		info: null, took: null, error: '', rendering: false, polling: false, el: null, onchange: null,
	};
	const len = () => (L.buf ? L.buf.duration : 0);
	const changed = () => { render(); L.onchange?.(); };

	/* ---------- audio ---------- */
	function ctx() {
		if (!L.ctx) {
			L.ctx = new (window.AudioContext || window.webkitAudioContext)();
			L.out = L.ctx.createGain();
			L.out.connect(L.ctx.destination);
		}
		return L.ctx;
	}
	function pos() {   // seconds into the loop
		const n = len();
		if (!n) return 0;
		if (!L.cur || !L.playing) return L.pausedAt % n;
		return (((L.ctx.currentTime - L.cur.t0) % n) + n) % n;
	}
	// one looping buffer through its own gain, so the next one can crossfade over it
	function voice(buffer, at, offset, fadeIn) {
		const g = L.ctx.createGain(), s = L.ctx.createBufferSource();
		s.buffer = buffer; s.loop = true;
		s.connect(g); g.connect(L.out);
		g.gain.setValueAtTime(fadeIn ? 0 : 1, at);
		if (fadeIn) g.gain.linearRampToValueAtTime(1, at + 0.03);
		s.start(at, offset);
		return { src: s, gain: g, t0: at - offset };
	}
	function fadeOut(v, at) {
		if (!v) return;
		at ??= L.ctx.currentTime;
		v.gain.gain.cancelScheduledValues(at);
		v.gain.gain.setValueAtTime(v.gain.gain.value, at);
		v.gain.gain.linearRampToValueAtTime(0, at + 0.03);
		try { v.src.stop(at + 0.05); } catch {}
	}
	function play() {
		L.playing = true;
		if (L.buf) {
			ctx().resume();
			L.cur = voice(L.buf, L.ctx.currentTime + 0.03, pos(), true);
		}
		changed();
	}
	function pause() {
		if (!L.playing) return;
		L.pausedAt = pos();
		fadeOut(L.cur);
		L.cur = null;
		L.playing = false;
		changed();
	}
	// the new loop takes over where the playhead is (from the top when its length changed)
	function swapIn(buffer) {
		const same = L.buf && Math.abs(L.buf.duration - buffer.duration) < 1e-3;
		const p = same ? pos() : 0;
		L.buf = buffer;
		if (!L.playing) { L.pausedAt = p; return; }
		ctx().resume();
		const at = L.ctx.currentTime + 0.02, old = L.cur;
		const off = same && old ? (((at - old.t0) % buffer.duration) + buffer.duration) % buffer.duration : 0;
		L.cur = voice(buffer, at, off, !!old);
		fadeOut(old, at);
	}

	/* ---------- asking the server ---------- */
	// faders, pans and mutes that differ from the job; a solo mutes every other track
	function mixSpec() {
		const out = {};
		for (const t of state.song?.job?.tracks || []) {
			const m = L.mix[t.name] || {}, e = {};
			if (m.gain) e.gain = (+t.gain || 0) + m.gain;
			if (m.pan != null && Math.abs(m.pan - (+t.pan || 0)) > 1e-6) e.pan = m.pan;
			let mute = m.mute ?? !!t.mute;
			if (L.solo.size && !L.solo.has(t.name)) mute = true;
			if (mute !== !!t.mute) e.mute = mute;
			if (Object.keys(e).length) out[t.name] = e;
		}
		return out;
	}
	let timer = 0;
	function ask(delay = 120) {
		if (!L.active) return;
		if (!L.changedAt) L.changedAt = performance.now();
		clearTimeout(timer);
		timer = setTimeout(send, delay);
	}
	async function send() {
		if (!L.active) return;
		L.rendering = true; changed();
		try {
			const r = await postJson('api/loop', { song: L.song, from: L.from, to: L.to, mix: mixSpec() });
			L.asked = r.want;
			poll();
		} catch (err) { L.error = err.message; L.rendering = false; changed(); }
	}
	async function poll() {
		if (L.polling) return;
		L.polling = true;
		try {
			while (L.active) {
				const r = await fetch('api/loop?song=' + encodeURIComponent(L.song)).then(x => x.json()).catch(() => null);
				if (!L.active) break;
				if (r && r.done > L.heard && r.from === L.from && r.to === L.to) {
					if (r.error) { L.error = r.error; L.heard = r.done; }
					else {
						const res = await fetch('api/loop/audio?song=' + encodeURIComponent(L.song), { cache: 'no-store' });
						const seq = +res.headers.get('X-Wavelength-Loop') || r.done;
						const buffer = await ctx().decodeAudioData(await res.arrayBuffer());
						if (!L.active) break;
						if (seq > L.heard) {
							swapIn(buffer);
							L.heard = seq;
							L.info = r.info;
							const w = r.info.window;
							L.win = [w.songStart, w.songStart + w.seconds];
							L.error = '';
							if (seq >= L.asked && L.changedAt) { L.took = (performance.now() - L.changedAt) / 1000; L.changedAt = 0; }
						}
					}
				}
				L.rendering = !r || r.done < L.asked;
				changed();
				if (!L.rendering) break;
				await new Promise(ok => setTimeout(ok, 100));
			}
		} finally { L.polling = false; }
	}

	/* ---------- the panel: status and a mixer row per track ---------- */
	const fmtDb = v => (v > 0 ? '+' : '') + v.toFixed(1);
	function status() {
		if (!L.active) return '';
		if (L.error) return `<span class="lv-err">${esc(L.error)}</span>`;
		const i = L.info;
		const parts = [];
		if (L.rendering) parts.push('<b>Rendering…</b>');
		if (i) {
			const fresh = i.rendered || [];
			if (L.took != null) parts.push(`heard <b>${L.took.toFixed(1)} s</b> after the change`);
			parts.push(`${i.cached}/${i.tracks} tracks reused`);
			if (fresh.length && fresh.length < i.tracks) parts.push('rendered ' + fresh.map(t => `${esc(t.name)} ${t.seconds.toFixed(1)} s`).join(', '));
			else if (fresh.length) parts.push(`all ${fresh.length} rendered (the first pass fills the cache)`);
		} else if (!L.rendering) parts.push('Waiting…');
		return parts.join(' · ');
	}
	function render() {
		const el = L.el;
		if (!el) return;
		el.hidden = !L.active;
		if (!L.active) return;
		const st = el.querySelector('.lv-status'), head = el.querySelector('.lv-head');
		if (head) head.textContent = `Bars ${L.from}–${L.to - 1} · ${L.playing ? 'playing' : 'paused'}`;
		if (st) st.innerHTML = status();
	}
	function rows() {
		const el = L.el?.querySelector('.lv-mixer');
		if (!el) return;
		const tracks = state.song?.job?.tracks || [];
		el.innerHTML = tracks.map((t, i) => {
			const m = L.mix[t.name] || {}, gain = m.gain || 0, pan = m.pan ?? (+t.pan || 0), mute = m.mute ?? !!t.mute;
			return `<div class="lv-row" data-t="${esc(t.name)}">
				<span class="lv-dot" style="background:${hue(i)}"></span><span class="lv-name" title="${esc(t.name)}">${esc(t.name)}</span>
				<button class="lv-m ${mute ? 'on' : ''}" data-act="mute" title="Mute">M</button><button class="lv-s ${L.solo.has(t.name) ? 'on' : ''}" data-act="solo" title="Solo">S</button>
				<input type="range" class="lv-gain" min="-24" max="12" step="0.5" value="${gain}" title="Fader: dB up or down from the song's own level (double-click: back to it)"><span class="lv-db mono">${fmtDb(gain)}</span>
				<input type="range" class="lv-pan" min="-1" max="1" step="0.05" value="${pan}" title="Pan (double-click: centre)">
			</div>`;
		}).join('');
	}
	function mount(el) {
		L.el = el;
		el.innerHTML = `<div class="lv-top"><span class="lv-head"></span><span class="sp"></span><button class="ed-btn" data-act="reset" title="Back to the song's own mix">Reset mix</button></div>
			<div class="lv-status"></div><div class="lv-mixer"></div>
			<div class="ed-actions"><button class="ed-btn primary" data-act="send" title="Leave these mixer changes as a comment for the agent to make in the song">Send mix to agent</button></div>
			<div class="lv-note">The mixer is for listening: it doesn't change the song until the agent makes it so. Note edits save to edits.json and play on the next change.</div>`;
		el.addEventListener('click', e => {
			const b = e.target.closest('button[data-act]'); if (!b) return;
			const name = b.closest('.lv-row')?.dataset.t, act = b.dataset.act;
			if (act === 'reset') { L.mix = {}; L.solo.clear(); rows(); ask(0); return; }
			if (act === 'send') { sendMix(b); return; }
			if (!name) return;
			const t = (state.song?.job?.tracks || []).find(x => x.name === name), m = L.mix[name] ||= {};
			if (act === 'mute') { m.mute = !(m.mute ?? !!t?.mute); b.classList.toggle('on', m.mute); }
			if (act === 'solo') { L.solo.has(name) ? L.solo.delete(name) : L.solo.add(name); b.classList.toggle('on', L.solo.has(name)); }
			ask();
		});
		el.addEventListener('input', e => {
			const r = e.target.closest('input[type=range]'); if (!r) return;
			const name = r.closest('.lv-row').dataset.t, m = L.mix[name] ||= {};
			if (r.classList.contains('lv-gain')) { m.gain = +r.value; r.nextElementSibling.textContent = fmtDb(m.gain); }
			else m.pan = +r.value;
			ask(250);   // while dragging: one render when the hand pauses
		});
		el.addEventListener('dblclick', e => {
			const r = e.target.closest('input[type=range]'); if (!r) return;
			const m = L.mix[r.closest('.lv-row').dataset.t] ||= {};
			r.value = 0;
			if (r.classList.contains('lv-gain')) { m.gain = 0; r.nextElementSibling.textContent = fmtDb(0); } else m.pan = 0;
			ask();
		});
		rows(); render();
	}

	// the mixer changes, in words, for the agent (a review comment pinned to the loop's bars and those tracks)
	async function sendMix(button) {
		const lines = [], names = [];
		for (const t of state.song?.job?.tracks || []) {
			const m = L.mix[t.name] || {}, say = [];
			if (m.gain) say.push(`fader ${fmtDb(m.gain)} dB (${fmtDb(+t.gain || 0)} -> ${fmtDb((+t.gain || 0) + m.gain)})`);
			if (m.pan != null && Math.abs(m.pan - (+t.pan || 0)) > 1e-6) say.push(`pan ${(+t.pan || 0).toFixed(2)} -> ${m.pan.toFixed(2)}`);
			if (m.mute != null && m.mute !== !!t.mute) say.push(m.mute ? 'muted' : 'unmuted');
			if (say.length) { lines.push(`- ${t.name}: ${say.join(', ')}`); names.push(t.name); }
		}
		if (!lines.length) { button.textContent = 'No mixer changes'; setTimeout(() => { button.textContent = 'Send mix to agent'; }, 1500); return; }
		const solo = L.solo.size ? `\n(Heard with ${[...L.solo].join(', ')} soloed.)` : '';
		const text = `Live mix for bars ${L.from}-${L.to - 1}, heard in the editor. Make these in the song (its gains, pans and mutes):\n${lines.join('\n')}${solo}`;
		const ok = await L.onsend?.(text, names, L.from, L.to);
		button.textContent = ok ? 'Sent ✓' : 'Not sent';
		setTimeout(() => { button.textContent = 'Send mix to agent'; }, 1800);
	}

	return {
		get active() { return L.active; },
		set onchange(fn) { L.onchange = fn; },
		set onsend(fn) { L.onsend = fn; },
		mount,
		// loop bars from..to (to exclusive) of the song; it starts playing when the first loop arrives
		start(song, from, to) {
			if (L.active) this.stop();
			Object.assign(L, { active: true, song, from, to, win: null, buf: null, cur: null, playing: true, pausedAt: 0,
				asked: 0, heard: 0, changedAt: performance.now(), info: null, took: null, error: '' });
			ctx().resume();
			rows(); send();
		},
		stop() {
			if (!L.active) return;
			fadeOut(L.cur);
			const song = L.song;
			Object.assign(L, { active: false, cur: null, playing: false, buf: null, win: null });
			postJson('api/loop/stop', { song }).catch(() => {});
			changed();
		},
		toggle() { if (L.active) (L.playing ? pause() : play()); },
		playing: () => L.active && L.playing,
		window: () => (L.active ? { from: L.from, to: L.to, sec: L.win } : null),
		songSec: () => (L.win ? L.win[0] + pos() : 0),
		// true when the song second is inside the loop (the playhead moves there)
		seek(sec) {
			if (!L.win || !L.buf) return false;
			const p = sec - L.win[0];
			if (p < 0 || p >= len()) return false;
			if (L.playing) { const old = L.cur, at = L.ctx.currentTime + 0.02; L.cur = voice(L.buf, at, p, true); fadeOut(old, at); }
			else L.pausedAt = p;
			return true;
		},
		// the job or its edits changed (a saved note edit, the agent re-running make-job.py): hear it
		refresh() { if (L.active) { rows(); L.changedAt = performance.now(); ask(60); } },
	};
})();
