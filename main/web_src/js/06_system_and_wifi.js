// Performance & CPU Control
let currentCpuFreq = 240;
let currentWifiPs = false;

function updatePresetUi(freq, ps) {
  ['240', '160', '80'].forEach(f => {
    let b = document.getElementById('btn-preset-' + f);
    if (b) b.classList.remove('active');
  });
  if (freq === 240 && !ps) {
    let b = document.getElementById('btn-preset-240');
    if (b) b.classList.add('active');
  } else if (freq === 160 && !ps) {
    let b = document.getElementById('btn-preset-160');
    if (b) b.classList.add('active');
  } else if (freq === 80 && ps) {
    let b = document.getElementById('btn-preset-80');
    if (b) b.classList.add('active');
  }
}

function updateDashboardCpuFreq(freq) {
  let el = document.getElementById('cpu-clock-str');
  if (el) el.textContent = 'التردد: ' + freq + ' MHz (Dual Core)';
  let badge = document.getElementById('perf-badge-freq');
  if (badge) badge.textContent = freq + ' MHz';
}

function selectCpuPreset(freq, ps) {
  currentCpuFreq = freq;
  currentWifiPs = ps;
  let sel = document.getElementById('cfg_cpu_freq');
  if (sel) sel.value = String(freq);
  let chk = document.getElementById('cfg_wifi_ps');
  if (chk) chk.checked = ps;
  updatePresetUi(freq, ps);
  updateDashboardCpuFreq(freq);
}

function onManualCpuFreqChange(val) {
  currentCpuFreq = parseInt(val, 10) || 240;
  let chk = document.getElementById('cfg_wifi_ps');
  currentWifiPs = chk ? chk.checked : false;
  updatePresetUi(currentCpuFreq, currentWifiPs);
  updateDashboardCpuFreq(currentCpuFreq);
}

function onManualPowerSaveChange(val) {
  currentWifiPs = !!val;
  let sel = document.getElementById('cfg_cpu_freq');
  currentCpuFreq = sel ? (parseInt(sel.value, 10) || 240) : 240;
  updatePresetUi(currentCpuFreq, currentWifiPs);
}

async function submitPerformance(e) {
  if (e) e.preventDefault();
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  let freq = document.getElementById('cfg_cpu_freq').value;
  let ps = document.getElementById('cfg_wifi_ps').checked ? '1' : '0';
  let btn = document.getElementById('perf_save_btn');
  if (btn) btn.disabled = true;
  try {
    let res = await fetch('/api/performance', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({cpu_freq: freq, wifi_ps: ps})
    });
    if (res.ok) {
      updateDashboardCpuFreq(parseInt(freq, 10));
      updatePresetUi(parseInt(freq, 10), ps === '1');
      showToast('تم تطبيق إعدادات أداء المعالج والواي فاي بنجاح!');
    } else {
      showToast('فشل حفظ إعدادات الأداء', 'error');
    }
  } catch (e) {
    showToast('حدث خطأ في الاتصال بالراوتر', 'error');
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function submitAutoReboot(e) {
  e.preventDefault();
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  let en = document.getElementById('cfg_rb_enable').checked ? '1' : '0';
  let tm = document.getElementById('cfg_rb_time').value;
  let tz = document.getElementById('cfg_rb_tz').value;
  let btn = document.getElementById('rb_save_btn');
  btn.disabled = true;
  try {
    await fetch('/api/auto_reboot', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({enable: en, time: tm, tz: tz})
    });
    showToast('تم حفظ مؤقت إعادة التشغيل اليومي بنجاح!');
  } catch (e) {}
  finally { btn.disabled = false; }
}

async function submitCountdownReboot(e) {
  e.preventDefault();
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  let min = document.getElementById('cfg_cd_min').value;
  if (!confirm('بدء عد تنازلي لإعادة تشغيل الراوتر بعد ' + min + ' دقيقة؟')) return;
  try {
    await fetch('/api/system/reboot', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({countdown_min: min})
    });
    showToast('تم تفعيل المؤقت التنازلي للريستارت!');
  } catch (e) {}
}

