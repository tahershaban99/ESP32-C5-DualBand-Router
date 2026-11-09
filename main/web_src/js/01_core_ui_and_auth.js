// Dynamic CSS token getter for canvas, charts and dynamic styling
function getToken(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

// -----------------------------------------------------------------------------
// GLOBAL FULL-PAGE ZOOM / SCALE CONTROLLER
// -----------------------------------------------------------------------------
let currentZoom = 85;

function updateStickyTopHeight() {
  let el = document.getElementById('sticky-top-section');
  if (el && el.offsetHeight > 0) {
    document.documentElement.style.setProperty('--sticky-top-height', el.offsetHeight + 'px');
  }
}
window.addEventListener('resize', updateStickyTopHeight);
window.addEventListener('DOMContentLoaded', updateStickyTopHeight);
setTimeout(updateStickyTopHeight, 100);

function applyPageZoom(pct, saveToStorage = true) {
  pct = Math.max(70, Math.min(110, pct));
  currentZoom = pct;
  
  // Apply zoom to document.body (scales entire page and all elements)
  document.body.style.zoom = (pct / 100);

  // Update display text
  const disp = document.getElementById('zoom-display');
  if (disp) {
    disp.textContent = pct + '%';
    disp.title = `نسبة العرض الحالية: ${pct}% (انقر للتبديل بين 85% و 100%)`;
  }
  
  if (saveToStorage) {
    try { localStorage.setItem('esp_page_zoom', pct); } catch(e){}
  }
  
  setTimeout(updateStickyTopHeight, 60);

  if (typeof drawSpeedChart === 'function') {
    setTimeout(drawSpeedChart, 60);
  }
}

let _toastTimer = null;
function showToast(msg, type = 'success') {
  let toast = document.getElementById('live-floating-toast');
  let txt = document.getElementById('toast-text');
  if (!toast) return;
  if (!txt) {
    txt = document.createElement('span');
    txt.id = 'toast-text';
    toast.appendChild(txt);
  }

  let icon = '✅';
  let bg = 'var(--toast-success-bg)';
  let border = 'var(--toast-success-border)';
  let color = 'var(--toast-success-text)';

  if (type === 'error') {
    bg = 'var(--toast-error-bg)';
    border = 'var(--toast-error-border)';
    color = 'var(--toast-error-text)';
    icon = '❌';
  } else if (type === 'warn') {
    bg = 'var(--toast-warning-bg)';
    border = 'var(--toast-warning-border)';
    color = 'var(--toast-warning-text)';
    icon = '⚠️';
  } else if (type === 'info') {
    bg = 'var(--toast-info-bg)';
    border = 'var(--toast-info-border)';
    color = 'var(--toast-info-text)';
    icon = 'ℹ️';
  }

  toast.style.background = bg;
  toast.style.borderColor = border;
  toast.style.color = color;
  txt.innerHTML = `<span style="margin-left:4px">${icon}</span> ${msg}`;

  clearTimeout(_toastTimer);
  _toastTimer = setTimeout(() => {
    toast.style.background = 'var(--toast-success-bg)';
    toast.style.borderColor = 'var(--toast-success-border)';
    toast.style.color = 'var(--toast-success-text)';
    txt.textContent = 'يتم تحديث السرعة اللحظية والاستهلاك مباشرة كل ثانيتين';
  }, 3200);
}

function formatLiveSpeed(bps) {
  if (!bps || bps <= 0) return '0.00 Mbps';
  let mbps = bps / 1000000.0;
  return `${mbps.toFixed(2)} Mbps`;
}

function formatDuration(sec) {
  if (!sec || sec < 0) return 'الآن';
  let d = Math.floor(sec / 86400);
  let h = Math.floor((sec % 86400) / 3600);
  let m = Math.floor((sec % 3600) / 60);
  let s = Math.floor(sec % 60);
  if (d > 0) return `${d}ي ${h}س`;
  if (h > 0) return `${h}س ${m}د`;
  if (m > 0) return `${m}د ${s}ث`;
  return `${s}ث`;
}

function formatLastSeen(sec) {
  if (!sec || sec <= 0) return 'غير محدد';
  let d = Math.floor(sec / 86400);
  let h = Math.floor((sec % 86400) / 3600);
  let m = Math.floor((sec % 3600) / 60);
  if (d > 0) return `منذ ${d}ي ${h}س`;
  if (h > 0) return `منذ ${h}س`;
  if (m > 0) return `منذ ${m}د`;
  return `منذ ثوانٍ`;
}

let _pendingAction = null;

function adjustZoom(delta) {
  applyPageZoom(currentZoom + delta, true);
  showToast(`🔍 تم ضبط حجم الصفحة بالكامل على: ${currentZoom}%`);
}

function resetZoom() {
  const target = (currentZoom === 100) ? 85 : 100;
  applyPageZoom(target, true);
  showToast(`🔍 تم إعادة ضبط حجم الصفحة إلى: ${target}%`);
}

// Automatically load and apply initial zoom on page load
(function() {
  let saved = null;
  try { saved = localStorage.getItem('esp_page_zoom'); } catch(e){}
  let initVal = saved ? parseInt(saved, 10) : 85;
  if (isNaN(initVal) || initVal < 70 || initVal > 110) initVal = 85;
  applyPageZoom(initVal, false);
})();


function getDeviceIcon(name) {
  if (!name) return '📱';
  let n = name.toLowerCase();
  if (n.match(/(galaxy|iphone|pixel|xiaomi|redmi|huawei|oppo|vivo|realme|oneplus|honor|phone|mobile|هاتف|موبايل|جوال)/)) return '📱';
  if (n.match(/(thinkpad|dell|xps|macbook|laptop|pc|desktop|notebook|hp|lenovo|asus|acer|كمبيوتر|لابتوب|حاسوب)/)) return '💻';
  if (n.match(/(playstation|ps5|ps4|ps3|xbox|switch|nintendo|steam|gaming|gamer|ألعاب|بلايستيشن|كونسول)/)) return '🎮';
  if (n.match(/(tv|smart tv|apple tv|firetv|chromecast|roku|oled|qled|bravia|شاشة|تلفاز|تلفزيون)/)) return '📺';
  if (n.match(/(watch|apple watch|band|gear|ساعة)/)) return '⌚';
  if (n.match(/(ipad|tablet|tab|لوحي|تابلت|آيباد)/)) return '📱';
  if (n.match(/(printer|طابعة)/)) return '🖨️';
  if (n.match(/(cam|camera|كاميرا)/)) return '📷';
  return '🖥️';
}

let isAdmin = false;
let currentTheme = localStorage.getItem('esp_theme') || 'dark';
if (currentTheme === 'sunset') currentTheme = 'dark';
if (currentTheme === 'matrix') currentTheme = 'light';
let viewMode = localStorage.getItem('esp_view') || 'cards';
let speedHistoryDown = new Array(30).fill(0);
let speedHistoryUp = new Array(30).fill(0);

function setTheme(theme) {
  if (theme === 'sunset') theme = 'dark';
  if (theme === 'matrix') theme = 'light';
  currentTheme = theme;
  document.documentElement.setAttribute('data-theme', theme);
  localStorage.setItem('esp_theme', theme);
  let btnDark = document.getElementById('btn-theme-sunset');
  let btnLight = document.getElementById('btn-theme-matrix');
  if (btnDark) btnDark.classList.toggle('active', theme === 'dark');
  if (btnLight) btnLight.classList.toggle('active', theme === 'light');
  drawSpeedChart();
  if (typeof drawOvSparkline === 'function') drawOvSparkline();
}
setTheme(currentTheme);


let gbStandard = 1024;
let meterScalePct = 100;
let meterScaleMode = 0;

function formatSmartBytes(bytes) {
  if (!bytes || bytes <= 0) return '0 MB';
  let div = (gbStandard === 1000) ? 1000 : 1024;
  let divMB = div * div;
  let divGB = divMB * div;
  if (bytes < divGB) {
    let mb = (bytes / divMB).toFixed(bytes < (divMB * 10) ? 1 : 0);
    return mb + ' MB';
  }
  return (bytes / divGB).toFixed(2) + ' GB';
}

let offlineSectionVisible = true;

let userOverrodeView = false;
let lastDevicesData = [];

function setViewMode(mode, isManual = true) {
  if (isManual) {
    userOverrodeView = true;
    try { localStorage.setItem('esp_view', mode); } catch(e){}
  }
  let prevMode = viewMode;
  viewMode = mode;
  let btnCards = document.getElementById('btn-view-cards');
  let btnTbl = document.getElementById('btn-view-table');
  if (btnCards) btnCards.classList.toggle('active', mode === 'cards');
  if (btnTbl) btnTbl.classList.toggle('active', mode === 'table');
  let cc = document.getElementById('cards-container');
  let tc = document.getElementById('table-container');
  let occ = document.getElementById('offline-cards-container');
  let otc = document.getElementById('offline-table-container');
  if (cc) cc.style.display = (mode === 'cards') ? 'grid' : 'none';
  if (tc) tc.style.display = (mode === 'table') ? 'block' : 'none';
  if (occ) occ.style.display = (mode === 'cards') ? 'grid' : 'none';
  if (otc) otc.style.display = (mode === 'table') ? 'block' : 'none';

  if (prevMode !== mode && lastDevicesData && lastDevicesData.length > 0) {
    renderDevices(lastDevicesData);
  }
}

function handleResponsiveView() {
  if (userOverrodeView) return;
  let isMobile = window.innerWidth <= 768;
  let saved = null;
  try { saved = localStorage.getItem('esp_view'); } catch(e){}
  if (saved) {
    setViewMode(saved, false);
  } else {
    setViewMode(isMobile ? 'cards' : 'table', false);
  }
}

window.addEventListener('resize', handleResponsiveView);
window.addEventListener('load', handleResponsiveView);
handleResponsiveView();

function toggleOfflineSection() {
  offlineSectionVisible = !offlineSectionVisible;
  let sec = document.getElementById('offline-devices-section');
  let icon = document.getElementById('offline-toggle-icon');
  let txt = document.getElementById('offline-toggle-text');
  let badge = document.getElementById('offline-count-badge');
  let countStr = badge ? badge.textContent : '0 أجهزة';
  if (offlineSectionVisible) {
    if (sec) sec.style.display = 'block';
    if (icon) icon.textContent = '▼';
    if (txt) txt.textContent = `إخفاء الأجهزة غير المتصلة (${countStr})`;
    if (lastDevicesData && lastDevicesData.length > 0) {
      renderDevices(lastDevicesData);
    }
  } else {
    if (sec) sec.style.display = 'none';
    if (icon) icon.textContent = '▲';
    if (txt) txt.textContent = `عرض الأجهزة غير المتصلة (${countStr})`;
  }
}



function switchTab(tabId, btn, shouldScrollTop = false) {
  document.querySelectorAll('.tab-btn, .sidebar-tabs .tab-btn, .floating-tab-btn').forEach(b => {
    let onclickAttr = b.getAttribute('onclick') || '';
    let dataTab = b.getAttribute('data-tab') || '';
    if (onclickAttr.includes(`'${tabId}'`) || dataTab === tabId) {
      b.classList.add('active');
    } else {
      b.classList.remove('active');
    }
  });

  document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
  let target = document.getElementById(tabId);
  if (target) target.classList.add('active');

  // Instant collapse or expand of top 7 metric cards on Overview vs other tabs
  let mg = document.querySelector('.metrics-grid');
  if (mg) {
    if (tabId === 'tab-overview') {
      mg.classList.add('collapsed-on-overview');
    } else {
      mg.classList.remove('collapsed-on-overview');
    }
  }

  // Update sticky header & sidebar positions immediately with zero lag
  if (typeof updateStickyTopHeight === 'function') {
    updateStickyTopHeight();
  }

  if (tabId === 'tab-overview' && typeof drawOvSparkline === 'function') setTimeout(drawOvSparkline, 40);
  if (tabId === 'tab-reboot' && typeof drawSpeedChart === 'function') setTimeout(drawSpeedChart, 40);

  if (shouldScrollTop) {
    let sc = document.querySelector('.main-content-scroll') || window;
    if (sc.scrollTo) sc.scrollTo({ top: 0, behavior: 'instant' });
    else if (window.scrollTo) window.scrollTo({ top: 0, behavior: 'instant' });
  }
}

function togglePwVis(id, btn) {
  let el = document.getElementById(id);
  if (!el) return;
  if (el.type === 'password') { el.type = 'text'; btn.textContent = '🙈'; }
  else { el.type = 'password'; btn.textContent = '👁️'; }
}

function getAuthHeaders(extra = {}) {
  let headers = {...extra};
  let token = localStorage.getItem('esp_session_token');
  if (token) {
    headers['X-Session-Token'] = token;
  }
  return headers;
}

let _isAuthenticating = false;
async function ensureAdminAuth() {
  if (isAdmin) return true;
  if (_isAuthenticating) return false;
  let u = localStorage.getItem('esp_admin_user');
  let p = localStorage.getItem('esp_admin_pass');
  if (!u || !p) return false;
  _isAuthenticating = true;
  try {
    let res = await fetch('/api/login', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: new URLSearchParams({username: u, password: p})
    });
    if (res.ok) {
      let data = await res.json();
      if (data.ok) {
        isAdmin = true;
        if (data.token) localStorage.setItem('esp_session_token', data.token);
        _isAuthenticating = false;
        return true;
      }
    }
  } catch(e) {}
  _isAuthenticating = false;
  return false;
}

