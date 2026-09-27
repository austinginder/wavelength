/* Right-click menus and the page's own dialogs (confirm, a text prompt, the song details form).
 *
 * The menu is a popover and the dialogs are modal <dialog>s, so both sit in the top layer: they
 * show above the arrangement editor, which is a modal dialog itself.
 *
 * WLUI.menu(event, items): items are {label, hint?, danger?, disabled?, run} or '-' (a separator).
 * WLUI.confirm({title, body, ok, danger}) -> Promise<boolean>
 * WLUI.prompt({title, body, label, value, ok, check}) -> Promise<string|null>; check(value) returns an
 *   error message or '' and runs as you type.
 */
(() => {
	const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

	/* ---------- context menu ---------- */
	let menuEl = null, menuItems = [], active = -1;
	function menuBuild() {
		menuEl = document.createElement('div');
		menuEl.className = 'wl-menu';
		menuEl.setAttribute('popover', 'manual');
		menuEl.setAttribute('role', 'menu');
		document.body.appendChild(menuEl);
		menuEl.addEventListener('click', e => {
			const b = e.target.closest('button[data-i]');
			if (!b || b.disabled) return;
			const it = menuItems[+b.dataset.i];
			close();
			it.run?.();
		});
		menuEl.addEventListener('mousemove', e => { const b = e.target.closest('button[data-i]'); if (b) focusItem(+b.dataset.i); });
		// any press outside, a scroll, a blur or Esc closes it
		addEventListener('pointerdown', e => { if (isOpen() && !menuEl.contains(e.target)) close(); }, true);
		addEventListener('blur', () => close());
		addEventListener('wheel', e => { if (isOpen() && !menuEl.contains(e.target)) close(); }, { passive: true, capture: true });
		addEventListener('keydown', e => {
			if (!isOpen()) return;
			const live = menuItems.map((it, i) => it !== '-' && !it.disabled ? i : -1).filter(i => i >= 0);
			if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); }
			else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
				e.preventDefault(); e.stopPropagation();
				const at = live.indexOf(active), step = e.key === 'ArrowDown' ? 1 : -1;
				focusItem(live[(at + step + live.length) % live.length] ?? live[0]);
			} else if (e.key === 'Enter' && active >= 0) { e.preventDefault(); e.stopPropagation(); menuEl.querySelector(`button[data-i="${active}"]`)?.click(); }
		}, true);
	}
	const isOpen = () => menuEl?.matches(':popover-open');
	function close() { if (isOpen()) menuEl.hidePopover(); active = -1; }
	function focusItem(i) {
		active = i;
		menuEl.querySelectorAll('button[data-i]').forEach(b => b.classList.toggle('act', +b.dataset.i === i));
	}
	function menu(e, items) {
		e.preventDefault();
		if (!menuEl) menuBuild();
		menuItems = items.filter(Boolean);
		while (menuItems[0] === '-') menuItems.shift();
		while (menuItems[menuItems.length - 1] === '-') menuItems.pop();
		if (!menuItems.length) return;
		menuEl.innerHTML = menuItems.map((it, i) => it === '-' ? '<hr>'
			: `<button type="button" role="menuitem" data-i="${i}" class="${it.danger ? 'danger' : ''}" ${it.disabled ? 'disabled' : ''}>${esc(it.label)}${it.hint ? `<span>${esc(it.hint)}</span>` : ''}</button>`).join('');
		if (isOpen()) menuEl.hidePopover();
		// inside the top modal dialog (the editor), since a modal makes everything outside it inert
		const host = [...document.querySelectorAll('dialog[open]')].filter(d => d.matches(':modal')).pop() || document.body;
		if (menuEl.parentNode !== host) host.appendChild(menuEl);
		menuEl.style.left = '0px'; menuEl.style.top = '0px';
		menuEl.showPopover();
		const r = menuEl.getBoundingClientRect();
		menuEl.style.left = Math.max(4, Math.min(e.clientX, innerWidth - r.width - 6)) + 'px';
		menuEl.style.top = Math.max(4, Math.min(e.clientY, innerHeight - r.height - 6)) + 'px';
		active = -1;
	}

	/* ---------- dialogs ---------- */
	// one modal: title, body html, footer buttons; resolves with what closed it
	function modal({ title, body, buttons, init, cls = '' }) {
		return new Promise(resolve => {
			const d = document.createElement('dialog');
			d.className = 'wl-dlg ' + cls;
			d.innerHTML = `<form method="dialog"><h3>${esc(title)}</h3><div class="wl-dlg-body">${body || ''}</div>
				<p class="wl-dlg-err" hidden></p>
				<div class="wl-dlg-foot">${buttons.map(b => `<button value="${esc(b.value)}" class="ed-btn ${b.cls || ''}" ${b.value === 'cancel' ? 'formnovalidate' : ''}>${esc(b.label)}</button>`).join('')}</div></form>`;
			document.body.appendChild(d);
			let result = 'cancel';
			const api = {
				dlg: d,
				error(msg) { const p = d.querySelector('.wl-dlg-err'); p.hidden = !msg; p.textContent = msg || ''; },
				busy(on) { d.querySelectorAll('.wl-dlg-foot button').forEach(b => b.disabled = on); },
				done(v) { result = v; d.close(); },
			};
			d.addEventListener('close', () => { d.remove(); resolve(result === 'cancel' ? null : { value: result, api }); });
			d.querySelector('form').addEventListener('submit', e => {
				const v = e.submitter?.value ?? 'ok';
				if (v === 'cancel') return;   // closes with 'cancel'
				e.preventDefault();
				const b = buttons.find(x => x.value === v);
				if (b?.submit) b.submit(api); else api.done(v);
			});
			d.addEventListener('keydown', e => e.stopPropagation());   // keys belong to the dialog, not the editor behind it
			d.showModal();
			init?.(api);
		});
	}
	async function confirmDlg({ title, body, ok = 'OK', danger = false }) {
		return !!(await modal({ title, body: `<p>${body}</p>`, buttons: [
			{ value: 'cancel', label: 'Cancel' }, { value: 'ok', label: ok, cls: danger ? 'danger' : 'primary' }],
			init: api => api.dlg.querySelector(`button[value="${danger ? 'cancel' : 'ok'}"]`).focus() }));
	}
	async function promptDlg({ title, body = '', label = '', value = '', ok = 'Save', check }) {
		let out = null;
		await modal({ title, body: `${body ? `<p>${body}</p>` : ''}<label class="wl-field"><span>${esc(label)}</span><input name="v" value="${esc(value)}" autocomplete="off" spellcheck="false"></label>`,
			buttons: [{ value: 'cancel', label: 'Cancel' }, { value: 'ok', label: ok, cls: 'primary', submit: api => {
				const v = api.dlg.querySelector('input').value.trim(), err = check?.(v) || '';
				if (err) return api.error(err);
				out = v; api.done('ok');
			} }],
			init: api => {
				const i = api.dlg.querySelector('input');
				i.focus(); i.select();
				if (check) i.addEventListener('input', () => api.error(i.value.trim() === value ? '' : check(i.value.trim())));
			} });
		return out;
	}

	window.WLUI = { menu, closeMenu: close, modal, confirm: confirmDlg, prompt: promptDlg, esc };
})();