function updateMeterScaleBadges(sc, std, mode) {
  let bSc = document.getElementById('badge-meter-scale');
  if (bSc) bSc.textContent = sc + '%';
  let bStd = document.getElementById('badge-gb-std');
  if (bStd) bStd.textContent = std + ' MB';
  let bMode = document.getElementById('badge-meter-mode');
  if (bMode) bMode.textContent = (mode === 1) ? 'تراكمي' : 'فوري وشامل';
}

function onMeterScaleChange(val) {
  meterScalePct = parseInt(val, 10) || 100;
  updateMeterScaleBadges(meterScalePct, gbStandard, meterScaleMode);
}

function onGbStandardChange(val) {
  gbStandard = parseInt(val, 10) || 1024;
  updateMeterScaleBadges(meterScalePct, gbStandard, meterScaleMode);
  renderDevices();
}

function onMeterModeChange(val) {
  meterScaleMode = parseInt(val, 10) || 0;
  updateMeterScaleBadges(meterScalePct, gbStandard, meterScaleMode);
}

async function saveMeterScaleConfig() {
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  let sc = document.getElementById('cfg_meter_scale').value;
  let std = document.getElementById('cfg_gb_std').value;
  let mode = document.getElementById('cfg_meter_mode')?.value || '0';
  let btn = document.getElementById('scale_save_btn');
  if (btn) btn.disabled = true;
  try {
    let res = await fetch('/api/meter_scale', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({scale: sc, gb_std: std, mode: mode})
    });
    if (res.ok) {
      gbStandard = parseInt(std, 10) || 1024;
      meterScalePct = parseInt(sc, 10) || 100;
      meterScaleMode = parseInt(mode, 10) || 0;
      updateMeterScaleBadges(meterScalePct, gbStandard, meterScaleMode);
      renderDevices();
      showToast('تم حفظ وتطبيق إعدادات المعايرة ونمط الاحتساب بنجاح!');
    } else {
      showToast('فشل حفظ إعدادات المعايرة', 'error');
    }
  } catch (err) {
    showToast('حدث خطأ في الاتصال بالراوتر', 'error');
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function changeAdminPassword(e) {
  e.preventDefault();
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  let oldP = document.getElementById('pw_old').value;
  let newU = document.getElementById('pw_new_user').value.trim();
  let newP = document.getElementById('pw_new_pass').value;
  let btn = document.getElementById('pw_save_btn');
  btn.disabled = true;
  try {
    let res = await fetch('/api/password', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({old_pass: oldP, new_user: newU, new_pass: newP})
    });
    let data = await res.json();
    if (res.ok && data.ok) {
      localStorage.setItem('esp_admin_user', newU);
      localStorage.setItem('esp_admin_pass', newP);
      showToast('تم تحديث بيانات المشرف بنجاح!');
      document.getElementById('pw_old').value = '';
      document.getElementById('pw_new_pass').value = '';
    } else {
      showToast('تعذر التحديث: ' + (data.error || 'خطأ'), 'error');
    }
  } catch (e) { showToast('خطأ في الاتصال', 'error'); }
  finally { btn.disabled = false; }
}

async function rebootRouter() {
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  if (!confirm('هل تريد إعادة تشغيل الراوتر الآن؟')) return;
  try {
    await fetch('/api/system/reboot', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: 'immediate=1'
    });
    showToast('جاري إعادة تشغيل الراوتر الآن... انتظر حوالي 10 ثوانٍ.');
  } catch (e) {}
}

setInterval(refreshData, 2000);
refreshData();
if (!localStorage.getItem('esp_session_token') && localStorage.getItem('esp_admin_user') && localStorage.getItem('esp_admin_pass')) {
  ensureAdminAuth().then(() => refreshData());
}
window.addEventListener('resize', drawSpeedChart);

