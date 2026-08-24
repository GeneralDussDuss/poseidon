/* POSEIDON site script. Null-guarded so subpages can share it
 * even when they don't have hero / particles / caustic canvases. */

/* Shared environment flags (mobile perf gating — added 2026-06-18).
 * Desktop keeps full-power effects; phones throttle/disable heavy loops. */
var POSEIDON_MOBILE = matchMedia('(max-width:768px)').matches;
var POSEIDON_REDUCED = matchMedia('(prefers-reduced-motion: reduce)').matches;

// Decorative canvases are presentation-only — hide from assistive tech (a11y, 2026-06-17).
addEventListener('DOMContentLoaded',function(){document.querySelectorAll('canvas').forEach(function(c){c.setAttribute('aria-hidden','true');});});


// ===== Cinematic per-section boot sequence =====
// Replaces the static splash: each page boots with its own ASCII emblem and a
// dramatic streaming boot log, CRT-styled. Data-driven so it covers every page.
(function bootSequence() {
  const bo = document.getElementById('bootOverlay');
  if (!bo) return;
  const hud = document.getElementById('hud');
  const page = ((location.pathname.split('/').pop() || 'index').replace('.html', '')) || 'index';

  if (!document.getElementById('dbootStyle')) {
    const st = document.createElement('style'); st.id = 'dbootStyle';
    st.textContent =
      '#bootOverlay .dboot{max-width:720px;width:92vw;font-family:var(--font-mono,ui-monospace,monospace);text-align:left}' +
      '#bootOverlay .dboot-art{white-space:pre;color:var(--bacc,#38bdf8);font-size:clamp(8px,1.7vw,13px);line-height:1.15;' +
      'text-shadow:0 0 10px var(--bacc,#38bdf8);margin:0 0 .4rem;opacity:0;animation:dbootArt .5s ease-out forwards}' +
      '#bootOverlay .dboot-name{font-family:var(--font-display,sans-serif);font-weight:800;letter-spacing:.18em;' +
      'font-size:clamp(1.4rem,5vw,2.6rem);color:var(--bacc,#38bdf8);text-shadow:0 0 18px var(--bacc,#38bdf8);margin:.1rem 0 .7rem}' +
      '#bootOverlay .dboot-log{font-size:clamp(9px,1.5vw,12.5px);line-height:1.5;color:#8fa6c8;min-height:11em}' +
      '#bootOverlay .dboot-log div{opacity:0;transform:translateX(-6px);transition:opacity .14s,transform .14s;white-space:pre}' +
      '#bootOverlay .dboot-log div.on{opacity:1;transform:none}' +
      '#bootOverlay .dboot-log .k{color:var(--bacc,#38bdf8)} #bootOverlay .dboot-log .ok{color:#5effc8}' +
      '#bootOverlay .dboot-log .warn{color:#fbbf24}' +
      '#bootOverlay .dboot-bar{height:4px;margin-top:.6rem;background:rgba(255,255,255,.06);border-radius:99px;overflow:hidden}' +
      '#bootOverlay .dboot-bar i{display:block;height:100%;width:0;background:var(--bacc,#38bdf8);box-shadow:0 0 10px var(--bacc,#38bdf8);transition:width .25s}' +
      '#bootOverlay .dboot-cur{display:inline-block;width:.6em;height:1em;background:var(--bacc,#38bdf8);vertical-align:-2px;animation:dbootCur 1s steps(2) infinite;box-shadow:0 0 8px var(--bacc,#38bdf8)}' +
      '#bootOverlay::after{content:"";position:absolute;inset:0;pointer-events:none;z-index:2;' +
      'background:repeating-linear-gradient(to bottom,rgba(0,0,0,0) 0 2px,rgba(0,0,0,.5) 2px 3px);opacity:.4}' +
      '@keyframes dbootArt{from{opacity:0;filter:brightness(3)}to{opacity:1;filter:none}}' +
      '@keyframes dbootCur{50%{opacity:0}}';
    document.head.appendChild(st);
  }

  const A = '#38bdf8';
  const SEQ = {
    index:    { acc:'#38bdf8', name:'POSEIDON', tag:'COMMANDER OF THE DEEP',
      art:['        ╲     │     ╱','         ╲   ‖‖‖   ╱','      ◣════╗ ‖‖‖ ╔════◢','          ╚══╪╪╪══╝','             ‖‖‖','       ≈≈≈≈≈≈≈≈≈≈≈≈≈≈≈'],
      log:['mounting six-band RF core','wifi · ble · sub-ghz · 2.4 · lora · ir','cc1101 + nrf24 + sx1262 linked','TRIDENT C5 satellite handshake','80+ modules registered'] },
    arsenal:  { acc:'#f0abfc', name:'ARSENAL', tag:'EVERY TOOL, ARMED',
      art:['   ┌────┬────┬────┬────┐','   │ ⚷  │ ▚▚ │ ◎  │ ☢  │','   ├────┼────┼────┼────┤','   │ ▟▙ │ ⌁  │ ⟿  │ ▒▒ │','   └────┴────┴────┴────┘'],
      log:['indexing module registry','arming wifi / ble / sub-ghz suites','loading 80+ attack + recon tools','safety gates: DISARMED by default'] },
    codex:    { acc:'#38bdf8', name:'CODEX', tag:'DECRYPT THE ARSENAL',
      art:['    ╔═══════════════╗','    ║ 0x504F5345 ▒▒ ║','    ║ decrypting... ▚ ║','    ╚═══════════════╝'],
      log:['loading radio index','decrypting tool manifests','9 radios · per-chip arsenals','console standing by'] },
    install:  { acc:'#34d399', name:'INSTALL', tag:'BRING IT TO LIFE',
      art:['     ┌─[ ESP32 ]─┐','     │ ▓▓▓▓▓▓▓▓ │ ◂ flash','     └─────┬─────┘','        ▟ USB-C ▙'],
      log:['probing flash subsystem','M5Burner + WebSerial bridge up','TRIDENT C5 no-stub flasher ready','select your device'] },
    hardware: { acc:'#7dd3fc', name:'HARDWARE', tag:'KNOW YOUR GEAR',
      art:['   ┌───────────────────┐','   │ ▢ LCD    ⌨ KBD   │','   │ ▟ RF ▟  ▟ HATS ▟ │','   └──┬─────────────┬──┘','      ╿             ╿'],
      log:['probing i2c bus','ST7789v2 LCD + TCA8418 keyboard','sub-ghz / 2.4 / lora / gnss hats','pinmap resolved'] },
    tembed:   { acc:'#22d3ee', name:'T-EMBED', tag:'ONE BOARD, EVERY RADIO',
      art:['   ┌───────────────────┐','   │ ◉ enc   ▢ 320x170 │','   │ cc1101 nrf24 pn532│','   └──────┬─────┬──────┘','          ⌁     ≈'],
      log:['power gate high','ST7789 320x170 landscape','cc1101 + nrf24 + pn532 nfc + ir','encoder + 8px led ring','board target: tembed'] },
    roadmap:  { acc:'#fbbf24', name:'ROADMAP', tag:'THE DEEP AHEAD',
      art:['   ●───●───●───◐───○───○','   v0.4 v0.5 v0.6 now next','        ▸ charting course'],
      log:['loading release timeline','shipped vs in-flight','plotting next milestones'] },
    testers:  { acc:'#a78bfa', name:'TESTERS', tag:'FIRST IN THE WATER',
      art:['      (( • ))   (( • ))','     ◢█◣  ◢█◣  ◢█◣','     field crew online'],
      log:['pinging tester roster','field reports synced','thank-you protocol loaded'] },
    credits:  { acc:'#f472b6', name:'CREDITS', tag:'STANDING ON GIANTS',
      art:['        ✦   ✦   ✦','       ╲  ☼  ╱','        ╲ │ ╱','     ── built by ──'],
      log:['loading contributors','open-source dependencies','respect + attribution'] },
    legal:    { acc:'#f59e0b', name:'LEGAL', tag:'AUTHORIZED USE ONLY',
      art:['          ╔═╗','        ╔═╝▟▙╚═╗','        ║ ⚖  ║','        ╚══════╝'],
      log:['loading compliance banner','regulated bands · operate on your own gear','consent required for all targets'] },
  };
  const d = SEQ[page] || SEQ.index;
  bo.style.setProperty('--bacc', d.acc);

  const lines = [
    { t: 'POSEIDON OS · cold boot', c: 'k' },
    { t: 'core esp32-s3 @ 240MHz ............ [ OK ]', c: 'ok' },
    { t: '8MB PSRAM mapped ................. [ OK ]', c: 'ok' },
    ...d.log.map(s => ({ t: '> ' + s + ' ', c: '' })),
    { t: d.name + ' online. ready_', c: 'k', cur: true },
  ];

  const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const artHtml = d.art.map(esc).join('\n');
  const logHtml = lines.map(l =>
    `<div class="${l.c}">${esc(l.t)}${l.cur ? '<span class="dboot-cur"></span>' : ''}</div>`).join('');
  bo.innerHTML =
    `<div class="dboot"><pre class="dboot-art">${artHtml}</pre>` +
    `<div class="dboot-name">${esc(d.name)}</div>` +
    `<div class="dboot-log">${logHtml}</div>` +
    `<div class="dboot-bar"><i></i></div></div>`;

  const REDUCED = POSEIDON_REDUCED;
  const logEls = [...bo.querySelectorAll('.dboot-log div')];
  const bar = bo.querySelector('.dboot-bar i');
  const showHud = () => { if (hud) setTimeout(() => hud.classList.add('visible'), 350); };

  if (REDUCED) {
    logEls.forEach(e => e.classList.add('on')); if (bar) bar.style.width = '100%';
    setTimeout(() => { bo.classList.add('hidden'); showHud(); }, 600);
    return;
  }
  let i = 0;
  const step = () => {
    if (i < logEls.length) {
      logEls[i].classList.add('on');
      if (bar) bar.style.width = Math.round(((i + 1) / logEls.length) * 100) + '%';
      i++;
      setTimeout(step, 95 + Math.random() * 70);
    } else {
      setTimeout(() => { bo.classList.add('hidden'); showHud(); }, 650);
    }
  };
  setTimeout(step, 380);
})();