let _actionInFlight = false;
let isOtaActive = false;
async function postWithRetry(url, params, maxRetries = 1) {
  _actionInFlight = true;
  for (let attempt = 0; attempt <= maxRetries; attempt++) {
    try {
      let bodyData = (typeof params === 'string') ? params : new URLSearchParams(params);
      let res = await fetch(url, {
        method: 'POST',
        headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
        body: bodyData
      });
      if (res.status === 401) {
        isAdmin = false;
        _actionInFlight = false;
        let authBtn = document.getElementById('auth-btn');
        if (authBtn) {
          authBtn.className = 'auth-btn';
          authBtn.innerHTML = '<span>👁️</span> <span>وضع المراقبة (دخول المشرف)</span>';
          authBtn.title = 'أنت في وضع المراقبة (عرض فقط). انقر لتسجيل الدخول كمشرف لتعديل الإعدادات.';
        }
        return { ok: false, status: 401, error: 'unauthorized' };
      }
      if (res.ok) {
        let json = null;
        try { json = await res.json(); } catch(e) {}
        _actionInFlight = false;
        return { ok: true, data: json };
      }
      if (attempt < maxRetries) {
        await new Promise(r => setTimeout(r, 400));
        continue;
      }
      let errTxt = '';
      try { let j = await res.json(); errTxt = j.error || ''; } catch(e){}
      _actionInFlight = false;
      return { ok: false, status: res.status, error: errTxt };
    } catch(e) {
      if (attempt < maxRetries) {
        await new Promise(r => setTimeout(r, 400));
        continue;
      }
      _actionInFlight = false;
      return { ok: false, status: 0, error: 'network_error' };
    }
  }
  _actionInFlight = false;
  return { ok: false, status: 0, error: 'network_error' };
}

