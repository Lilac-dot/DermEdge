// DermEdge in the browser: answers the dashboard's /api/* calls on the visitor's own device, so
// the same page that talks to the laptop hub (app/server.py) runs with no server at all.
// Mirrors app/server.py, app/pipeline.py and app/features.py. History lives in IndexedDB,
// settings and the journal in localStorage; nothing is sent anywhere.
(function () {
  const realFetch = window.fetch.bind(window);
  let engine;
  const loadEngine = () => (engine = engine || import('/js/engine.mjs'));

  // ---------- storage ----------
  // v2 adds the DermEdge band's paired devices, recording sessions, readings and alerts; v3 indexes
  // readings by (session, kind), since 4 Hz skin conductance adds ~14,000 readings an hour
  const DB_NAME = 'dermedge', STORES = ['analyses', 'treatments', 'band_devices', 'band_sessions', 'band_readings', 'band_alerts'];
  let dbp;
  function db() {
    return dbp = dbp || new Promise((res, rej) => {
      const r = indexedDB.open(DB_NAME, 3);
      r.onupgradeneeded = () => {
        STORES.forEach(s => { if (!r.result.objectStoreNames.contains(s)) r.result.createObjectStore(s, { keyPath: 'id', autoIncrement: true }); });
        const readings = r.transaction.objectStore('band_readings');
        if (!readings.indexNames.contains('session_kind')) readings.createIndex('session_kind', ['session_id', 'kind']);
      };
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
  const put = (store, v) => tx(store, 'readwrite', s => s.put(v));
  const getOne = (store, id) => tx(store, 'readonly', s => s.get(id));
  async function removeWhere(store, pred) {
    const items = (await all(store)).filter(pred);
    await tx(store, 'readwrite', s => items.forEach(i => s.delete(i.id)));
    return items.length;
  }
  const ls = {
    get(k, d) { try { const v = localStorage.getItem('dermedge.' + k); return v ? JSON.parse(v) : d; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem('dermedge.' + k, JSON.stringify(v)); } catch (e) { /* private mode: keep in memory */ } },
  };
  const DEFAULT_SETTINGS = { fitzpatrick: null, sites: [], consent: false, onboarded: false };
  let settings = ls.get('settings', { ...DEFAULT_SETTINGS });
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
    'scratching (DermEdge band)': 'Your band picked up scratching. Keep nails short, and try a cool compress or moisturiser on the itchy spot instead.',
    'stress (band GSR)': STRESS_TIP,
    'dry skin (band moisture patch)': 'Your band reads the eczema patch drier than healthy skin. Apply moisturiser now and after bathing.',
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
    if (m.band_scratches) t['scratching (DermEdge band)'] = clip(m.band_scratches / 30, 0, 1);  // same scale as the hub
    // the band's own sensors count while they're current. Stress starts at the model's threshold
    // (0.5): a calm wearer scores ~0.45 right after calibration and shouldn't count as a trigger
    const lm = live.markers;
    if (liveCurrent() && lm.stress_p != null) t['stress (band GSR)'] = clip((lm.stress_p - 0.5) / 0.3, 0, 1);
    if (liveCurrent() && lm.ecg_stress_p != null) t['stress (band ECG)'] = clip((lm.ecg_stress_p - 0.5) / 0.3, 0, 1);
    if (liveCurrent() && lm.hydration_deficit != null) t['dry skin (band moisture patch)'] = clip(lm.hydration_deficit / 8, 0, 1);
    return t;
  }

  // ---------- the band's own sensors: skin conductance -> stress, skin temperature, moisture ----------
  // Skin conductance is scored with web/models/stress_gsr.json (scripts/train_gsr_stress.py):
  // logistic regression on six statistics of each 30 s window, relative to the wearer's first 3
  // windows (90 s sitting calm) of the recording. Tested on WESAD one held-out person at a time:
  // 89.6% accuracy (77% by always saying calm), 73% of stress windows caught, poor for 4 of 15
  // people. Skin temperature is shown but not scored: in that test it drifted with time on the
  // wrist and made stress detection worse.
  const GSR_WIN = 30, GSR_MIN_READINGS = 20, LIVE_STALE_S = 600;
  let gsrModel = null, live = freshLive(null);
  function freshLive(sessionId) { return { sessionId, buf: [], winStart: null, calib: [], profile: null, stress: [], markers: {}, series: [], updated: null }; }
  const liveFor = sessionId => (live.sessionId === sessionId ? live : (live = freshLive(sessionId)));
  const liveCurrent = () => live.updated != null && Date.now() / 1000 - live.updated < LIVE_STALE_S;
  const avg = a => a.reduce((s, x) => s + x, 0) / a.length;

  function gsrFeatures(pts) {  // pts: [{ts, v}], the readings of one window (scripts/wesad_features.py, EDA)
    const v = pts.map(p => p.v), m = avg(v), mn = Math.min(...v), mx = Math.max(...v), tm = avg(pts.map(p => p.ts));
    let num = 0, den = 0;
    for (const p of pts) { num += (p.ts - tm) * (p.v - m); den += (p.ts - tm) ** 2; }
    return { EDA_mean: m, EDA_std: Math.sqrt(avg(v.map(x => (x - m) ** 2))), EDA_min: mn, EDA_max: mx, EDA_range: mx - mn,
             EDA_slope: (den ? num / den : 0) / 4 };  // per second -> per sample at the E4's 4 Hz, as trained
  }
  function gsrStress(f, profile, model) {  // also scores ECG features with stress_ecg.json
    const g = model || gsrModel;
    const z = g.features.reduce((s, name, i) => {
      const k = name.replace(/_rel$/, '');
      return s + g.coef[i] * ((f[k] - profile[k]) - g.mean[i]) / g.scale[i];
    }, g.intercept);
    return 1 / (1 + Math.exp(-z));
  }
  function closeGsrWindow(L) {
    const end = L.winStart + GSR_WIN, pts = L.buf.filter(p => p.ts < end), m = L.markers;
    L.buf = L.buf.filter(p => p.ts >= end);
    L.winStart = end;
    if (pts.length < GSR_MIN_READINGS) return;  // a gap (out of range, electrodes off): not scored
    const f = gsrFeatures(pts);
    if (!L.profile) {
      L.calib.push(f);
      m.calibration = `${L.calib.length}/3 calm windows`;
      if (L.calib.length >= 3) {
        L.profile = Object.fromEntries(Object.keys(f).map(k => [k, avg(L.calib.map(c => c[k]))]));
        m.calibration = 'done';
      }
    } else {
      L.stress = L.stress.concat(gsrStress(f, L.profile)).slice(-3);  // smoothed over ~90 s, as the hub does
      m.stress_p = +avg(L.stress).toFixed(3);
    }
    L.series = L.series.concat({ t: end, gsr: +f.EDA_mean.toFixed(3), stress: m.stress_p ?? null, temp: m.skin_temp ?? null }).slice(-240);
  }
  // ECG: the band sends one reading per 30 s of electrode contact with the hand still (heart rate,
  // RMSSD, SDNN). The first 3 of a recording are the wearer's calm level; later ones are shown
  // against it. Stress is scored only once a model exists: web/models/stress_ecg.json, in
  // stress_gsr.json's format (features named <feature>_rel, relative to the calm level, from
  // HR_mean, RMSSD, SDNN and TEMP_mean). Until then, heart rate and HRV are shown, not scored.
  let ecgModel;  // undefined: not looked for yet; null: there isn't one
  function hrvFeatures(x, temp) { return { HR_mean: x.hr, RMSSD: x.rmssd, SDNN: x.sdnn, TEMP_mean: temp ?? NaN }; }
  async function liveHrv(L, hrv) {
    const m = L.markers;
    if (ecgModel === undefined) ecgModel = await realFetch('/models/stress_ecg.json').then(r => (r.ok ? r.json() : null)).catch(() => null);
    L.hrvCalib = L.hrvCalib || [];
    L.hrvStress = L.hrvStress || [];
    for (const x of hrv) {
      const f = hrvFeatures(x, m.skin_temp);
      if (!L.hrvProfile) {
        L.hrvCalib.push(f);
        m.hrv_calibration = `${L.hrvCalib.length}/3 calm readings`;
        if (L.hrvCalib.length >= 3) {
          L.hrvProfile = Object.fromEntries(Object.keys(f).map(k => [k, avg(L.hrvCalib.map(c => c[k]))]));
          m.hrv_calibration = 'done';
        }
      } else if (ecgModel && ecgModel.features.every(n => Number.isFinite(f[n.replace(/_rel$/, '')]) && Number.isFinite(L.hrvProfile[n.replace(/_rel$/, '')]))) {
        L.hrvStress = L.hrvStress.concat(gsrStress(f, L.hrvProfile, ecgModel)).slice(-3);
        m.ecg_stress_p = +avg(L.hrvStress).toFixed(3);
      }
      L.series = L.series.concat({ t: x.ts, hr: x.hr, rmssd: x.rmssd, stress: m.ecg_stress_p ?? null, temp: m.skin_temp ?? null }).slice(-240);
    }
    const last = hrv[hrv.length - 1];
    Object.assign(m, { heart_rate: Math.round(last.hr), rmssd: Math.round(last.rmssd), sdnn: Math.round(last.sdnn),
                       ecg_model: !!ecgModel });
    if (L.hrvProfile) {
      m.hr_vs_calm = Math.round(last.hr - L.hrvProfile.HR_mean);
      m.rmssd_vs_calm = Math.round(last.rmssd - L.hrvProfile.RMSSD);
    }
  }
  async function liveSensors(sessionId, gsr, temps, moist, hrv = []) {  // each sorted by ts
    const L = liveFor(sessionId), m = L.markers;
    if (gsr.length) {
      gsrModel = gsrModel || await realFetch('/models/stress_gsr.json?v=4').then(r => r.json());
      for (const r of gsr) {
        if (L.winStart == null) L.winStart = r.ts;
        while (r.ts >= L.winStart + GSR_WIN) {
          closeGsrWindow(L);
          if (!L.buf.length && r.ts >= L.winStart + GSR_WIN) L.winStart = r.ts;  // after a long gap, restart here
        }
        L.buf.push({ ts: r.ts, v: r.microsiemens });
      }
      m.skin_conductance = +avg(gsr.map(r => r.microsiemens)).toFixed(2);
      if (!m.calibration) m.calibration = '0/3 calm windows';
    }
    if (temps.length) m.skin_temp = +avg(temps.map(x => x.c)).toFixed(2);
    if (hrv.length) await liveHrv(L, hrv);
    const ref = moist.filter(x => x.reference > 0);
    if (ref.length) m.hydration_deficit = +((1 - avg(ref.map(x => x.patch)) / avg(ref.map(x => x.reference))) * 100).toFixed(1);
    L.updated = Date.now() / 1000;
  }
  async function startSim(name) {
    clearInterval(sim.timer); sim = { scenario: null, timer: null, k: 0 };
    if (!SCEN[name]) return;
    stressModel = stressModel || await realFetch('/models/stress_model.json?v=4').then(r => r.json());
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
             band_sensors: { markers: { ...live.markers }, updated: live.updated, current: liveCurrent(), session_id: live.sessionId,
                             series: live.series },
             triggers: trig, trigger_index: index, alerts: alerts(trig, index) };
  }

  // ---------- the DermEdge band (mirrors app/band.py; no login: nothing leaves this browser) ----------
  const BAND_ALERT_COOLDOWN = 30 * 60;  // seconds between flare alerts
  const SCRATCH_WINDOW = 8 * 3600;      // scratch episodes count towards the trigger for 8 h
  function bandScratch(ts) {
    const cutoff = Date.now() / 1000 - SCRATCH_WINDOW;
    wear.scratches = wear.scratches.filter(t => t >= cutoff).concat(ts >= cutoff ? [ts] : []);
    wear.markers.band_scratches = wear.scratches.length;
    wear.updated = Date.now() / 1000;
  }
  const ofKind = (id, kind) => IDBKeyRange.only([id, kind]);
  async function bandSession(s, withReadings) {
    const out = { ...s, scratch_count: await tx('band_readings', 'readonly', st => st.index('session_kind').count(ofKind(s.id, 'scratch'))),
                  duration_s: +(((s.stopped_at || Date.now() / 1000) - s.started_at)).toFixed(1) };
    if (withReadings) {
      const kind = async k => (await tx('band_readings', 'readonly', st => st.index('session_kind').getAll(ofKind(s.id, k)))).sort((a, b) => a.ts - b.ts);
      out.scratch_events = (await kind('scratch')).map(r => ({ ts: r.ts, duration_s: r.duration_s, intensity: r.intensity }));
      out.motion_summary = (await kind('motion')).map(r => ({ ts: r.ts, activity: r.activity, scratch_energy: r.scratch_energy }));
      out.hrv = (await kind('hrv')).map(r => ({ ts: r.ts, hr: r.hr, rmssd: r.rmssd, sdnn: r.sdnn, beats: r.beats }));
      out.gsr = (await kind('gsr')).map(r => ({ ts: r.ts, microsiemens: r.microsiemens }));
      out.skin_temp = (await kind('skin_temp')).map(r => ({ ts: r.ts, c: r.c }));
      out.moisture = (await kind('moisture')).map(r => ({ ts: r.ts, patch: r.patch, reference: r.reference }));
    }
    return out;
  }
  async function bandAlertCheck(sessionId) {
    const st = state(), a = st.alerts;
    if (a.level !== 'high') return null;
    const now = Date.now() / 1000, last = Math.max(0, ...(await all('band_alerts')).map(x => x.ts));
    if (now - last < BAND_ALERT_COOLDOWN) return null;
    const alert = { session_id: sessionId, ts: now, level: a.level, risk: a.index, reasons: a.triggers, tips: a.tips, dismissed_at: null };
    alert.id = await add('band_alerts', alert);
    return alert;
  }
  async function bandRoute(p, method, body) {
    const now = Date.now() / 1000;
    let m;
    if (p === '/api/band/devices' && method === 'POST') {
      const id = String(body.device_id || '').slice(0, 64), name = String(body.name || 'DermEdge band').slice(0, 40);
      if (id.length < 4) return json({ detail: 'device_id is required' }, 422);
      const known = (await all('band_devices')).find(d => d.device_id === id);
      const dev = Object.assign(known || { device_id: id, paired_at: now }, { name, last_seen: now });
      dev.id = await put('band_devices', dev);
      return json(dev);
    }
    if (p === '/api/band/devices') return json(await all('band_devices'));
    if (p === '/api/band/sessions/start' && method === 'POST') {
      const dev = (await all('band_devices')).find(d => d.device_id === body.device_id);
      if (!dev) return json({ detail: 'Pair this band first' }, 404);
      // starting while one is open returns that one, so a band press and a click don't make two
      const open = (await all('band_sessions')).find(s => s.device === dev.id && s.stopped_at == null);
      if (open) return json(await bandSession(open));
      const s = { device: dev.id, started_at: Number(body.started_at) > 0 ? Math.min(body.started_at, now) : now, stopped_at: null,
                  label: String(body.label || 'day').slice(0, 40), started_by: body.started_by === 'touch' ? 'touch' : 'website',
                  notes: String(body.notes || '').slice(0, 200) };
      s.id = await add('band_sessions', s);
      return json(await bandSession(s));
    }
    if ((m = p.match(/^\/api\/band\/sessions\/(\d+)\/(stop|readings)$/)) && method === 'POST') {
      const s = await getOne('band_sessions', +m[1]);
      if (!s) return json({ detail: 'No such session' }, 404);
      if (m[2] === 'stop') {
        if (s.stopped_at == null) { s.stopped_at = now; await put('band_sessions', s); }
        return json(await bandSession(s));
      }
      if (s.stopped_at != null) return json({ detail: 'This session has stopped' }, 409);
      const when = x => Math.min(Math.max(x.ts != null ? x.ts : x.t != null ? s.started_at + x.t : now, s.started_at), now);
      const scratches = (body.scratch_events || []).filter(e => e.duration_s >= 0 && e.intensity >= 0);
      const motion = (body.motion || []).filter(x => x.activity >= 0 && x.scratch_energy >= 0);
      // the hub's limits (app/band.py): conductance 0-100 uS, skin temperature 10-50 C; heart rate
      // 25-220 bpm, RMSSD and SDNN 0-500 ms
      const byTs = (a, b) => a.ts - b.ts;
      const hrv = (body.hrv || []).filter(x => x.hr >= 25 && x.hr <= 220 && x.rmssd >= 0 && x.rmssd <= 500 && x.sdnn >= 0 && x.sdnn <= 500)
        .map(x => ({ ts: when(x), hr: +x.hr, rmssd: +x.rmssd, sdnn: +x.sdnn, beats: +x.beats || 0 })).sort(byTs);
      const gsr = (body.gsr || []).filter(x => x.microsiemens >= 0 && x.microsiemens <= 100).map(x => ({ ts: when(x), microsiemens: +x.microsiemens })).sort(byTs);
      const temps = (body.skin_temp || []).filter(x => x.c >= 10 && x.c <= 50).map(x => ({ ts: when(x), c: +x.c })).sort(byTs);
      const moist = (body.moisture || []).filter(x => Number.isFinite(x.patch)).map(x => ({ ts: when(x), patch: +x.patch,
        reference: Number.isFinite(x.reference) ? +x.reference : null })).sort(byTs);
      await tx('band_readings', 'readwrite', st => {
        scratches.forEach(e => st.add({ session_id: s.id, kind: 'scratch', ts: when(e), duration_s: +e.duration_s, intensity: +e.intensity }));
        motion.forEach(x => st.add({ session_id: s.id, kind: 'motion', ts: when(x), activity: +x.activity, scratch_energy: +x.scratch_energy }));
        hrv.forEach(x => st.add({ session_id: s.id, kind: 'hrv', ...x }));
        gsr.forEach(x => st.add({ session_id: s.id, kind: 'gsr', ...x }));
        temps.forEach(x => st.add({ session_id: s.id, kind: 'skin_temp', ...x }));
        moist.forEach(x => st.add({ session_id: s.id, kind: 'moisture', ...x }));
      });
      scratches.forEach(e => bandScratch(when(e)));
      if (hrv.length || gsr.length || temps.length || moist.length) await liveSensors(s.id, gsr, temps, moist, hrv);
      return json({ stored: { scratch_events: scratches.length, motion: motion.length, hrv: hrv.length, gsr: gsr.length, skin_temp: temps.length, moisture: moist.length },
                    alert: await bandAlertCheck(s.id) });
    }
    if ((m = p.match(/^\/api\/band\/sessions\/(\d+)$/))) {
      const s = await getOne('band_sessions', +m[1]);
      return s ? json(await bandSession(s, true)) : json({ detail: 'No such session' }, 404);
    }
    if (p === '/api/band/sessions') {
      const list = (await all('band_sessions')).sort((a, b) => b.started_at - a.started_at);
      return json(await Promise.all(list.map(s => bandSession(s))));
    }
    if ((m = p.match(/^\/api\/band\/alerts\/(\d+)\/dismiss$/)) && method === 'POST') {
      const a = await getOne('band_alerts', +m[1]);
      if (!a) return json({ detail: 'No such alert' }, 404);
      if (a.dismissed_at == null) { a.dismissed_at = now; await put('band_alerts', a); }
      return json({ ok: true });
    }
    if (p === '/api/band/alerts') return json((await all('band_alerts')).sort((a, b) => b.ts - a.ts));
    return json({ detail: 'not found' }, 404);
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
  // "Delete all my data": every check, treatment, journal entry and setting on this device. The
  // downloaded models stay cached; they hold nothing about the user.
  async function wipe() {
    const removed = { checks: (await all('analyses')).length, treatments: (await all('treatments')).length,
                      recordings: (await all('band_sessions')).length };
    await Promise.all(STORES.map(st => tx(st, 'readwrite', s => s.clear())));
    ['settings', 'journal', 'journal_at'].forEach(k => { try { localStorage.removeItem('dermedge.' + k); } catch (e) { /* nothing stored */ } });
    settings = { ...DEFAULT_SETTINGS }; journal = {}; journalAt = null;
    await startSim(null); wear = fresh(); live = freshLive(null);
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
<style>body{font-family:Figtree,-apple-system,Segoe UI,sans-serif;margin:32px;color:#1d2b2a;background:#fff}h1,h2{font-family:"Young Serif",Georgia,serif;font-weight:400;color:#134e4a}
.muted{color:#5c6f6c}table{border-collapse:collapse;width:100%;margin-top:16px}th{color:#5c6f6c;font-weight:600}td,th{border-bottom:1px solid #d9e6e3;padding:8px;text-align:left;vertical-align:top;font-size:14px}
img{height:90px;border-radius:10px;margin-right:6px}.note{margin-top:24px;font-size:12px;color:#5c6f6c}</style></head><body>
<h1>DermEdge &middot; skin history report</h1><div class="muted">Generated ${when(Date.now() / 1000)} - ${items.length} checks</div>
<table><tr><th>When</th><th>Body site</th><th>Photo / heatmap</th><th>Result</th><th>Severity</th><th>Flare risk</th></tr>${rows || '<tr><td colspan=6>No checks yet.</td></tr>'}</table>
<h2 style="margin-top:28px">Treatments</h2><table><tr><th>When</th><th>Treatment</th><th>Body site</th><th>Notes</th></tr>${trRows || '<tr><td colspan=4>None logged.</td></tr>'}</table>
<h2 style="margin-top:28px">Possible triggers in this log</h2>${insHtml}
<p class="note">Screening aid, not a diagnosis. Severity is scored 0-12 on four EASI signs (erythema, papulation, excoriation, lichenification) from one photo, on the patient's own device.</p>
<script>window.onload=()=>setTimeout(()=>window.print(),500)<\/script></body></html>`);
    win.document.close();
  }

  // ---------- model loading banner ----------
  let bannerTimer;
  function banner(msg, frac, hideAfter = 2500) {
    let b = document.getElementById('modelBanner');
    if (!b) {
      b = document.createElement('div'); b.id = 'modelBanner';
      b.style.cssText = 'position:fixed;left:50%;bottom:max(16px,env(safe-area-inset-bottom,0px));transform:translateX(-50%);z-index:30;' +
        'background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:999px;padding:10px 18px;font-size:14px;' +
        'box-shadow:var(--shadow);max-width:calc(100vw - 32px);text-align:center';
      document.body.appendChild(b);
    }
    b.textContent = msg; b.hidden = false;
    clearTimeout(bannerTimer);
    if (frac >= 1) bannerTimer = setTimeout(() => { b.hidden = true; }, hideAfter);
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
      const h = await realFetch('/models/heads.json?v=4').then(r => r.json());
      return json({ before: h.fairness.before, after: h.fairness.after, offsets: h.rash.fst_offsets });
    }
    if (p === '/api/demo/seed') return json(await seed());
    if (p === '/api/demo/clear') return json(await clear());
    if (p === '/api/data/delete') return json(await wipe());
    if (p.startsWith('/api/band/')) return bandRoute(p, (opts && opts.method) || 'GET', body ? readJSON() : {});
    return json({ error: 'not found' }, 404);
  }

  window.fetch = (input, opts) => {
    const url = typeof input === 'string' ? input : input.url;
    if (new URL(url, location.origin).pathname.startsWith('/api/')) return route(url, opts).catch(e => {
      console.error(e); banner('Something went wrong: ' + e.message, 1, 12000); return json({ error: String(e) }, 500);
    });
    return realFetch(input, opts);
  };
  window.DermLocal = { openReport, preload: () => loadEngine().then(e => e.init(banner)) };
})();