// Scroll progress + nav
const bar = document.getElementById('scrollProgress');
const mainNav = document.getElementById('mainNav');
window.addEventListener('scroll', () => {
  if (bar) {
    const pct = window.scrollY / (document.documentElement.scrollHeight - window.innerHeight) * 100;
    bar.style.width = pct + '%';
  }
  if (mainNav) mainNav.classList.toggle('scrolled', window.scrollY > 50);
}, { passive: true });

// Mobile nav (hamburger). No-op on desktop: the injected button is CSS-hidden
// above 768px and we only wire toggling behaviour at small viewports. Added 2026-06-18.
(function mobileNav() {
  const nav = document.getElementById('mainNav');
  if (!nav) return;
  const links = nav.querySelector('.nav-links');
  if (!links) return;

  // Inject an accessible hamburger button (hidden by CSS on desktop).
  const btn = document.createElement('button');
  btn.className = 'nav-toggle';
  btn.type = 'button';
  btn.setAttribute('aria-label', 'Toggle navigation menu');
  btn.setAttribute('aria-expanded', 'false');
  btn.setAttribute('aria-controls', 'navLinks');
  btn.innerHTML = '<span class="nt-bars" aria-hidden="true"></span>';
  if (!links.id) links.id = 'navLinks';
  nav.appendChild(btn);

  const mq = matchMedia('(max-width:768px)');
  const isMobile = () => mq.matches;

  function close() {
    links.classList.remove('open');
    btn.setAttribute('aria-expanded', 'false');
  }
  function open() {
    links.classList.add('open');
    btn.setAttribute('aria-expanded', 'true');
  }
  function toggle() {
    if (links.classList.contains('open')) close(); else open();
  }

  // Button only acts as a toggle on small screens. On desktop it's display:none,
  // so clicks/keys can't reach it — but guard anyway to stay a strict no-op.
  btn.addEventListener('click', () => { if (isMobile()) toggle(); });

  // Close on link tap, Escape, or outside tap — only while in mobile mode.
  links.addEventListener('click', (e) => {
    if (isMobile() && e.target.closest('a')) close();
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && links.classList.contains('open')) { close(); btn.focus(); }
  });
  document.addEventListener('click', (e) => {
    if (!links.classList.contains('open')) return;
    if (!nav.contains(e.target)) close();
  });

  // If the viewport grows past the breakpoint, drop any open state so desktop
  // nav is untouched.
  const onChange = () => { if (!isMobile()) close(); };
  if (mq.addEventListener) mq.addEventListener('change', onChange);
  else if (mq.addListener) mq.addListener(onChange);
})();

