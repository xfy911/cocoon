console.log('Cocoon test loaded');
function greet(name) { return 'Hello, ' + name + '!'; }
const utils = {
    formatDate: (d) => d.toISOString().split('T')[0],
    debounce: (fn, ms) => { let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); }; },
    throttle: (fn, ms) => { let last = 0; return (...a) => { const now = Date.now(); if (now - last >= ms) { last = now; fn(...a); } }; }
};
const api = {
    get: async (url) => { const r = await fetch(url); return r.json(); },
    post: async (url, data) => { const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data) }); return r.json(); }
};