function autoDetectLocalTimezone() {
  const offsetMin = -new Date().getTimezoneOffset();
  const offsetHours = Math.round(offsetMin / 60);
  const tzSelect = document.getElementById('cfg_rb_tz');
  if (tzSelect) {
    let found = false;
    for (let opt of tzSelect.options) {
      if (parseInt(opt.value) === offsetHours) {
        tzSelect.value = String(offsetHours);
        found = true;
        break;
      }
    }
    if (!found) {
      const newOpt = document.createElement('option');
      newOpt.value = String(offsetHours);
      newOpt.textContent = `UTC${offsetHours >= 0 ? '+' : ''}${offsetHours} (توقيت جهازك الحالي)`;
      tzSelect.appendChild(newOpt);
      tzSelect.value = String(offsetHours);
    }
  }
  alert(`تم اكتشاف وتعيين المنطقة الزمنية بنجاح: UTC${offsetHours >= 0 ? '+' : ''}${offsetHours}`);
}

let _isScanningWifi = false;

async function scanWifiNetworks() {
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  if (_isScanningWifi) return;
  _isScanningWifi = true;

  const btn = document.getElementById('btn_scan_wifi');
  const spinner = document.getElementById('scan_spinner');
  const icon = document.getElementById('scan_icon');
  const btnText = document.getElementById('scan_btn_text');
  const dropdown = document.getElementById('wifi_scan_results');

  if (btn) btn.disabled = true;
  if (spinner) spinner.style.display = 'inline-block';
  if (icon) icon.style.display = 'none';
  if (btnText) btnText.textContent = 'جاري الفحص...';

  if (dropdown) {
    dropdown.style.display = 'block';
    dropdown.innerHTML = `
      <div style="padding:14px;text-align:center;color:var(--muted);font-size:0.86rem;display:flex;align-items:center;justify-content:center;gap:10px">
        <span style="display:inline-block;animation:spin 1s linear infinite">🔄</span>
        <span>جاري فحص جميع قنوات 2.4GHz و 5GHz المحيطة...</span>
      </div>`;
  }

  try {
    const res = await fetch('/api/wifi/scan', {
      headers: getAuthHeaders()
    });
    const data = await res.json();

    if (!data.ok) {
      if (dropdown) {
        dropdown.innerHTML = `
          <div style="padding:12px;text-align:center;color:var(--red);font-size:0.85rem">
            ❌ تعذر إتمام فحص الشبكات (${data.error || 'خطأ'}). يرجى المحاولة لاحقاً.
          </div>`;
      }
      return;
    }

    const aps = data.aps || [];
    if (aps.length === 0) {
      if (dropdown) {
        dropdown.innerHTML = `
          <div style="padding:14px;text-align:center;color:var(--muted);font-size:0.85rem">
            لم يتم العثور على أي شبكات واي فاي قريبة.
          </div>`;
      }
      return;
    }

    let html = `
      <div style="display:flex;justify-content:space-between;align-items:center;padding:8px 12px;border-bottom:1px solid var(--border-subtle);margin-bottom:4px">
        <span style="font-weight:700;font-size:0.82rem;color:var(--purple-color)">الشبكات المكتشفة (${aps.length})</span>
        <button type="button" onclick="document.getElementById('wifi_scan_results').style.display='none'" style="background:none;border:none;color:var(--muted);cursor:pointer;font-size:1.1rem;padding:2px 6px">✕</button>
      </div>
      <div style="max-height:240px;overflow-y:auto;display:flex;flex-direction:column;gap:3px">
    `;

    aps.forEach(ap => {
      let sigIcon = '📶';
      let sigColor = 'var(--green)';
      if (ap.rssi < -75) { sigColor = 'var(--red)'; }
      else if (ap.rssi < -65) { sigColor = 'var(--amber)'; }

      const is5G = (ap.band === '5GHz');
      const bandBadge = is5G
        ? `<span class="badge" style="background:var(--purple-subtle);color:var(--purple-color);border:1px solid var(--purple-border);font-size:0.72rem;padding:2px 6px;font-weight:700">5 GHz</span>`
        : `<span class="badge" style="background:var(--cyan-subtle);color:var(--cyan);border:1px solid var(--cyan-border);font-size:0.72rem;padding:2px 6px;font-weight:700">2.4 GHz</span>`;

      const openBadge = ap.open
        ? `<span style="font-size:0.7rem;color:var(--green);background:var(--green-subtle);padding:1px 6px;border-radius:4px;border:1px solid var(--green-border)">🔓 مفتوحة</span>`
        : `<span style="font-size:0.7rem;color:var(--muted)" title="مشفرة">🔒</span>`;

      const safeSsid = ap.ssid.replace(/\\/g, '\\\\').replace(/'/g, "\\'").replace(/"/g, '&quot;');

      html += `
        <div onclick="selectScannedWifi('${safeSsid}', ${ap.open ? 'true' : 'false'}, '${ap.band}')" 
             style="display:flex;justify-content:space-between;align-items:center;padding:9px 12px;border-radius:8px;cursor:pointer;background:var(--surface-alt);border:1px solid var(--glass-border);transition:background 0.15s;gap:8px"
             onmouseover="this.style.background='var(--glass-border)'" 
             onmouseout="this.style.background='var(--surface-alt)'">
          <div style="display:flex;align-items:center;gap:8px;min-width:0;flex:1">
            <span style="font-size:1.05rem;color:${sigColor}">${sigIcon}</span>
            <span style="font-weight:700;font-size:0.88rem;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;direction:ltr;text-align:left">${ap.ssid}</span>
            ${openBadge}
          </div>
          <div style="display:flex;align-items:center;gap:8px;flex-shrink:0">
            <span style="font-size:0.75rem;color:${sigColor};font-family:monospace">${ap.rssi} dBm</span>
            ${bandBadge}
          </div>
        </div>
      `;
    });

    html += `</div>`;
    if (dropdown) dropdown.innerHTML = html;

  } catch (err) {
    if (dropdown) {
      dropdown.innerHTML = `
        <div style="padding:12px;text-align:center;color:var(--red);font-size:0.85rem">
          ❌ حدث خطأ أثناء الاتصال بالراوتر لفحص الشبكات.
        </div>`;
    }
  } finally {
    _isScanningWifi = false;
    if (btn) btn.disabled = false;
    if (spinner) spinner.style.display = 'none';
    if (icon) icon.style.display = 'inline-block';
    if (btnText) btnText.textContent = 'فحص الشبكات المحيطة';
  }
}

function selectScannedWifi(ssid, isOpen, band) {
  const ssidInput = document.getElementById('cfg_router_ssid');
  const passInput = document.getElementById('cfg_router_pass');
  const clearPassChk = document.getElementById('cfg_clear_router_password');
  const dropdown = document.getElementById('wifi_scan_results');

  if (ssidInput) ssidInput.value = ssid;
  if (dropdown) dropdown.style.display = 'none';

  if (isOpen) {
    if (clearPassChk) clearPassChk.checked = true;
    if (passInput) passInput.value = '';
    showToast(`تم اختيار الشبكة المفتوحة: ${ssid} (${band})`, 'info');
  } else {
    if (clearPassChk) clearPassChk.checked = false;
    if (passInput) {
      passInput.focus();
    }
    showToast(`تم اختيار: ${ssid} (${band}) - أدخل كلمة المرور`, 'info');
  }
}

document.addEventListener('click', function(e) {
  const dropdown = document.getElementById('wifi_scan_results');
  const btn = document.getElementById('btn_scan_wifi');
  if (dropdown && dropdown.style.display !== 'none') {
    if (!dropdown.contains(e.target) && (!btn || !btn.contains(e.target))) {
      dropdown.style.display = 'none';
    }
  }
});
