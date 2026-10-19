// DermEdge in the browser: answers the dashboard's /api/* calls on the visitor's own device, so
// the same page that talks to the laptop hub (app/server.py) runs with no server at all.
// Mirrors app/server.py, app/pipeline.py and app/features.py. History lives in IndexedDB,
// settings and the journal in localStorage; nothing is sent anywhere.
(function () {
  const realFetch = window.fetch.bind(window);
  let engine;
  const loadEngine = () => (engine = engine || import('/js/engine.mjs'));

  // ---------- storage ----------
  const DB_NAME = 'dermedge', STORES = ['analyses', 'treatments'];
  let dbp;
  function db() {
    return dbp = dbp || new Promise((res, rej) => {
      const r = indexedDB.open(DB_NAME, 1);
      r.onupgradeneeded = () => STORES.forEach(s => r.result.createObjectStore(s, { keyPath: 'id', autoIncrement: true }));
      r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
    });
  }
  async function tx(store, mode, fn) {
    const d = await db();
    return new Promise((res, rej) => {
      const t = d.transaction(store, mode), s = t.objectStore(store), out = fn(s);
      t.oncomplete = () => res(out && out.result !== undefined ? out.result : out); t.onerror = () => rej(t.error);
    });
  }
  const all = store => tx(store, 'readonly', s => s.getAll());
  const add = (store, v) => tx(store, 'readwrite', s => s.add(v));
  async function removeWhere(store, pred) {
    const items = (await all(store)).filter(pred);
    await tx(store, 'readwrite', s => items.forEach(i => s.delete(i.id)));
    return items.length;
  }
  const ls = {
    get(k, d) { try { const v = localStorage.getItem('dermedge.' + k); return v ? JSON.parse(v) : d; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem('dermedge.' + k, JSON.stringify(v)); } catch (e) { /* private mode: keep in memory */ } },
  };
  let settings = ls.get('settings', { fitzpatrick: null, sites: [], consent: false, onboarded: false });
  let journal = ls.get('journal', {}), journalAt = ls.get('journal_at', null);

  // ---------- features (app/features.py, app/pipeline.py) ----------
  const BODY_SITES = ['face', 'neck', 'inner_elbow', 'hand', 'wrist', 'back_of_knee', 'leg', 'foot', 'torso', 'other'];
  const TREATMENTS = ['moisturiser', 'steroid_cream', 'non_steroid_cream', 'antihistamine', 'wet_wrap', 'other'];
  const LOOK_ALIKES = 'psoriasis, tinea (ringworm), candidiasis, insect bites, lichen planus, drug eruption or rosacea';
  const STRESS_TIP = 'Stress is up. Take a 5-minute breathing break; stress often makes itch worse.';
  const TIPS = {
    'stress (GSR wristband)': STRESS_TIP, 'stress (journal)': STRESS_TIP,
    'dry skin (moisture patch)': 'Your eczema patch is drier than healthy skin. Apply moisturiser now and after bathing.',
    'night scratching (IMU, simulated)': 'Scratching was detected overnight. Keep nails short and consider cotton gloves at night.',
    'poor sleep': 'Short sleep. Aim for a cool bedroom and a regular bedtime tonight.',
    'new soap / product': 'You used a new product. If the skin reacts, stop using it and note it for your doctor.',
  };
  const RETAKE = { blurry: 'The photo is blurry. Hold the camera still, 10-15 cm from the skin.',
                   too_dark: 'The photo is too dark. Move to better light.',
                   too_bright: 'The photo is washed out. Avoid direct light or the flash glare.' };
  const clip = (x, a, b) => Math.min(b, Math.max(a, x));

  function triggerScores(w, j) {
    const s = { ...w };
    if (j.stress != null) s['stress (journal)'] = clip(j.stress / 10, 0, 1) * 0.8;
    if (j.sleep_hours != null) s['poor sleep'] = clip((7 - j.sleep_hours) / 4, 0, 1) * 0.6;
    if (j.new_product) s['new soap / product'] = 0.4;
    return s;
  }
  function flareRisk(imageScore, trig) {
    const vals = Object.values(trig), index = vals.length ? 1 - vals.reduce((p, v) => p * (1 - v), 1) : 0;
    return { trigger_index: +index.toFixed(3), risk: +(imageScore * index).toFixed(3) };
  }
  function alerts(trig, index) {
    const level = index >= 0.6 ? 'high' : index >= 0.3 ? 'moderate' : 'low';
    const ranked = Object.entries(trig).filter(([, v]) => v >= 0.25).sort((a, b) => b[1] - a[1]).map(([k]) => k);
    const tips = [...new Set(ranked.map(k => TIPS[k]).filter(Boolean))].slice(0, 3);
    return { level, index: +index.toFixed(2), triggers: ranked.slice(0, 3), tips };
  }
  function compareWithPrevious(hist, site, r) {
    if (!site || !r.severity) return null;
    const prev = hist.filter(h => h.site === site && h.severity);
    if (!prev.length) return null;
    const p = prev[prev.length - 1], delta = +(r.severity.score - p.severity.score).toFixed(1);
    return { prev_score: p.severity.score, prev_band: p.severity.band, prev_ts: p.ts, prev_image: p.image, delta,
             trend: delta <= -0.5 ? 'better' : delta >= 0.5 ? 'worse' : 'same' };
  }
  function siteSummary(hist) {
    const out = {};
    for (const h of hist) if (h.site && h.severity) {
      const s = out[h.site] = out[h.site] || { site: h.site, first: h.severity.score, first_ts: h.ts, checks: 0 };
      Object.assign(s, { latest: h.severity.score, latest_ts: h.ts, latest_image: h.image }); s.checks++;
    }
    return Object.values(out);
  }
  function insights(hist) {
    const checks = hist.filter(h => h.severity), names = [...new Set(checks.flatMap(h => Object.keys(h.triggers || {})))].sort();
    const mean = a => a.reduce((s, x) => s + x, 0) / a.length, out = [];
    for (const n of names) {
      const on = checks.filter(h => (h.triggers || {})[n] >= 0.5).map(h => h.severity.score);
      const off = checks.filter(h => !((h.triggers || {})[n] >= 0.5)).map(h => h.severity.score);
      if (on.length >= 3 && off.length >= 3 && Math.abs(mean(on) - mean(off)) >= 0.5)
        out.push({ trigger: n, mean_with: +mean(on).toFixed(1), mean_without: +mean(off).toFixed(1), diff: +(mean(on) - mean(off)).toFixed(1),
                   n_with: on.length, n_without: off.length });
    }
    out.sort((a, b) => Math.abs(b.diff) - Math.abs(a.diff));
    return { insights: out, n_checks: checks.length, min_checks: 3, all_sample: checks.length > 0 && checks.every(h => h.sample) };
  }
  function verdict(res, trig) {
    if (!res.skin || res.p_rash < 0.5) return Object.assign(res, { verdict: 'normal', headline: 'No rash detected',
      advice: 'Skin looks normal. Keep logging triggers in your journal.', triggers: trig, flare: flareRisk(0, trig) });
    let imageScore = 0;
    if (res.p_eczema >= 0.5) {
      const band = res.severity.band;
      Object.assign(res, { verdict: 'eczema', headline: `Likely eczema - ${band.toLowerCase()}`,
        advice: band !== 'Severe' ? 'Moisturise, avoid your triggers, and see a doctor if it spreads, oozes or disturbs sleep.'
                                  : 'Looks severe - please book a doctor or dermatologist visit.' });
      imageScore = res.p_eczema * res.severity.score / 12;
    } else {
      Object.assign(res, { verdict: 'other_rash', headline: 'Rash - probably not eczema',
        advice: `It looks more like a look-alike condition (${LOOK_ALIKES}). These need different treatment - please see a doctor.` });
    }
    res.unsure = res.p_eczema >= 0.35 && res.p_eczema <= 0.65;
    if (res.unsure) { res.headline += ' (uncertain)'; res.advice = "The models aren't sure. " + res.advice + ' A dermatologist should confirm.'; }
    return Object.assign(res, { triggers: trig, flare: flareRisk(imageScore, trig) });
  }
  const history = async () => (await all('analyses')).sort((a, b) => a.ts - b.ts);
  async function treatments() {
    const items = (await all('treatments')).sort((a, b) => a.ts - b.ts), cutoff = Date.now() / 1000 - 7 * 86400, counts = {};
    items.filter(t => t.ts >= cutoff).forEach(t => { counts[t.kind] = (counts[t.kind] || 0) + 1; });
    return { treatments: items, summary: { days: 7, counts }, kinds: TREATMENTS };
  }

  // ---------- simulated wearable (synthetic signals through the real stress model) ----------
  const W = () => window.DermWearable;
  let stressModel, wear = fresh(), sim = { scenario: null, timer: null, k: 0 };
  function fresh() { return { calib: [], profile: null, stress: [], series: [], markers: {}, scratches: [], updated: null }; }
  const SCEN = {
    calm: { stressed: false, deficit: 2.2, scratch: 0, label: 'Calm day: relaxed physiology, near-normal skin hydration' },
    stress: { stressed: true, deficit: 2.2, scratch: 0, label: 'Stressful day: raised skin conductance and heart rate' },
    itchy_night: { stressed: false, deficit: 9.5, scratch: 0.6, label: 'Itchy night: calm physiology, dry patch, scratching bursts' },
    flare: { stressed: true, deficit: 9.5, scratch: 0.6, label: 'Flare conditions: stress + dry skin + night scratching' },
  };
  const rnd = (a) => a * (Math.random() * 2 - 1);
  function synthWindow(stressed) {  // 30 s at the E4 rates: EDA/TEMP 4 Hz, BVP 64 Hz, ACC 32 Hz
    const eda = [], temp = [], bvp = [], acc = [];
    let scr = 0, phase = 0;
    const hr = stressed ? 95 : 68, level = stressed ? 1.7 : 0.3, t0 = stressed ? 31.1 : 29.4, jit = stressed ? 6 : 2;
    for (let i = 0; i < 120; i++) {
      if (Math.random() < (stressed ? 0.06 : 0.01)) scr += stressed ? 0.25 : 0.08;
      scr *= 0.93; eda.push(level + (stressed ? i * 0.002 : 0) + scr + rnd(0.01)); temp.push(t0 + rnd(0.02));
    }
    for (let i = 0; i < 1920; i++) {
      phase = (phase + hr / 60 / 64) % 1;
      bvp.push(80 * (Math.exp(-(((phase - 0.15) / 0.06) ** 2)) + 0.35 * Math.exp(-(((phase - 0.45) / 0.08) ** 2))) - 30 + rnd(3));
    }
    for (let i = 0; i < 960; i++) acc.push([rnd(jit), rnd(jit), 64 + rnd(jit)]);
    return { eda, temp, bvp, acc };
  }
  function wearWindow(w) {
    const f = W().windowFeatures(w.eda, w.temp, w.bvp, w.acc), m = wear.markers, mean = a => a.reduce((s, x) => s + x, 0) / a.length;
    Object.assign(m, { skin_conductance: +mean(w.eda).toFixed(2), skin_temp: +mean(w.temp).toFixed(2), heart_rate: Math.round(W().heartRate(w.bvp) || 0) });
    if (!wear.profile) {
      wear.calib.push(f); m.calibration = `${wear.calib.length}/3 calm windows`;
      if (wear.calib.length >= 3) { wear.profile = W().baselineProfile(wear.calib); m.calibration = 'done'; }
    } else if (stressModel) {
      wear.stress.push(W().stressProbability(stressModel, f, wear.profile)); wear.stress = wear.stress.slice(-3);
      m.stress_p = +mean(wear.stress).toFixed(3);
      wear.series.push({ t: Date.now() / 1000, stress: m.stress_p, hr: m.heart_rate }); wear.series = wear.series.slice(-120);
    }
    wear.updated = Date.now() / 1000;
  }
  function wearTriggers() {
    const m = wear.markers, t = {};
    if (m.stress_p != null) t['stress (GSR wristband)'] = clip((m.stress_p - 0.25) / 0.2, 0, 1);
    if (m.hydration_deficit != null) t['dry skin (moisture patch)'] = clip(m.hydration_deficit / 8, 0, 1);
    if (m.night_scratches) t['night scratching (IMU, simulated)'] = clip(m.night_scratches / 30, 0, 1);
    return t;
  }
  async function startSim(name) {
    clearInterval(sim.timer); sim = { scenario: null, timer: null, k: 0 };
    if (!SCEN[name]) return;
    stressModel = stressModel || await realFetch('/models/stress_model.json?v=2').then(r => r.json());
    wear = fresh(); sim.scenario = name;
    const cfg = SCEN[name];
    const tick = () => {
      wearWindow(synthWindow(sim.k >= 3 && cfg.stressed));  // the first 3 windows calibrate, so they're calm
      wear.markers.hydration_deficit = +(cfg.deficit + rnd(0.4)).toFixed(1);
      if (cfg.scratch && Math.random() < cfg.scratch) { wear.markers.night_scratches = (wear.markers.night_scratches || 0) + 1 + Math.floor(Math.random() * 3); }
      sim.k++;
    };
    tick(); sim.timer = setInterval(tick, 2000);
  }
  function state() {
    const trig = triggerScores(wearTriggers(), journal), index = flareRisk(1, trig).trigger_index;
    return { wearable: { markers: { ...wear.markers }, updated: wear.updated, series: wear.series }, journal, journal_at: journalAt,
             sim: { scenario: sim.scenario, available: true, missing: null, label: sim.scenario ? SCEN[sim.scenario].label : null,
                    source: sim.scenario ? 'Simulated wristband and moisture patch, scored by the WESAD stress model' : null },
             triggers: trig, trigger_index: index, alerts: alerts(trig, index) };
  }

  // ---------- sample history (synthetic check-ins, clearly labelled) ----------
  function sampleImage(seed, score) {
    const c = document.createElement('canvas'); c.width = c.height = 256; const g = c.getContext('2d');
    let s = seed * 9301 + 49297; const r = () => ((s = (s * 9301 + 49297) % 233280) / 233280);
    const grd = g.createLinearGradient(0, 0, 256, 256); grd.addColorStop(0, '#f1c7b3'); grd.addColorStop(1, '#e2a98f');
    g.fillStyle = grd; g.fillRect(0, 0, 256, 256);
    for (let i = 0; i < 18 + score * 10; i++) {
      g.fillStyle = `rgba(${190 + r() * 40},${60 + r() * 40},${70 + r() * 30},${0.12 + 0.05 * score * r()})`;
      g.beginPath(); g.ellipse(60 + r() * 136, 60 + r() * 136, 6 + r() * 26, 4 + r() * 18, r() * 3, 0, 7); g.fill();
    }
    g.fillStyle = 'rgba(58,31,43,.65)'; g.font = '600 18px Figtree, sans-serif'; g.fillText('sample', 12, 240);
    return c.toDataURL('image/jpeg', 0.85);
  }
  async function seed() {
    await clear();
    const now = Date.now() / 1000, sites = ['inner_elbow', 'hand', 'back_of_knee'];
    for (let i = 0; i < 12; i++) {
      const score = +clip(6.6 - i * 0.5 + rnd(0.5), 0.5, 12).toFixed(1), stressed = i < 6;
      const trig = { 'stress (journal)': +(stressed ? 0.5 + Math.random() * 0.3 : Math.random() * 0.3).toFixed(2),
                     'poor sleep': +(stressed ? 0.5 + Math.random() * 0.4 : Math.random() * 0.3).toFixed(2),
                     'dry skin (moisture patch)': +(i % 3 ? 0.4 + Math.random() * 0.6 : Math.random() * 0.3).toFixed(2) };
      const band = score < 2.5 ? 'Mild' : score < 4.5 ? 'Moderate' : 'Severe', p = +(0.8 + Math.random() * 0.17).toFixed(3);
      const ts = now - (28 - Math.round(i * 28 / 12)) * 86400 + (8 + Math.random() * 12) * 3600;
      const res = verdict({ p_rash: 0.95, skin: true, p_eczema: p, severity: { score, band }, calibrated_for: null }, trig);
      const img = sampleImage(i + 1, score);
      await add('analyses', Object.assign(res, { ts, source: 'sample', site: sites[i % 3], sample: true, image: img, heatmap: img }));
      if (i % 2 === 0) await add('treatments', { ts: ts + 3600, kind: ['moisturiser', 'steroid_cream', 'moisturiser', 'antihistamine', 'moisturiser', 'wet_wrap'][i / 2],
                                                 notes: '', site: sites[i % 3], sample: true });
    }
    return { added: 12, treatments: 6 };
  }
  async function clear() {
    const removed = await removeWhere('analyses', a => a.sample);
    await removeWhere('treatments', t => t.sample);
    return { removed };
  }

  // ---------- the doctor report (opened as its own printable page) ----------
  async function openReport() {
    const win = window.open('', '_blank');
    const items = (await history()).filter(h => h.verdict !== 'no_skin').reverse(), tr = (await treatments()).treatments.slice().reverse();
    const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
    const when = ts => new Date(ts * 1000).toLocaleString([], { day: '2-digit', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
    const ins = insights(items);
    const rows = items.map(h => `<tr><td>${when(h.ts)}${h.sample ? ' <i>(sample)</i>' : ''}</td><td>${esc((h.site || '-').replace(/_/g, ' '))}</td>
      <td><img src="${h.image}">${h.heatmap && h.heatmap !== h.image ? `<img src="${h.heatmap}">` : ''}</td><td>${esc(h.headline)}</td>
      <td>${h.severity ? `${h.severity.band} (${h.severity.score}/12)` : '-'}</td><td>${h.flare ? h.flare.risk.toFixed(2) : '-'}</td></tr>`).join('');
    const trRows = tr.map(t => `<tr><td>${when(t.ts)}${t.sample ? ' <i>(sample)</i>' : ''}</td><td>${esc(t.kind.replace(/_/g, ' '))}</td>
      <td>${esc((t.site || '-').replace(/_/g, ' '))}</td><td>${esc(t.notes)}</td></tr>`).join('');
    const insHtml = ins.insights.length ? '<ul>' + ins.insights.map(i => `<li>${esc(i.trigger)}: average severity ${i.mean_with} when present vs ${i.mean_without} when absent (${i.n_with} vs ${i.n_without} checks)</li>`).join('') + '</ul>'
      : '<p class="muted">Not enough check-ins yet to suggest triggers.</p>';
    win.document.write(`<!doctype html><html><head><meta charset="utf-8"><title>DermEdge report</title><link rel="stylesheet" href="${location.origin}/static/fonts/fonts.css">
<style>body{font-family:Figtree,-apple-system,Segoe UI,sans-serif;margin:32px;color:#3a1f2b;background:#fff}h1,h2{font-family:"Young Serif",Georgia,serif;font-weight:400;color:#8e2a57}
.muted{color:#8b6474}table{border-collapse:collapse;width:100%;margin-top:16px}th{color:#8b6474;font-weight:600}td,th{border-bottom:1px solid #f4d4df;padding:8px;text-align:left;vertical-align:top;font-size:14px}
img{height:90px;border-radius:10px;margin-right:6px}.note{margin-top:24px;font-size:12px;color:#8b6474}</style></head><body>
<h1>DermEdge &middot; skin history report</h1><div class="muted">Generated ${when(Date.now() / 1000)} - ${items.length} checks</div>
<table><tr><th>When</th><th>Body site</th><th>Photo / heatmap</th><th>Result</th><th>Severity</th><th>Flare risk</th></tr>${rows || '<tr><td colspan=6>No checks yet.</td></tr>'}</table>
<h2 style="margin-top:28px">Treatments</h2><table><tr><th>When</th><th>Treatment</th><th>Body site</th><th>Notes</th></tr>${trRows || '<tr><td colspan=4>None logged.</td></tr>'}</table>
<h2 style="margin-top:28px">Possible triggers in this log</h2>${insHtml}
<p class="note">Screening aid, not a diagnosis. Severity is scored 0-12 on four EASI signs (erythema, papulation, excoriation, lichenification) from one photo, on the patient's own device.</p>
<script>window.onload=()=>setTimeout(()=>window.print(),500)<\/script></body></html>`);
    win.document.close();
  }

  // ---------- model loading banner ----------
  function banner(msg, frac) {
    let b = document.getElementById('modelBanner');
    if (!b) {
      b = document.createElement('div'); b.id = 'modelBanner';
      b.style.cssText = 'position:fixed;left:50%;bottom:max(16px,env(safe-area-inset-bottom,0px));transform:translateX(-50%);z-index:30;' +
        'background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:999px;padding:10px 18px;font-size:14px;' +
        'box-shadow:var(--shadow);max-width:calc(100vw - 32px);text-align:center';
      document.body.appendChild(b);
    }
    b.textContent = msg; b.hidden = false;
    if (frac >= 1) setTimeout(() => { b.hidden = true; }, 2500);
  }

  // ---------- the router ----------
  const json = (v, status = 200) => new Response(JSON.stringify(v), { status, headers: { 'Content-Type': 'application/json' } });
  async function route(url, opts) {
    const u = new URL(url, location.origin), p = u.pathname, body = opts && opts.body;
    const readJSON = () => JSON.parse(body || '{}');
    if (p === '/api/screen') {
      const e = await loadEngine(); await e.init(banner);
      const blob = body.get('image'), bmp = await e.bitmap(blob);
      return json(Object.assign(await e.screen(blob, settings.fitzpatrick), { quality: e.quality(bmp) }));
    }
    if (p === '/api/analyze') {
      const e = await loadEngine(); await e.init(banner);
      const blob = body.get('image'), site = BODY_SITES.includes(u.searchParams.get('site')) ? u.searchParams.get('site') : null;
      const bmp0 = await e.bitmap(blob), q = e.quality(bmp0);
      if (!q.ok && u.searchParams.get('force') !== 'true')
        return json({ verdict: 'retake', headline: 'Please retake the photo', quality: q, advice: q.issues.map(i => RETAKE[i]).join(' ') });
      const { res, bmp, cam } = await e.analyze(blob, settings.fitzpatrick);
      const r = verdict(res, triggerScores(wearTriggers(), journal));
      r.site = site; r.quality = q; r.compare = compareWithPrevious(await history(), site, r);
      if (r.triggers) r.alerts = alerts(r.triggers, r.flare.trigger_index);
      r.image = e.toDataURL(bmp, 512); r.heatmap = cam ? e.toDataURL(bmp, 512, cam) : null;
      r.ts = Date.now() / 1000; r.source = u.searchParams.get('source') || 'webcam';
      r.id = await add('analyses', r);
      return json(r);
    }
    if (p === '/api/history') return json(await history());
    if (p === '/api/sites') return json({ sites: siteSummary(await history()), all_sites: BODY_SITES });
    if (p === '/api/insights') return json(insights(await history()));
    if (p === '/api/treatments') return json(await treatments());
    if (p === '/api/treatment') {
      const t = readJSON();
      await add('treatments', { ts: Date.now() / 1000, kind: TREATMENTS.includes(t.kind) ? t.kind : 'other', notes: String(t.notes || '').slice(0, 200),
                                site: BODY_SITES.includes(t.site) ? t.site : null, sample: false });
      return json(await treatments());
    }
    if (p === '/api/settings') {
      if (opts && opts.method === 'POST') {
        const n = readJSON();
        if ('fitzpatrick' in n) settings.fitzpatrick = [1, 2, 3, 4, 5, 6].includes(n.fitzpatrick) ? n.fitzpatrick : null;
        if (Array.isArray(n.sites)) settings.sites = n.sites.filter(s => BODY_SITES.includes(s));
        if (n.consent != null) { settings.consent = !!n.consent; settings.consent_at = n.consent ? Date.now() / 1000 : null; }
        if (n.onboarded != null) settings.onboarded = !!n.onboarded;
        ls.set('settings', settings);
      }
      return json(settings);
    }
    if (p === '/api/journal') { journal = readJSON(); journalAt = Date.now() / 1000; ls.set('journal', journal); ls.set('journal_at', journalAt); return json(state()); }
    if (p === '/api/state') return json(state());
    if (p === '/api/sim') { const s = readJSON().scenario; await startSim(s); return json(state()); }
    if (p === '/api/fairness') {
      const h = await realFetch('/models/heads.json?v=2').then(r => r.json());
      return json({ before: h.fairness.before, after: h.fairness.after, offsets: h.rash.fst_offsets });
    }
    if (p === '/api/demo/seed') return json(await seed());
    if (p === '/api/demo/clear') return json(await clear());
    return json({ error: 'not found' }, 404);
  }

  window.fetch = (input, opts) => {
    const url = typeof input === 'string' ? input : input.url;
    if (new URL(url, location.origin).pathname.startsWith('/api/')) return route(url, opts).catch(e => {
      console.error(e); banner('Something went wrong running the models: ' + e.message, 1); return json({ error: String(e) }, 500);
    });
    return realFetch(input, opts);
  };
  window.DermLocal = { openReport, preload: () => loadEngine().then(e => e.init(banner)) };
})();
