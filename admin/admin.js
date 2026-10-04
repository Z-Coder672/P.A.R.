// P.A.R. admin panel. Renders the three lists from api.php's `state` and sends
// every action back to it; each response carries fresh state, so the DOM is
// always re-rendered from what the server actually holds.
(() => {
    'use strict';

    const GRID_W = 37;
    const GRID_H = 18;
    const SCALE = 4;
    const REFRESH_MS = 10000;

    const csrf = document.body.dataset.csrf;
    const listEl = document.getElementById('list');
    const hintEl = document.getElementById('hint');
    const toastEl = document.getElementById('toast');
    const dialogEl = document.getElementById('dialog');

    let state = { mod: [], queue: [], gallery: [], max_queue: 100 };
    let rendered = '';
    let dragging = null;
    let tab = 'mod';
    try {
        tab = localStorage.getItem('par-admin-tab') || 'mod';
    } catch (e) { /* storage unavailable */ }
    if (!['mod', 'queue', 'gallery'].includes(tab)) tab = 'mod';

    const HINTS = {
        mod: 'Submissions waiting on moderation. Approving moves one into the print queue; rejecting removes it.',
        queue: 'Approved prints, in the order the device will take them. Drag a row or use the arrows to reorder.',
        gallery: 'Everything the device has picked up, newest first. "In progress" entries are still pending.json on disk.',
    };

    const ERRORS = {
        not_found: 'That item changed or is gone — the list has been refreshed.',
        main_queue_full: 'The print queue is full.',
        bad_video_id: 'A YouTube video id is exactly 11 characters (letters, digits, - and _).',
        bad_bitmap: 'That entry has no valid bitmap to print.',
        no_entry_json: 'That gallery folder has no entry file; it can only be deleted.',
        bad_csrf: 'Session token mismatch — reload the page.',
        lock_failed: 'The server could not lock the queue file. Try again.',
        trash_failed: 'The server could not move the entry to the trash folder; nothing was removed.',
    };

    // ---------------------------------------------------------- helpers ---

    function el(tag, props, ...children) {
        const node = document.createElement(tag);
        for (const [k, v] of Object.entries(props || {})) {
            if (k === 'class') node.className = v;
            else if (k === 'text') node.textContent = v;
            else if (k === 'onclick') node.addEventListener('click', v);
            else if (v === true) node.setAttribute(k, '');
            else if (v !== false && v != null) node.setAttribute(k, v);
        }
        for (const child of children) {
            if (child != null) node.append(child);
        }
        return node;
    }

    const button = (label, onclick, cls = '', title = '') =>
        el('button', { class: `btn ${cls}`.trim(), type: 'button', text: label, onclick, title: title || null });

    let toastTimer = 0;
    function toast(message, isError = false) {
        toastEl.textContent = message;
        toastEl.className = 'toast' + (isError ? ' error' : '');
        toastEl.hidden = false;
        clearTimeout(toastTimer);
        toastTimer = setTimeout(() => { toastEl.hidden = true; }, isError ? 6000 : 2500);
    }

    function bitmapCanvas(base64) {
        const canvas = el('canvas', { class: 'thumb', width: GRID_W * SCALE, height: GRID_H * SCALE });
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        let raw = '';
        try {
            raw = atob(base64);
        } catch (e) { /* leave it black */ }
        ctx.fillStyle = '#02b2d9';
        for (let i = 0; i < GRID_W * GRID_H; i++) {
            if ((raw.charCodeAt(i >> 3) >> (7 - (i & 7))) & 1) {
                ctx.fillRect((i % GRID_W) * SCALE, Math.floor(i / GRID_W) * SCALE, SCALE - 1, SCALE - 1);
            }
        }
        return canvas;
    }

    function ago(ts) {
        if (!ts) return '';
        const s = Math.max(0, Math.floor(Date.now() / 1000) - ts);
        if (s < 90) return 'just now';
        if (s < 5400) return `${Math.round(s / 60)} min ago`;
        if (s < 129600) return `${Math.round(s / 3600)} h ago`;
        return `${Math.round(s / 86400)} d ago`;
    }

    const label = (item) => (item.name ? `“${item.name}”` : 'this untitled piece');

    // Whether the submitter left a notification email. Always shown, so "no
    // email" is a stated fact rather than a missing badge.
    const emailTag = (has) => (has
        ? el('span', { class: 'badge mail', text: '✉ email added', title: 'The submitter will be emailed when this prints' })
        : el('span', { text: 'no email' }));

    function nameBlock(item, ...meta) {
        return el('div', { class: 'info' },
            el('div', { class: 'name' + (item.name ? '' : ' untitled'), text: item.name || '(untitled)' }),
            el('div', { class: 'meta' },
                item.artist ? el('span', { text: `by ${item.artist}` }) : null,
                ...meta));
    }

    // -------------------------------------------------------------- api ---

    async function api(action, params = {}, okMessage = '') {
        let res;
        let body = null;
        try {
            res = await fetch('api.php', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf },
                body: JSON.stringify({ action, ...params }),
            });
            body = await res.json().catch(() => null);
        } catch (e) {
            toast('Could not reach the server.', true);
            return false;
        }
        if (res.status === 401) {
            window.location.reload();
            return false;
        }
        if (body && body.state) {
            state = body.state;
            render();
        }
        if (!res.ok || !body || !body.ok) {
            const code = (body && body.error) || `HTTP ${res.status}`;
            toast(ERRORS[code] || `Failed: ${code}`, true);
            return false;
        }
        if (okMessage) toast(okMessage);
        return true;
    }

    // ----------------------------------------------------------- dialog ---

    // Resolves to the dialog's field values on confirm, or null on cancel.
    function ask({ title, text, fields = [], confirm = 'OK', danger = false }) {
        return new Promise((resolve) => {
            const inputs = {};
            const form = el('form', { method: 'dialog' }, el('h2', { text: title }));
            for (const paragraph of [].concat(text || [])) {
                form.append(el('p', { text: paragraph }));
            }
            for (const f of fields) {
                if (f.type === 'check') {
                    const box = el('input', { type: 'checkbox', checked: !!f.value });
                    inputs[f.name] = () => box.checked;
                    form.append(el('label', { class: 'check' }, box, el('span', { text: f.label })));
                } else {
                    const input = el('input', { type: 'text', maxlength: f.max || 100, value: f.value || '', placeholder: f.placeholder || null });
                    inputs[f.name] = () => input.value;
                    form.append(el('label', { text: f.label }, input));
                }
            }
            const cancel = button('Cancel', () => dialogEl.close('cancel'));
            const ok = el('button', { class: 'btn ' + (danger ? 'danger solid' : 'primary'), type: 'submit', value: 'ok', text: confirm });
            form.append(el('div', { class: 'buttons' }, cancel, ok));

            dialogEl.replaceChildren(form);
            dialogEl.returnValue = '';
            dialogEl.addEventListener('close', () => {
                if (dialogEl.returnValue !== 'ok') return resolve(null);
                const values = {};
                for (const [name, read] of Object.entries(inputs)) values[name] = read();
                resolve(values);
            }, { once: true });
            dialogEl.showModal();
        });
    }

    // Click on the backdrop (the dialog element itself, outside its form) cancels.
    dialogEl.addEventListener('click', (e) => {
        if (e.target === dialogEl) dialogEl.close('cancel');
    });

    // --------------------------------------------------------- reorder ---

    function move(list, key, to) {
        const keys = state[list].map((i) => i.key);
        const from = keys.indexOf(key);
        if (from < 0 || to < 0 || to >= keys.length || to === from) return;
        keys.splice(to, 0, keys.splice(from, 1)[0]);
        api('reorder', { list, keys });
    }

    function makeDraggable(row, list, key) {
        row.draggable = true;
        row.addEventListener('dragstart', (e) => {
            dragging = key;
            row.classList.add('dragging');
            e.dataTransfer.effectAllowed = 'move';
            e.dataTransfer.setData('text/plain', key);
        });
        row.addEventListener('dragend', () => {
            dragging = null;
            row.classList.remove('dragging');
            clearDropMarks();
        });
        row.addEventListener('dragover', (e) => {
            if (!dragging || dragging === key) return;
            e.preventDefault();
            const r = row.getBoundingClientRect();
            const after = e.clientY > r.top + r.height / 2;
            clearDropMarks();
            row.classList.add(after ? 'drop-after' : 'drop-before');
        });
        row.addEventListener('drop', (e) => {
            if (!dragging || dragging === key) return;
            e.preventDefault();
            const after = row.classList.contains('drop-after');
            const keys = state[list].map((i) => i.key).filter((k) => k !== dragging);
            keys.splice(keys.indexOf(key) + (after ? 1 : 0), 0, dragging);
            dragging = null;
            clearDropMarks();
            api('reorder', { list, keys });
        });
    }

    function clearDropMarks() {
        for (const r of listEl.querySelectorAll('.drop-before, .drop-after')) {
            r.classList.remove('drop-before', 'drop-after');
        }
    }

    function orderControls(list, item, index) {
        const last = state[list].length - 1;
        return [
            el('button', { class: 'btn', type: 'button', text: 'Top', title: 'Move to front', disabled: index === 0, onclick: () => move(list, item.key, 0) }),
            el('button', { class: 'btn icon', type: 'button', text: '↑', title: 'Move up', disabled: index === 0, onclick: () => move(list, item.key, index - 1) }),
            el('button', { class: 'btn icon', type: 'button', text: '↓', title: 'Move down', disabled: index === last, onclick: () => move(list, item.key, index + 1) }),
        ];
    }

    async function editLine(list, item) {
        const v = await ask({
            title: 'Edit details',
            fields: [
                { name: 'name', label: 'Piece name', value: item.name },
                { name: 'artist', label: 'Artist credit (optional)', value: item.artist },
            ],
            confirm: 'Save',
        });
        if (v) api('edit', { list, key: item.key, name: v.name, artist: v.artist }, 'Saved');
    }

    // ------------------------------------------------------------ rows ---

    const MOD_STATUS = {
        pending: 'Awaiting auto-check',
        processing: 'Being auto-checked',
        email_sent: 'Held for human review',
    };

    function modRow(item, index) {
        const status = el('select', { class: 'btn', title: 'Moderation status' });
        const known = Object.keys(MOD_STATUS).includes(item.status);
        for (const [value, text] of Object.entries(MOD_STATUS)) {
            status.append(el('option', { value, text, selected: value === item.status }));
        }
        if (!known) status.append(el('option', { value: '', text: item.status, selected: true, disabled: true }));
        status.addEventListener('change', () => api('mod_status', { key: item.key, status: status.value }, 'Status updated'));

        const row = el('div', { class: 'row' },
            el('span', { class: 'grip', text: '⠿', title: 'Drag to reorder' }),
            bitmapCanvas(item.bitmap),
            nameBlock(item,
                el('span', { text: `submitted ${ago(item.ts)}` }),
                emailTag(item.notify)),
            el('div', { class: 'actions' },
                status,
                ...orderControls('mod', item, index),
                button('Edit', () => editLine('mod', item)),
                button('Duplicate', () => api('duplicate', { list: 'mod', key: item.key }, 'Duplicated'), '', 'Add a copy right below (the copy has no notification email)'),
                button('Approve', () => api('approve', { key: item.key, position: 'end' }, 'Approved — added to the print queue'), 'primary'),
                button('Approve → front', () => api('approve', { key: item.key, position: 'front' }, 'Approved — next to print'), '', 'Approve and put at the front of the print queue'),
                button('Reject', async () => {
                    const v = await ask({
                        title: 'Reject submission',
                        text: `Reject ${label(item)}? It is removed from moderation and will not print.`
                            + (item.notify ? ' The submitter’s notification email on file is discarded.' : ''),
                        confirm: 'Reject',
                        danger: true,
                    });
                    if (v) api('delete', { list: 'mod', key: item.key }, 'Rejected');
                }, 'danger')));
        makeDraggable(row, 'mod', item.key);
        return row;
    }

    function queueRow(item, index) {
        const row = el('div', { class: 'row' },
            el('span', { class: 'grip', text: '⠿', title: 'Drag to reorder' }),
            el('span', { class: 'pos', text: `${index + 1}.` }),
            bitmapCanvas(item.bitmap),
            nameBlock(item,
                index === 0 ? el('span', { class: 'badge ok', text: 'next to print' }) : null,
                emailTag(item.notify)),
            el('div', { class: 'actions' },
                ...orderControls('queue', item, index),
                button('Edit', () => editLine('queue', item)),
                button('Duplicate', () => api('duplicate', { list: 'queue', key: item.key }, 'Duplicated'), '', 'Add a copy right below (the copy has no notification email)'),
                button('Back to moderation', () => api('demote', { key: item.key }, 'Moved to moderation (held for review)'), '',
                    'Take out of the print queue and hold in moderation'),
                button('Delete', async () => {
                    const v = await ask({
                        title: 'Delete from print queue',
                        text: `Delete ${label(item)} from the print queue? It will not print.`
                            + (item.notify ? ' The submitter’s notification email on file is discarded.' : ''),
                        confirm: 'Delete',
                        danger: true,
                    });
                    if (v) api('delete', { list: 'queue', key: item.key }, 'Deleted from the queue');
                }, 'danger')));
        makeDraggable(row, 'queue', item.key);
        return row;
    }

    function galleryRow(item) {
        const meta = [];
        if (item.file === null) {
            meta.push(el('span', { class: 'badge bad', text: 'no entry file' }));
        } else if (item.pending) {
            meta.push(el('span', { class: 'badge wait', text: 'in progress' }));
        } else if (item.file === 'pending') {
            meta.push(el('span', { class: 'badge wait', text: 'pending on disk, shown complete (has photo)' }));
        } else {
            meta.push(el('span', { class: 'badge ok', text: 'complete' }));
        }
        if (item.video_id) {
            meta.push(el('a', { href: `https://youtu.be/${encodeURIComponent(item.video_id)}`, target: '_blank', rel: 'noopener', text: 'video' }));
        } else {
            meta.push(el('span', { text: 'no video' }));
        }
        if (item.notify === 'owed') meta.push(el('span', { class: 'badge mail', text: '✉ email added — not sent yet' }));
        else if (item.notify === 'sent') meta.push(el('span', { class: 'badge ok', text: '✉ email added — sent' }));
        else if (item.notify === 'sending') meta.push(el('span', { class: 'badge wait', text: '✉ email added — sending' }));
        else meta.push(el('span', { text: 'no email' }));

        const actions = el('div', { class: 'actions' });
        if (item.file === 'pending') {
            actions.append(button('Mark complete', async () => {
                let notify = false;
                if (item.notify === 'owed') {
                    const v = await ask({
                        title: `Mark #${item.id} complete`,
                        text: 'The submitter asked to be emailed when this piece is done.',
                        fields: [{ type: 'check', name: 'notify', label: 'Send them the “your piece is done” email now', value: false }],
                        confirm: 'Mark complete',
                    });
                    if (!v) return;
                    notify = v.notify;
                }
                api('gallery_status', { id: item.id, status: 'complete', notify }, `#${item.id} marked complete`);
            }));
        } else if (item.file === 'info') {
            actions.append(button('Mark pending', () => api('gallery_status', { id: item.id, status: 'pending' }, `#${item.id} marked pending`), '',
                item.image ? 'Has a photo, so the public gallery will still show it as complete' : ''));
        }
        if (item.file !== null) {
            actions.append(
                button('Edit', async () => {
                    const v = await ask({
                        title: `Edit #${item.id}`,
                        fields: [
                            { name: 'name', label: 'Piece name', value: item.name },
                            { name: 'artist', label: 'Artist credit (optional)', value: item.artist },
                            { name: 'video_id', label: 'YouTube video id (blank = none)', value: item.video_id || '', max: 11, placeholder: '11 characters' },
                        ],
                        confirm: 'Save',
                    });
                    if (v) api('gallery_edit', { id: item.id, name: v.name, artist: v.artist, video_id: v.video_id }, 'Saved');
                }),
                button('Back to queue…', async () => {
                    const v = await ask({
                        title: `Send #${item.id} back to the print queue`,
                        text: `${label(item)} will be printed again.`
                            + (item.notify ? ' The submitter’s email goes with it, so they are notified when the reprint finishes.' : ''),
                        fields: [
                            { type: 'check', name: 'front', label: 'Put it at the front of the queue (prints next)', value: true },
                            {
                                type: 'check', name: 'remove', value: item.file === 'pending',
                                label: 'Remove this gallery entry (the reprint gets a new one). Leave unticked to keep it and just print a copy.',
                            },
                        ],
                        confirm: 'Send to queue',
                    });
                    if (v) {
                        api('gallery_requeue', { id: item.id, position: v.front ? 'front' : 'end', remove: v.remove },
                            v.remove ? `#${item.id} moved back to the queue` : `Copy of #${item.id} added to the queue`);
                    }
                }));
        }
        actions.append(button('Delete', async () => {
            const v = await ask({
                title: `Delete gallery entry #${item.id}`,
                text: [
                    `Remove ${label(item)} from the gallery? The entry and its photo are moved to the server’s private trash folder, so it can be restored by hand. Its YouTube video is not touched.`,
                    item.pending ? 'This entry is still in progress — if the device is printing it right now, its photo and video will have nowhere to attach.' : null,
                ].filter(Boolean),
                confirm: 'Delete',
                danger: true,
            });
            if (v) api('gallery_delete', { id: item.id }, `#${item.id} moved to trash`);
        }, 'danger'));

        return el('div', { class: 'row' },
            el('span', { class: 'pos', text: `#${item.id}` }),
            bitmapCanvas(item.bitmap),
            item.image
                ? el('a', { href: `../${item.image}`, target: '_blank', rel: 'noopener' },
                    el('img', { class: 'photo', src: `../${item.image}`, alt: `Photo of #${item.id}`, loading: 'lazy' }))
                : null,
            nameBlock(item, ...meta),
            actions);
    }

    // ---------------------------------------------------------- render ---

    function render() {
        const snapshot = JSON.stringify([tab, state]);
        for (const name of ['mod', 'queue', 'gallery']) {
            document.getElementById(`count-${name}`).textContent = state[name].length;
        }
        for (const t of document.querySelectorAll('.tab')) {
            t.classList.toggle('active', t.dataset.tab === tab);
        }
        if (snapshot === rendered) return; // unchanged — don't disturb an open dropdown
        rendered = snapshot;

        hintEl.textContent = HINTS[tab]
            + (tab === 'queue' ? ` ${state.queue.length} of ${state.max_queue} slots used.` : '');
        const items = state[tab];
        if (!items.length) {
            listEl.replaceChildren(el('div', { class: 'empty', text: 'Nothing here.' }));
            return;
        }
        const build = { mod: modRow, queue: queueRow, gallery: galleryRow }[tab];
        listEl.replaceChildren(...items.map((item, i) => build(item, i)));
    }

    for (const t of document.querySelectorAll('.tab')) {
        t.addEventListener('click', () => {
            tab = t.dataset.tab;
            try {
                localStorage.setItem('par-admin-tab', tab);
            } catch (e) { /* storage unavailable */ }
            render();
        });
    }

    document.getElementById('refresh').addEventListener('click', () => api('state', {}, 'Refreshed'));

    document.getElementById('logout').addEventListener('click', async () => {
        await fetch('logout.php', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf },
            body: '{}',
        }).catch(() => {});
        window.location.reload();
    });

    setInterval(() => {
        if (!document.hidden && !dialogEl.open && !dragging) api('state');
    }, REFRESH_MS);

    render();
    api('state');
})();
