// The DermEdge band over Web Bluetooth (Chrome and Edge; firmware_dermedge/band/). Connects,
// waits for the 6-digit pairing code the band shows, then keeps the page and the band in step:
// the band's recordings become sessions in /api/band/* (local-api.js in the browser, app/band.py
// on the hub), its readings are relayed there, and flare alerts go back to its screen.
//
// Protocol (firmware_dermedge/band/main/ble.h): service a3c87510-..., status (JSON, ~1 Hz),
// data (one reading per notification, tagged with the band's recording number r and t = seconds
// since that recording started) and command (start[:label], stop, dismiss, alert:...).
(function () {
  const UUID = n => `a3c875${n}-8ed3-4bdf-8a39-a01bebede295`;
  const SVC = UUID('10'), STATUS = UUID('11'), DATA = UUID('12'), CMD = UUID('13');
  const PAIR_WAIT_MS = 120000, RECONNECTS = 10, FLUSH_MS = 1500;
  const enc = new TextEncoder(), dec = new TextDecoder();

  const st = {
    supported: !!(navigator.bluetooth && window.isSecureContext),
    phase: 'idle',       // idle, choosing, connecting, pairing, connected, reconnecting
    name: null, id: null, status: null, error: null,
    alert: null,         // the newest undismissed alert, as sent to the band
    sessionsChanged: 0,  // bumps when a recording starts or stops, for the page to reload its list
  };
  const listeners = new Set();
  const emit = () => listeners.forEach(f => { try { f(st); } catch (e) { console.error(e); } });
  const set = patch => { Object.assign(st, patch); emit(); };

  let device = null, ch = {}, userClosed = false, gatt = Promise.resolve();
  // Chrome allows one GATT operation at a time ("GATT operation already in progress")
  const op = f => (gatt = gatt.then(f, f));
  const api = (url, body) => fetch(url, body === undefined ? undefined : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }).then(r => r.json());

  // ---------- recordings: band r -> session id ----------
  // A band reboot restarts r at 1, so the map belongs to one connection's run of status messages.
  let recs = new Map(), queue = [], lastStatus = null;
  const now = () => Date.now() / 1000;

  // el: seconds the band has been recording (from its status), or an estimate for a recording
  // that only shows up afterwards, from readings it made offline
  function session(r, el, src, label) {
    let rec = recs.get(r);
    if (rec) return rec.ready;
    rec = { r, startedAt: now() - el, stopped: false, sessionId: null };
    recs.set(r, rec);  // before any await, so two callers can't both start it
    rec.ready = (async () => {
      // a newer recording means older ones have ended; close them once their readings are in
      for (const [k, old] of recs) if (k < r && !old.stopped) await stopRecording(k);
      const s = await api('/api/band/sessions/start', { device_id: st.id, label: label || 'day',
        started_by: src === 'touch' ? 'touch' : 'website', started_at: rec.startedAt });
      rec.sessionId = s.id;
      rec.startedAt = s.started_at;  // an open session that already existed keeps its own start
      set({ sessionsChanged: st.sessionsChanged + 1 });
      return rec;
    })();
    return rec.ready;
  }

  async function stopRecording(r) {
    const rec = recs.get(r);
    if (!rec || rec.stopped) return;
    rec.stopped = true;
    await rec.ready;
    await flush();
    await api(`/api/band/sessions/${rec.sessionId}/stop`, {});
    set({ sessionsChanged: st.sessionsChanged + 1 });
  }

  // One batch at a time; a caller that arrives mid-batch (a stop) waits for it and anything queued
  // since, so a session never closes before its last readings are in.
  let inFlight = null;
  function flush() {
    if (inFlight) return inFlight.then(() => (queue.length ? flush() : undefined));
    if (!queue.length) return Promise.resolve();
    inFlight = sendBatch().finally(() => { inFlight = null; });
    return inFlight;
  }
  async function sendBatch() {
    const batch = queue.splice(0), byRec = new Map();
    for (const m of batch) { if (!byRec.has(m.r)) byRec.set(m.r, []); byRec.get(m.r).push(m); }
    for (const [r, items] of byRec) {
      try {
        const live = lastStatus && lastStatus.r === r && lastStatus.s === 'recording' ? lastStatus : null;
        const maxT = Math.max(0, ...items.flatMap(m => [...(m.motion || []), ...(m.scratch_events || [])].map(x => x.t)));
        const rec = await session(r, live ? live.el : maxT + 1, live ? live.src : 'touch', live ? live.label : 'day');
        const body = { scratch_events: [], motion: [] };
        for (const m of items) {
          // absolute times: the band only knows seconds since its recording started
          for (const e of m.scratch_events || []) body.scratch_events.push({ ts: rec.startedAt + e.t, duration_s: e.duration_s, intensity: e.intensity });
          for (const x of m.motion || []) body.motion.push({ ts: rec.startedAt + x.t, activity: x.activity, scratch_energy: x.scratch_energy });
        }
        const res = await api(`/api/band/sessions/${rec.sessionId}/readings`, body);
        if (res.detail) { console.warn('band: readings refused:', res.detail); continue; }  // e.g. the session was stopped elsewhere
        if (res.alert) await showAlert(res.alert);
        if (body.scratch_events.length) set({ sessionsChanged: st.sessionsChanged + 1 });
      } catch (e) {
        console.warn('band: relaying readings failed, retrying', e);
        queue.unshift(...items);  // keep them for the next flush
      }
    }
  }
  setInterval(flush, FLUSH_MS);

  // ---------- alerts both ways ----------
  const clean = s => String(s || '').replace(/\s*\([^)]*\)/g, '').replace(/[|]/g, '/').trim();
  async function showAlert(a) {
    set({ alert: a });
    const tip = clean((a.tips || [])[0]), reason = clean((a.reasons || [])[0]);
    let msg = `alert:${a.level}|${reason.charAt(0).toUpperCase() + reason.slice(1)}|${tip}`;
    while (enc.encode(msg).length > 180) msg = msg.slice(0, -2) + '…';  // the band reads up to 199 bytes
    await command(msg).catch(e => console.warn('band: alert not delivered', e));
  }
  async function dismissAlert(fromBand) {
    const a = st.alert;
    set({ alert: null });
    if (!fromBand) await command('dismiss').catch(() => {});
    if (a && a.id != null) await api(`/api/band/alerts/${a.id}/dismiss`, {});
  }

  // ---------- notifications ----------
  async function onStatus(s) {
    const prev = lastStatus;
    lastStatus = s;
    if (prev && (s.r < prev.r || s.s === 'boot')) recs = new Map();  // the band restarted
    set({ status: s, id: s.id || st.id });
    if (s.s === 'recording' && s.r) await session(s.r, s.el || 0, s.src, s.label);
    // a recording that has ended: stop it once the band has sent everything it queued
    for (const [r, rec] of recs) if (!rec.stopped && !(s.s === 'recording' && s.r === r) && s.q === 0) await stopRecording(r);
    if (prev && prev.alert && !s.alert && st.alert) await dismissAlert(true);  // double tap on the band
  }
  function parse(e) {
    try { return JSON.parse(dec.decode(e.target.value)); } catch (err) { console.warn('band: bad message', err); return null; }
  }
  // named, so a reconnect can't attach them twice (Chrome may hand back the same characteristic)
  function onStatusEvent(e) { const s = parse(e); if (s) onStatus(s); }
  function onDataEvent(e) { const m = parse(e); if (m && m.r) queue.push(m); }

  // ---------- connection ----------
  async function attach() {
    set({ phase: 'connecting', error: null });
    const server = await device.gatt.connect();
    const svc = await server.getPrimaryService(SVC);
    for (const [k, u] of [['status', STATUS], ['data', DATA], ['cmd', CMD]]) ch[k] = await svc.getCharacteristic(u);
    // The band asks for pairing as soon as we connect, and every characteristic refuses until the
    // user types the code from its screen into the system prompt. Keep trying until it's paired.
    set({ phase: 'pairing' });
    const deadline = Date.now() + PAIR_WAIT_MS;
    let first;
    for (;;) {
      try { first = JSON.parse(dec.decode(await op(() => ch.status.readValue()))); break; }
      catch (e) {
        if (!device.gatt.connected) throw new Error('The band disconnected while pairing. Try again.');
        if (Date.now() > deadline) throw new Error('Pairing timed out. Check the code on the band and try again.');
        await new Promise(r => setTimeout(r, 1500));
      }
    }
    await api('/api/band/devices', { device_id: first.id, name: device.name || 'DermEdge band' });
    ch.status.removeEventListener('characteristicvaluechanged', onStatusEvent);
    ch.data.removeEventListener('characteristicvaluechanged', onDataEvent);
    ch.status.addEventListener('characteristicvaluechanged', onStatusEvent);
    ch.data.addEventListener('characteristicvaluechanged', onDataEvent);
    await op(() => ch.status.startNotifications());
    await op(() => ch.data.startNotifications());
    lastStatus = null;
    set({ phase: 'connected', name: device.name || 'DermEdge', id: first.id });
    await onStatus(first);
  }

  async function onDisconnected() {
    if (userClosed) { set({ phase: 'idle', status: null }); return; }
    // the band went out of range or restarted: try to get it back without asking the user again
    for (let i = 0; i < RECONNECTS && !userClosed; i++) {
      set({ phase: 'reconnecting', error: null });
      await new Promise(r => setTimeout(r, 2000 + i * 1000));
      try { await attach(); return; } catch (e) { /* not back yet */ }
    }
    if (!userClosed) set({ phase: 'idle', status: null, error: 'Lost the band. Bring it closer and connect again.' });
  }

  async function connect() {
    if (!st.supported) throw new Error('This browser has no Web Bluetooth. Use Chrome or Edge on a computer or Android phone.');
    userClosed = false;
    set({ phase: 'choosing', error: null });
    try {
      device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SVC] }], optionalServices: [SVC] });
    } catch (e) {
      set({ phase: 'idle', error: e.name === 'NotFoundError' ? null : e.message });  // NotFoundError: the user closed the chooser
      return;
    }
    device.addEventListener('gattserverdisconnected', onDisconnected);
    try { await attach(); }
    catch (e) {
      userClosed = true;
      if (device.gatt.connected) device.gatt.disconnect();
      set({ phase: 'idle', error: e.message });
    }
  }

  function disconnect() {
    userClosed = true;
    if (device && device.gatt.connected) device.gatt.disconnect();
    else set({ phase: 'idle', status: null });
  }

  function command(text) {
    if (!ch.cmd || !device || !device.gatt.connected) return Promise.reject(new Error('The band is not connected'));
    return op(() => ch.cmd.writeValueWithResponse(enc.encode(text)));
  }

  window.DermBand = {
    state: () => st,
    onChange: f => { listeners.add(f); f(st); return () => listeners.delete(f); },
    connect, disconnect,
    start: label => command(label ? `start:${label}` : 'start'),
    stop: () => command('stop'),
    dismissAlert: () => dismissAlert(false),
  };
})();
