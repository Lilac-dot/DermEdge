// Wearable maths for the browser version: the WESAD window features (scripts/wesad_features.py),
// per-wearer baseline calibration (scripts/wesad_calibration.py) and an evaluator for the
// exported LightGBM stress model (web/models/stress_model.json). Plain script so the page and
// Node tests can both load it.
(function (root) {
  const FS = { EDA: 4, TEMP: 4, BVP: 64, ACC: 32 };

  const mean = a => a.reduce((s, x) => s + x, 0) / a.length;
  const std = a => { const m = mean(a); return Math.sqrt(mean(a.map(x => (x - m) ** 2))); };

  function slope(a) {  // least-squares line through (i, a[i]), as numpy.polyfit(t, x, 1)[0]
    const n = a.length; if (n < 2) return 0;
    const tm = (n - 1) / 2, am = mean(a);
    let num = 0, den = 0;
    for (let i = 0; i < n; i++) { num += (i - tm) * (a[i] - am); den += (i - tm) ** 2; }
    return num / den;
  }

  function stats(prefix, a, out) {
    let mn = Infinity, mx = -Infinity;
    for (const x of a) { if (x < mn) mn = x; if (x > mx) mx = x; }
    out[prefix + '_mean'] = mean(a); out[prefix + '_std'] = std(a);
    out[prefix + '_min'] = mn; out[prefix + '_max'] = mx; out[prefix + '_range'] = mx - mn;
  }

  function windowFeatures(eda, temp, bvp, acc) {
    const f = {};
    for (const [name, a] of [['EDA', eda], ['TEMP', temp]]) {
      stats(name, a, f); f[name + '_slope'] = slope(a);
    }
    stats('BVP', bvp, f); delete f.BVP_range;
    // dominant frequency in the cardiac band (42-210 bpm) of |rfft(bvp - mean)|
    const n = bvp.length, m = mean(bvp), x = bvp.map(v => v - m);
    let best = -1, bestHz = 0, power = 0;
    for (let k = 0; k <= Math.floor(n / 2); k++) {
      const hz = k * FS.BVP / n;
      if (hz < 0.7 || hz > 3.5) continue;
      let re = 0, im = 0;
      for (let t = 0; t < n; t++) { const ang = -2 * Math.PI * k * t / n; re += x[t] * Math.cos(ang); im += x[t] * Math.sin(ang); }
      const mag = Math.hypot(re, im);
      power += mag; if (mag > best) { best = mag; bestHz = hz; }
    }
    f.BVP_dominant_hz = best >= 0 && power > 0 ? bestHz : 0;
    f.BVP_band_power = power;
    const mag = acc.map(r => Math.hypot(r[0], r[1], r[2]));
    f.ACC_mag_mean = mean(mag); f.ACC_mag_std = std(mag);
    ['x', 'y', 'z'].forEach((ax, i) => { const c = acc.map(r => r[i]); f[`ACC_${ax}_mean`] = mean(c); f[`ACC_${ax}_std`] = std(c); });
    return f;
  }

  function baselineProfile(featList) {
    const p = {};
    for (const k of Object.keys(featList[0])) p[k] = mean(featList.map(f => f[k]));
    return p;
  }

  function predictTree(node, x) {
    while (node.v === undefined) {
      const v = x[node.f];
      node = (Number.isNaN(v) ? node.dl : v <= node.t) ? node.l : node.r;
    }
    return node.v;
  }

  function stressProbability(model, feats, profile) {
    const all = { ...feats };
    for (const [k, v] of Object.entries(feats)) all[k + '_rel'] = v - profile[k];
    const x = model.features.map(n => all[n]);
    const z = model.trees.reduce((s, t) => s + predictTree(t, x), 0);
    return 1 / (1 + Math.exp(-z));
  }

  // beats per minute from the autocorrelation peak of the PPG within 40-200 bpm
  function heartRate(bvp, fs = FS.BVP) {
    const m = mean(bvp), x = bvp.map(v => v - m);
    let best = -Infinity, lag = 0;
    for (let L = Math.round(fs * 60 / 200); L <= Math.round(fs * 60 / 40); L++) {
      let s = 0; for (let i = 0; i + L < x.length; i++) s += x[i] * x[i + L];
      if (s > best) { best = s; lag = L; }
    }
    return lag ? 60 * fs / lag : null;
  }

  root.DermWearable = { FS, windowFeatures, baselineProfile, stressProbability, heartRate };
})(typeof window !== 'undefined' ? window : globalThis);