// Scroll reveal
const obs = new IntersectionObserver((entries) => {
  entries.forEach(e => {
    if (e.isIntersecting) {
      e.target.classList.add('visible');
      obs.unobserve(e.target);
    }
  });
}, { threshold: 0.1 });
document.querySelectorAll('.reveal,.reveal-left').forEach(el => obs.observe(el));

// Stat number counters
const counterObs = new IntersectionObserver((entries) => {
  entries.forEach(e => {
    if (!e.isIntersecting) return;
    const el = e.target;
    const target = parseInt(el.dataset.count, 10);
    const suffix = el.dataset.suffix || '';
    const dur = 1400;
    const start = performance.now();
    const tick = (t) => {
      const p = Math.min(1, (t - start) / dur);
      const eased = 1 - Math.pow(1 - p, 3);
      el.textContent = Math.round(target * eased) + (p >= 1 ? suffix : '');
      if (p < 1) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    counterObs.unobserve(el);
  });
}, { threshold: 0.6 });
document.querySelectorAll('[data-count]').forEach(el => counterObs.observe(el));

// Floating particles (ocean depth) — optional on any page
(function particles() {
  const pc = document.getElementById('particles');
  if (!pc) return;
  // Mobile perf: reduced-motion users get nothing; phones get far fewer
  // particles and a capped frame rate so the page stays smooth + battery-friendly.
  if (POSEIDON_REDUCED) return;
  const px = pc.getContext('2d');
  function resize() { pc.width = window.innerWidth; pc.height = window.innerHeight; }
  resize();
  window.addEventListener('resize', resize);
  const COUNT = POSEIDON_MOBILE ? 18 : 50;
  const MIN_DT = POSEIDON_MOBILE ? 1000 / 30 : 0; // cap ~30fps on phones
  const pts = Array.from({ length: COUNT }, () => ({
    x: Math.random() * pc.width, y: Math.random() * pc.height,
    vx: (Math.random() - 0.5) * 0.3, vy: (Math.random() - 0.5) * 0.3,
    r: Math.random() * 2 + 0.8, hue: Math.random() > 0.6 ? 190 : 205
  }));
  let last = 0;
  (function draw(t) {
    requestAnimationFrame(draw);
    if (t - last < MIN_DT) return;
    last = t || 0;
    px.clearRect(0, 0, pc.width, pc.height);
    pts.forEach(p => {
      p.x += p.vx + Math.sin(Date.now() * 0.001 + p.y * 0.01) * 0.2;
      p.y += p.vy;
      if (p.x < 0) p.x = pc.width;
      if (p.x > pc.width) p.x = 0;
      if (p.y < 0) p.y = pc.height;
      if (p.y > pc.height) p.y = 0;
      px.beginPath(); px.arc(p.x, p.y, p.r, 0, Math.PI * 2);
      px.fillStyle = `hsla(${p.hue},80%,60%,.45)`; px.fill();
      px.beginPath(); px.arc(p.x, p.y, p.r * 3, 0, Math.PI * 2);
      px.fillStyle = `hsla(${p.hue},80%,60%,.06)`; px.fill();
    });
  })(0);
})();

// Hero constellation — home page only
(function heroConstellation() {
  const hc = document.getElementById('heroCanvas');
  if (!hc) return;
  // Mobile perf: skip the O(n²) constellation link-up entirely for reduced-motion;
  // phones get fewer nodes (cheaper inner loop) and a capped frame rate.
  if (POSEIDON_REDUCED) return;
  const hx = hc.getContext('2d');
  function resize() { hc.width = hc.parentElement.clientWidth; hc.height = hc.parentElement.clientHeight; }
  resize();
  window.addEventListener('resize', resize);
  const NODE_COUNT = POSEIDON_MOBILE ? 12 : 24;
  const MIN_DT = POSEIDON_MOBILE ? 1000 / 30 : 0;
  const nodes = Array.from({ length: NODE_COUNT }, () => ({
    x: Math.random() * 1400, y: Math.random() * 900,
    vx: (Math.random() - 0.5) * 0.35, vy: (Math.random() - 0.5) * 0.35
  }));
  let last = 0;
  (function draw(t) {
    requestAnimationFrame(draw);
    if (t - last < MIN_DT) return;
    last = t || 0;
    hx.clearRect(0, 0, hc.width, hc.height);
    const cx = hc.width / 2, cy = hc.height / 2;
    nodes.forEach(n => {
      n.x += n.vx; n.y += n.vy;
      if (n.x < 0 || n.x > hc.width) n.vx *= -1;
      if (n.y < 0 || n.y > hc.height) n.vy *= -1;
    });
    for (let i = 0; i < nodes.length; i++) {
      for (let j = i + 1; j < nodes.length; j++) {
        const dx = nodes[i].x - nodes[j].x, dy = nodes[i].y - nodes[j].y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        if (dist < 220) {
          hx.beginPath(); hx.moveTo(nodes[i].x, nodes[i].y); hx.lineTo(nodes[j].x, nodes[j].y);
          hx.strokeStyle = `rgba(14,165,233,${0.2 * (1 - dist / 220)})`; hx.lineWidth = 0.6; hx.stroke();
        }
      }
    }
    nodes.forEach(n => {
      hx.beginPath(); hx.arc(n.x, n.y, 2, 0, Math.PI * 2);
      hx.fillStyle = 'rgba(14,165,233,.6)'; hx.fill();
      hx.beginPath(); hx.arc(n.x, n.y, 6, 0, Math.PI * 2);
      hx.fillStyle = 'rgba(14,165,233,.08)'; hx.fill();
    });
    const now = Date.now() * 0.001;
    for (let w = 0; w < 2; w++) {
      const waveR = ((now + w * 1.3) % 4) * 180;
      hx.beginPath(); hx.arc(cx, cy, waveR, 0, Math.PI * 2);
      hx.strokeStyle = `rgba(14,165,233,${0.18 * (1 - waveR / 720)})`; hx.lineWidth = 1; hx.stroke();
    }
  })(0);
})();

// Caustic water shader — home page only
(function caustic() {
  const cc = document.getElementById('causticLayer');
  if (!cc) return;
  // Mobile perf: the caustic shader is a per-pixel ImageData loop every frame —
  // by far the heaviest effect. Skip it outright on phones and reduced-motion.
  if (POSEIDON_MOBILE || POSEIDON_REDUCED) { cc.style.display = 'none'; return; }
  const cx2 = cc.getContext('2d');
  function resize() { cc.width = cc.parentElement.clientWidth; cc.height = cc.parentElement.clientHeight; }
  resize();
  window.addEventListener('resize', resize);
  (function draw() {
    const t = Date.now() * 0.0006;
    const w = cc.width, h = cc.height;
    const img = cx2.createImageData(Math.floor(w / 4), Math.floor(h / 4));
    const iw = img.width, ih = img.height;
    for (let y = 0; y < ih; y++) {
      for (let x = 0; x < iw; x++) {
        const u = x / iw * 6, v = y / ih * 4;
        const n = Math.sin(u + t) + Math.sin(v * 1.3 + t * 1.2) + Math.sin((u + v) * 0.8 + t * 0.7);
        const c = Math.pow(Math.max(0, n + 1) / 4, 3);
        const i = (y * iw + x) * 4;
        img.data[i] = 14 * c; img.data[i + 1] = 165 * c; img.data[i + 2] = 233 * c;
        img.data[i + 3] = Math.min(255, c * 140);
      }
    }
    cx2.putImageData(img, 0, 0);
    cx2.imageSmoothingEnabled = true;
    cx2.globalCompositeOperation = 'copy';
    cx2.drawImage(cc, 0, 0, iw, ih, 0, 0, w, h);
    cx2.globalCompositeOperation = 'source-over';
    requestAnimationFrame(draw);
  })();
})();

// Click ripple — all pages
(function clickRipple() {
  document.addEventListener('click', e => {
    const r = document.createElement('div');
    r.style.cssText = `position:fixed;left:${e.clientX}px;top:${e.clientY}px;width:0;height:0;border-radius:50%;background:radial-gradient(circle,rgba(14,165,233,.4),transparent);transform:translate(-50%,-50%);pointer-events:none;z-index:9999;animation:ripple .7s ease-out forwards`;
    document.body.appendChild(r);
    setTimeout(() => r.remove(), 700);
  });
  const rippleStyle = document.createElement('style');
  rippleStyle.textContent = '@keyframes ripple{to{width:280px;height:280px;opacity:0}}';
  document.head.appendChild(rippleStyle);
})();

// Hero parallax — home page only
(function heroParallax() {
  const heroContent = document.querySelector('.hero-content');
  if (!heroContent) return;
  document.addEventListener('mousemove', e => {
    if (window.scrollY > 400) return;
    const cx = window.innerWidth / 2, cy = window.innerHeight / 2;
    const dx = (e.clientX - cx) / cx * 6, dy = (e.clientY - cy) / cy * 6;
    heroContent.style.transform = `translate(${dx}px,${dy}px)`;
  });
})();

/* ─────────────────────────────────────────
 *  PREEM PASS — cyber/tech polish
 * ───────────────────────────────────────── */

// Matrix rain layer. Injects a <canvas id="matrixRain"> and runs it if the page
// has a <body data-matrix>. Safe on every page.
(function matrixRain() {
  if (document.body.dataset.matrix === undefined) return;
  // Mobile perf: reduced-motion users skip the always-on rain; phones use a
  // wider column step (fewer columns) and a capped frame rate.
  if (POSEIDON_REDUCED) return;
  const STEP = POSEIDON_MOBILE ? 22 : 14;     // px between columns (fewer cols on phones)
  const MIN_DT = POSEIDON_MOBILE ? 1000 / 24 : 0;
  const c = document.createElement('canvas');
  c.id = 'matrixRain';
  document.body.prepend(c);
  const x = c.getContext('2d');
  let cols, drops;
  const glyphs = 'アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロ0123456789<>/*[]{}|=+#$%▸◂┃━╱╲'.split('');
  function resize() {
    c.width = window.innerWidth;
    c.height = window.innerHeight;
    cols = Math.floor(c.width / STEP);
    drops = Array.from({ length: cols }, () => Math.random() * c.height / STEP);
  }
  resize();
  window.addEventListener('resize', resize);
  x.font = '12px "JetBrains Mono", monospace';
  let last = 0;
  (function draw(t) {
    requestAnimationFrame(draw);
    if (t - last < MIN_DT) return;
    last = t || 0;
    x.fillStyle = 'rgba(4,8,18,0.08)';
    x.fillRect(0, 0, c.width, c.height);
    for (let i = 0; i < cols; i++) {
      const g = glyphs[Math.floor(Math.random() * glyphs.length)];
      const py = drops[i] * STEP;
      // Random leading character: brighter
      if (Math.random() > 0.97) x.fillStyle = 'rgba(125,211,252,.9)';
      else x.fillStyle = 'rgba(14,165,233,' + (0.25 + Math.random() * 0.35) + ')';
      x.fillText(g, i * STEP, py);
      drops[i] = py > c.height && Math.random() > 0.975 ? 0 : drops[i] + 1;
    }
  })(0);
})();

// CRT sweep element
(function crtSweep() {
  if (document.getElementById('crtSweep')) return;
  const el = document.createElement('div');
  el.id = 'crtSweep';
  document.body.appendChild(el);
})();

// Cursor spotlight
(function spotlight() {
  if (matchMedia('(hover:none)').matches) return;
  const sp = document.createElement('div');
  sp.id = 'spotlight';
  document.body.appendChild(sp);
  document.body.classList.add('has-cursor');
  let tx = 0, ty = 0, cx = 0, cy = 0;
  document.addEventListener('mousemove', e => { tx = e.clientX; ty = e.clientY; });
  (function tick() {
    cx += (tx - cx) * 0.08;
    cy += (ty - cy) * 0.08;
    sp.style.left = cx + 'px';
    sp.style.top = cy + 'px';
    requestAnimationFrame(tick);
  })();
})();

// Glitch attribute — stamp data-text on every .hero-title for the ::before/::after
(function glitchPrime() {
  document.querySelectorAll('.hero-title,.page-head h1').forEach(h => {
    if (!h.dataset.text) h.dataset.text = h.textContent.trim();
  });
})();

// Frame corners helper — stamp 2 extra divs in any .frame so all four corners render
(function frames() {
  document.querySelectorAll('.frame').forEach(f => {
    if (!f.querySelector('.cnr-bl')) {
      const bl = document.createElement('span');
      const br = document.createElement('span');
      bl.className = 'cnr-bl';
      br.className = 'cnr-br';
      f.appendChild(bl);
      f.appendChild(br);
    }
  });
})();

/* ===== Site-wide ambience: CRT scanline overlay + synthesized audio bed =====
 * One global engine for the whole suite — a low ambient hum, soft UI click
 * ticks, and a boot sweep on first entry. All audio is generated live (Web
 * Audio, no asset files) and only starts inside the first user gesture, so
 * autoplay policy is satisfied. One persisted mute (shared with the codex
 * boot FX via window.PoseidonSound + a 'poseidon-mute' event). Honors
 * prefers-reduced-motion: no audio, no scanline motion. */
(function ambience() {
  // --- inject the global scanline overlay + toggle styles once ---
  if (!document.getElementById('fxScanStyle')) {
    const st = document.createElement('style');
    st.id = 'fxScanStyle';
    st.textContent =
      '#fxScan{position:fixed;inset:0;z-index:9000;pointer-events:none;opacity:.05;' +
      'background:repeating-linear-gradient(to bottom,rgba(0,0,0,0) 0 2px,rgba(0,0,0,.55) 2px 3px);' +
      'mix-blend-mode:overlay;animation:fxFlick 5s ease-in-out infinite}' +
      '@keyframes fxFlick{0%,100%{opacity:.05}50%{opacity:.035}}' +
      '#fxToggle{position:fixed;left:14px;bottom:14px;z-index:9100;cursor:pointer;' +
      'font-family:var(--font-mono,ui-monospace,monospace);font-size:.64rem;letter-spacing:.12em;' +
      'background:rgba(4,8,16,.72);border:1px solid rgba(120,200,255,.5);color:#bfe9ff;' +
      'border-radius:8px;padding:.34rem .56rem;backdrop-filter:blur(4px);opacity:.66;transition:opacity .2s}' +
      '#fxToggle:hover{opacity:1}' +
      '@media(prefers-reduced-motion:reduce){#fxScan{display:none}}';
    document.head.appendChild(st);
  }

  const MUTE_KEY = 'poseidonMuted';
  let muted = localStorage.getItem(MUTE_KEY) === '1';
  let actx = null, hum = null, started = false;
  let bus = null, busWet = null;   // premium master chain + synthesized reverb send

  function ctx() {
    if (POSEIDON_REDUCED) return null;
    if (!actx) {
      try { actx = new (window.AudioContext || window.webkitAudioContext)(); } catch { return null; }
      // Premium master bus: every voice -> master gain -> warm lowpass -> out,
      // with a parallel short synthesized reverb for a sense of space (Tron-ish
      // depth without mud). Built once, lazily, inside the first gesture.
      const a = actx;
      const master = a.createGain(); master.gain.value = 0.9;
      const lp = a.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 5400; lp.Q.value = 0.4;
      master.connect(lp).connect(a.destination);
      const rev = a.createConvolver();
      const len = (a.sampleRate * 1.0) | 0, ir = a.createBuffer(2, len, a.sampleRate);
      for (let c = 0; c < 2; c++) { const dch = ir.getChannelData(c);
        for (let i = 0; i < len; i++) dch[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / len, 3.4); }
      rev.buffer = ir;
      const revOut = a.createGain(); revOut.gain.value = 0.5;
      rev.connect(revOut).connect(lp);
      bus = master; busWet = { rev };
    }
    if (actx.state === 'suspended') actx.resume();
    return actx;
  }
  // A voice: oscillator -> envelope -> master (dry); reverb>0 also sends to the
  // convolver for space. Sweeps glide f -> to over the duration (swoops/chirps).
  function blip({ f = 440, type = 'sine', d = .05, g = .03, to = null, delay = 0, reverb = 0 }) {
    const a = ctx(); if (!a || muted || !bus) return;
    const o = a.createOscillator(), gn = a.createGain(), t0 = a.currentTime + delay;
    o.type = type; o.frequency.setValueAtTime(f, t0);
    if (to) o.frequency.exponentialRampToValueAtTime(Math.max(1, to), t0 + d);
    gn.gain.setValueAtTime(.0001, t0);
    gn.gain.linearRampToValueAtTime(g, t0 + .008);
    gn.gain.exponentialRampToValueAtTime(.0001, t0 + d);
    o.connect(gn); gn.connect(bus);
    if (reverb && busWet) { const sd = a.createGain(); sd.gain.value = reverb; gn.connect(sd); sd.connect(busWet.rev); }
    o.start(t0); o.stop(t0 + d + .05);
  }
  function noise(d = .1, g = .04, reverb = 0) {
    const a = ctx(); if (!a || muted || !bus) return;
    const n = (a.sampleRate * d) | 0, b = a.createBuffer(1, n, a.sampleRate), ch = b.getChannelData(0);
    for (let i = 0; i < n; i++) ch[i] = (Math.random() * 2 - 1) * (1 - i / n);
    const s = a.createBufferSource(); s.buffer = b;
    const gn = a.createGain(); gn.gain.value = g; s.connect(gn); gn.connect(bus);
    if (reverb && busWet) { const sd = a.createGain(); sd.gain.value = reverb; gn.connect(sd); sd.connect(busWet.rev); }
    s.start();
  }
  // Deep CRT background hum that *modulates* — detuned low sines through a
  // resonant lowpass whose level AND cutoff slowly breathe via LFOs, plus a
  // faint pitch shimmer. Subtle, deep, alive — the bed under everything.
  function startHum() {
    const a = ctx(); if (!a || muted || hum) return;
    const out = a.createGain();
    out.gain.setValueAtTime(.0001, a.currentTime);
    out.gain.linearRampToValueAtTime(1, a.currentTime + 2.0);
    const lp = a.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 220; lp.Q.value = 6;
    out.connect(lp).connect(bus);
    const tones = [[50, .018], [50.4, .013], [100, .006], [60, .004]].map(([f, g]) => {
      const o = a.createOscillator(), gn = a.createGain();
      o.type = 'sine'; o.frequency.value = f; gn.gain.value = g;
      o.connect(gn).connect(out); o.start(); return o;
    });
    const lfo = (freq, depth, target) => {
      const o = a.createOscillator(), d = a.createGain();
      o.type = 'sine'; o.frequency.value = freq; d.gain.value = depth;
      o.connect(d); d.connect(target); o.start(); return o;
    };
    const mods = [
      lfo(.13, .28, out.gain),            // level breathes  ±0.28 (~7.7 s)
      lfo(.075, 90, lp.frequency),        // filter opens/closes ±90 Hz (~13 s)
      lfo(.09, .8, tones[1].frequency),   // faint pitch shimmer ±0.8 Hz
    ];
    hum = { osc: [...tones, ...mods], out };
  }
  function stopHum() {
    if (!hum) return; const a = ctx();
    try {
      hum.out.gain.cancelScheduledValues(a.currentTime);
      hum.out.gain.setValueAtTime(Math.max(.0001, hum.out.gain.value), a.currentTime);
      hum.out.gain.linearRampToValueAtTime(.0001, a.currentTime + .6);
      hum.osc.forEach(o => { try { o.stop(a.currentTime + .65); } catch {} });
    } catch {}
    hum = null;
  }
  // Subtle UI chirp — a quick clean up-glide, barely there.
  const clickTick = () => blip({ f: 900 + Math.random() * 220, type: 'sine', d: .045, g: .018, to: 2100 });
  // Page power-on swoop — sub thud, a long rising sine swoop, a soft confirm
  // chirp in reverb space. The boot of every page.
  const loadSweep = () => {
    blip({ f: 120,  type: 'sine', d: .30, g: .055, to: 38 });
    blip({ f: 150,  type: 'sine', d: .55, g: .028, to: 900,  delay: .05, reverb: .25 });
    blip({ f: 1500, type: 'sine', d: .12, g: .018, to: 2300, delay: .46, reverb: .4 });
    noise(.12, .013, .3);
  };
  // Section transition — a clean descending swoop diving into a rising chirp,
  // with space. Sounds like dropping into the section you picked.
  const transition = () => {
    blip({ f: 1700, type: 'sine',     d: .26, g: .032, to: 200,  reverb: .3 });
    blip({ f: 300,  type: 'triangle', d: .18, g: .028, to: 1400, delay: .12, reverb: .35 });
    blip({ f: 1046, type: 'sine',     d: .16, g: .022, delay: .26, reverb: .45 });
    noise(.08, .010, .25);
  };

  // Start audio on the first user gesture (autoplay policy).
  function firstGesture() {
    if (started) return; started = true;
    ctx(); if (!muted) { startHum(); loadSweep(); }
  }
  ['pointerdown', 'keydown', 'touchstart'].forEach(ev => addEventListener(ev, firstGesture, { once: true, passive: true }));

  // Soft tick on any interactive click, site-wide.
  addEventListener('pointerdown', e => {
    if (muted || !started) return;
    if (e.target.closest && e.target.closest('a,button,.mod,[role="button"],.tool,summary,input,select,.nav-links a')) clickTick();
  }, { passive: true });

  // Navigation flourish — internal page links play the transition sound, with a
  // short hold so it lands before the page swaps. Skips external/new-tab/download
  // /anchor links and modified clicks; never delays when muted or reduced-motion.
  addEventListener('click', e => {
    if (POSEIDON_REDUCED) return;
    if (e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    const a = e.target.closest && e.target.closest('a[href]');
    if (!a || a.target === '_blank' || a.hasAttribute('download')) return;
    let url; try { url = new URL(a.href, location.href); } catch { return; }
    if (url.origin !== location.origin) return;              // external
    if (url.pathname === location.pathname) return;          // same page / in-page anchor
    if (!/(\.html|\/)$/.test(url.pathname)) return;          // page navigations only
    started = true; ctx();
    if (muted) return;                                       // no sound → no delay
    e.preventDefault();
    transition();
    setTimeout(() => { location.href = a.href; }, 170);
  });

  // Mute toggle (bottom-left), shared across the suite.
  const tgl = document.createElement('button');
  tgl.id = 'fxToggle'; tgl.type = 'button'; tgl.setAttribute('aria-label', 'Toggle ambient sound');
  const paint = () => { tgl.textContent = muted ? '♪ SOUND OFF' : '♪ SOUND ON'; tgl.style.opacity = muted ? '.4' : '.66'; };
  function setMuted(m) {
    muted = m; localStorage.setItem(MUTE_KEY, m ? '1' : '0'); paint();
    if (m) stopHum(); else { ctx(); startHum(); }
    window.dispatchEvent(new CustomEvent('poseidon-mute', { detail: muted }));
  }
  tgl.addEventListener('click', e => { e.stopPropagation(); started = true; setMuted(!muted); });
  paint();
  const mount = () => { if (!document.getElementById('fxToggle') && document.body) { document.body.appendChild(tgl); if (!document.getElementById('fxScan')) { const s = document.createElement('div'); s.id = 'fxScan'; s.setAttribute('aria-hidden', 'true'); document.body.appendChild(s); } } };
  if (document.body) mount(); else addEventListener('DOMContentLoaded', mount);

  // Shared API so per-page FX (e.g. the codex CRT) can read/drive mute state.
  window.PoseidonSound = { isMuted: () => muted, setMuted, blip, noise, tick: clickTick };
})();