function openLoginModal(msg) {
  let m = document.getElementById('login-modal');
  let r = document.getElementById('login-reason-banner');
  let errBox = document.getElementById('login-error');
  if (errBox) errBox.style.display = 'none';
  if (r) {
    r.textContent = '🔒 ' + (msg || 'هذا الإجراء يتطلب صلاحيات المشرف لتأكيد التعديل وحفظه.');
    r.style.display = 'block';
  }
  if (m) {
    m.style.display = 'flex';
    let u = document.getElementById('login_user');
    let p = document.getElementById('login_pass');
    if (u && !u.value) u.value = localStorage.getItem('esp_admin_user') || 'admin';
    if (p && !p.value) p.value = localStorage.getItem('esp_admin_pass') || 'admin';
    if (u) u.focus();
  }
}
function closeLoginModal() {
  let m = document.getElementById('login-modal');
  if (m) m.style.display = 'none';
}
function handleAuthClick() {
  if (isAdmin) {
    if (confirm('هل تريد بالتأكيد تسجيل الخروج من صلاحيات المشرف والعودة لوضع المراقبة فقط؟')) logoutAdmin();
  } else {
    openLoginModal('سجل الدخول كمشرف لتفعيل التحكم والتعديل الكامل');
  }
}

async function submitLogin(e) {
  e.preventDefault();
  let u = document.getElementById('login_user').value.trim();
  let p = document.getElementById('login_pass').value;
  let btn = document.getElementById('login_submit_btn');
  let errBox = document.getElementById('login-error');
  let lockBox = document.getElementById('login-lockout');
  errBox.style.display = 'none'; lockBox.style.display = 'none';
  btn.disabled = true;

  try {
    let res = await fetch('/api/login', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: new URLSearchParams({username: u, password: p})
    });
    let data = await res.json();
    if (res.ok && data.ok) {
      isAdmin = true;
      if (data.token) localStorage.setItem('esp_session_token', data.token);
      localStorage.setItem('esp_admin_user', u);
      localStorage.setItem('esp_admin_pass', p);
      closeLoginModal();
      showToast('تم تسجيل الدخول كمشرف بنجاح! 🔓', 'success');
      if (_pendingAction) {
        let act = _pendingAction;
        _pendingAction = null;
        await act();
      }
      refreshData();
    } else if (res.status === 429) {
      lockBox.style.display = 'block';
      lockBox.textContent = '⚠️ تم حظر المحاولات مؤقتاً لتكرار الخطأ! انتظر ' + (data.remaining || 60) + ' ثانية.';
    } else {
      errBox.style.display = 'block';
      errBox.textContent = 'بيانات الدخول غير صحيحة! متبقي ' + (data.attempts_left !== undefined ? data.attempts_left : '') + ' محاولات.';
    }
  } catch (err) {
    errBox.style.display = 'block';
    errBox.textContent = 'تعذر الاتصال بالراوتر';
  } finally {
    btn.disabled = false;
  }
}

async function logoutAdmin() {
  localStorage.removeItem('esp_session_token');
  localStorage.removeItem('esp_admin_user');
  localStorage.removeItem('esp_admin_pass');
  try { await fetch('/api/logout', {method: 'POST'}); } catch (e) {}
  location.reload();
}
