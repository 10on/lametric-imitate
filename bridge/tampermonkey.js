// ==UserScript==
// @name         YT Subscriber Bridge
// @namespace    lametric
// @version      1.0
// @description  Sends exact subscriber count from YouTube Studio to local MQTT bridge
// @match        https://studio.youtube.com/*
// @grant        GM_xmlhttpRequest
// @connect      192.168.8.150
// ==/UserScript==

(function () {
    const BRIDGE_URL    = 'http://192.168.8.150:7431/subscribers';
    const MIN_INTERVAL  = 30_000; // don't spam more often than 30s

    let lastSent  = 0;
    let lastCount = null;

    // Studio uses XHR, not fetch — intercept XMLHttpRequest
    const origOpen = XMLHttpRequest.prototype.open;
    const origSend = XMLHttpRequest.prototype.send;

    XMLHttpRequest.prototype.open = function (method, url, ...rest) {
        this._ytbUrl = url;
        return origOpen.call(this, method, url, ...rest);
    };

    XMLHttpRequest.prototype.send = function (body) {
        const url = this._ytbUrl ?? '';
        if (url.includes('youtubei/v1/yta_web/join')) {
            this.addEventListener('load', function () {
                try {
                    const count = extractCount(JSON.parse(this.responseText));
                    if (count !== null) maybePublish(count);
                } catch {}
            });
        }
        return origSend.call(this, body);
    };

    function extractCount(data) {
        try {
            for (const result of data.results ?? []) {
                const cards = result?.value?.getCards?.cards ?? [];
                for (const card of cards) {
                    const total = card?.cumulativeSubscribersCardData?.lifetimeTotal;
                    if (typeof total === 'number') return total;
                }
            }
        } catch {}
        return null;
    }

    function maybePublish(count) {
        const now = Date.now();
        if (count === lastCount && now - lastSent < MIN_INTERVAL) return;
        if (now - lastSent < 5_000) return;  // absolute minimum gap

        lastCount = count;
        lastSent  = now;
        updateBadge(count, 'sending...');

        GM_xmlhttpRequest({
            method:  'POST',
            url:     BRIDGE_URL,
            headers: { 'Content-Type': 'application/json' },
            data:    JSON.stringify({ count }),
            onload:  (r) => updateBadge(count, r.status === 200 ? 'ok' : 'err'),
            onerror: ()  => updateBadge(count, 'err'),
        });
    }

    // --- status badge ---
    let lastOkTime   = 0;
    let lastOkCount  = null;
    let badgeStatus  = 'waiting'; // waiting | sending | ok | err

    const badge = document.createElement('div');
    Object.assign(badge.style, {
        position: 'fixed', bottom: '16px', right: '16px', zIndex: 99999,
        background: '#1a1a1a', fontFamily: 'monospace', fontSize: '12px',
        padding: '8px 12px', borderRadius: '6px', lineHeight: '1.6',
        border: '1px solid #333', boxShadow: '0 2px 8px rgba(0,0,0,0.5)',
        pointerEvents: 'none', minWidth: '160px',
        transition: 'border-color 0.3s',
    });
    document.body?.appendChild(badge);

    const COLORS = { waiting: '#666', sending: '#fa0', ok: '#0f0', err: '#f44' };

    function renderBadge() {
        const color = COLORS[badgeStatus];
        const ago   = lastOkTime
            ? (() => { const s = Math.round((Date.now() - lastOkTime) / 1000);
                return s < 60 ? `${s}s ago` : `${Math.floor(s/60)}m ${s%60}s ago`; })()
            : '—';
        badge.style.borderColor = color;
        badge.innerHTML =
            `<div style="color:#888;font-size:10px">📡 YT Bridge</div>` +
            `<div style="color:#fff;font-size:15px;font-weight:bold">${lastOkCount !== null ? lastOkCount.toLocaleString() : '—'}</div>` +
            `<div style="color:${color}">${badgeStatus}${badgeStatus === 'ok' ? ` · ${ago}` : ''}</div>`;
    }

    function updateBadge(count, status) {
        badgeStatus = status;
        if (status === 'ok') { lastOkTime = Date.now(); lastOkCount = count; }
        if (status === 'sending' && count !== null) lastOkCount = count;
        // flash
        badge.style.opacity = '1';
        setTimeout(() => { badge.style.opacity = '0.82'; }, 300);
        renderBadge();
    }

    // live "ago" timer
    setInterval(renderBadge, 5000);
    renderBadge();
})();
