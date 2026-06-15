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

    let lastSent     = 0;
    let lastCount    = null;
    const origFetch  = window.fetch;

    window.fetch = async function (...args) {
        const res = await origFetch.apply(this, args);
        const url = typeof args[0] === 'string' ? args[0] : args[0]?.url ?? '';

        // YouTube Studio internal API that returns channel stats
        if (url.includes('youtubei/v1/creator/get_creator_channels') ||
            url.includes('youtubei/v1/yta/get_endpoint')) {
            res.clone().json().then(data => {
                const count = extractCount(data);
                if (count !== null) maybePublish(count);
            }).catch(() => {});
        }
        return res;
    };

    function extractCount(data) {
        // Walk the response tree looking for subscriber count
        const str = JSON.stringify(data);
        const m = str.match(/"subscriberCount"\s*:\s*"?(\d+)"?/);
        return m ? parseInt(m[1], 10) : null;
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
            onload:  (r) => updateBadge(count, r.status === 200 ? 'ok' : `err ${r.status}`),
            onerror: ()  => updateBadge(count, 'net err'),
        });
    }

    // --- minimal status badge ---
    const badge = document.createElement('div');
    Object.assign(badge.style, {
        position: 'fixed', bottom: '12px', right: '12px', zIndex: 99999,
        background: '#111', color: '#0f0', fontFamily: 'monospace',
        fontSize: '11px', padding: '4px 8px', borderRadius: '4px',
        opacity: '0.75', pointerEvents: 'none',
    });
    document.body?.appendChild(badge);

    function updateBadge(count, status) {
        badge.textContent = `subs: ${count.toLocaleString()}  [${status}]`;
    }
})();
