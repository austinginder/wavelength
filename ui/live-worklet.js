/* Live playing (playground.js): the stream's PCM goes into a ring buffer here and plays out at the
 * output's pace. Starts after 35 ms are buffered; a late burst that piles up more than 160 ms drops
 * back to 50 ms, so the delay from key to sound stays short. */
class WlLive extends AudioWorkletProcessor {
constructor() { super(); this.buf = new Float32Array(sampleRate * 8); this.r = 0; this.n = 0; this.on = false;
	this.port.onmessage = e => { const d = e.data, B = this.buf, L = B.length; let w = (this.r + this.n) % L;
		for (let i = 0; i < d.length; i++) { B[w] = d[i]; w = w + 1 === L ? 0 : w + 1; }
		this.n = Math.min(L, this.n + d.length);
		const most = Math.round(sampleRate * 0.16) * 2, keep = Math.round(sampleRate * 0.05) * 2;   // a late burst: drop back to 50 ms
		if (this.n > most) { this.r = (this.r + this.n - keep) % L; this.n = keep; } }; }
process(_, outs) { const o = outs[0], L = o[0], R = o[1] || o[0], B = this.buf, len = B.length;
	if (!this.on) { if (this.n < Math.round(sampleRate * 0.035) * 2) return true; this.on = true; }
	for (let i = 0; i < L.length; i++) {
		if (this.n >= 2) { L[i] = B[this.r]; R[i] = B[this.r + 1]; this.r = (this.r + 2) % len; this.n -= 2; }
		else { L[i] = R[i] = 0; this.on = false; } }
	return true; } }
registerProcessor('wl-live', WlLive);
