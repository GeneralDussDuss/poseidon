/* POSEIDON site script. Null-guarded so subpages can share it
 * even when they don't have hero / particles / caustic canvases. */

// Boot sequence
window.addEventListener('load', () => {
  setTimeout(() => {
    const bo = document.getElementById('bootOverlay');
    const hud = document.getElementById('hud');
    if (bo) bo.classList.add('hidden');
    if (hud) setTimeout(() => hud.classList.add('visible'), 400);
  }, 1200);
});

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
  const px = pc.getContext('2d');
  function resize() { pc.width = window.innerWidth; pc.height = window.innerHeight; }
  resize();
  window.addEventListener('resize', resize);
  const pts = Array.from({ length: 50 }, () => ({
    x: Math.random() * pc.width, y: Math.random() * pc.height,
    vx: (Math.random() - 0.5) * 0.3, vy: (Math.random() - 0.5) * 0.3,
    r: Math.random() * 2 + 0.8, hue: Math.random() > 0.6 ? 190 : 205
  }));
  (function draw() {
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
    requestAnimationFrame(draw);
  })();
})();

// Hero constellation — home page only
(function heroConstellation() {
  const hc = document.getElementById('heroCanvas');
  if (!hc) return;
  const hx = hc.getContext('2d');
  function resize() { hc.width = hc.parentElement.clientWidth; hc.height = hc.parentElement.clientHeight; }
  resize();
  window.addEventListener('resize', resize);
  const nodes = Array.from({ length: 24 }, () => ({
    x: Math.random() * 1400, y: Math.random() * 900,
    vx: (Math.random() - 0.5) * 0.35, vy: (Math.random() - 0.5) * 0.35
  }));
  (function draw() {
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
    const t = Date.now() * 0.001;
    for (let w = 0; w < 2; w++) {
      const waveR = ((t + w * 1.3) % 4) * 180;
      hx.beginPath(); hx.arc(cx, cy, waveR, 0, Math.PI * 2);
      hx.strokeStyle = `rgba(14,165,233,${0.18 * (1 - waveR / 720)})`; hx.lineWidth = 1; hx.stroke();
    }
    requestAnimationFrame(draw);
  })();
})();

// Caustic water shader — home page only
(function caustic() {
  const cc = document.getElementById('causticLayer');
  if (!cc) return;
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
