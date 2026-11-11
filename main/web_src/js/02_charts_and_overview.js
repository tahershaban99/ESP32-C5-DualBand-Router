function drawSpeedChart() {
  let canvas = document.getElementById('speedChart');
  if (!canvas) return;
  let ctx = canvas.getContext('2d');
  let pw = canvas.parentElement ? canvas.parentElement.clientWidth : 0;
  if (pw <= 0) return;
  if (canvas.width !== pw) canvas.width = pw;
  if (canvas.height !== 130) canvas.height = 130;
  let w = canvas.width;
  let h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  let maxVal = Math.max(...speedHistoryDown, ...speedHistoryUp, 1000000); // at least 1 Mbps
  let step = w / (speedHistoryDown.length - 1);

  // Draw Download line
  ctx.beginPath();
  let colorDown = getToken('--green');
  ctx.strokeStyle = colorDown;
  ctx.lineWidth = 2.5;
  for (let i = 0; i < speedHistoryDown.length; i++) {
    let x = i * step;
    let y = h - (speedHistoryDown[i] / maxVal) * (h - 20) - 10;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Draw Upload line
  ctx.beginPath();
  let colorUp = getToken('--flow-up-color');
  ctx.strokeStyle = colorUp;
  ctx.lineWidth = 2.5;
  for (let i = 0; i < speedHistoryUp.length; i++) {
    let x = i * step;
    let y = h - (speedHistoryUp[i] / maxVal) * (h - 20) - 10;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

function formatBytes(bytes) {
  return formatSmartBytes(bytes);
}


// -----------------------------------------------------------------------------
// OVERVIEW TAB REAL-TIME RENDERING & LIVE SPARKLINE
// -----------------------------------------------------------------------------
let ovPeakSpeedMbps = 0.0;
let ovSpeedHistory = []; // stores last 30 readings (60s)

function updateOverviewTab(data, downMbps, upMbps, activeCount, vipCount, offlineCount) {
  try {
    // 1. Live Network Topology
    let wanSsidEl = document.getElementById('ov-wan-ssid');
    let wanDetailsEl = document.getElementById('ov-wan-details');
    let wanBandEl = document.getElementById('ov-wan-band');
    let rssi = data.sta_rssi || 0;
    let rssiPct = Math.min(100, Math.max(0, Math.round(2 * (rssi + 100))));

    if (wanSsidEl) wanSsidEl.textContent = data.sta_ssid || 'غير متصل بالمصدر';
    if (wanDetailsEl) {
      let gw = data.sta_gw || '2.2.2.2';
      wanDetailsEl.textContent = `GW: ${gw} • إشارة: ${rssi} dBm (${rssiPct}%)`;
    }
    if (wanBandEl) {
      wanBandEl.textContent = (data.sta_channel > 14) ? '5GHz' : '2.4GHz';
      wanBandEl.style.background = (data.sta_channel > 14) ? 'var(--green-subtle)' : 'var(--accent-subtle)';
      wanBandEl.style.color = (data.sta_channel > 14) ? 'var(--green)' : 'var(--accent)';
    }

    let ipTransEl = document.getElementById('ov-ip-transition');
    if (ipTransEl) {
      let staIp = data.sta_ip || '2.2.2.75';
      ipTransEl.textContent = `IP: ${staIp} ➔ 192.168.4.1`;
    }

    let coreSpecsEl = document.getElementById('ov-core-specs');
    let freeRamKb = Math.round((data.free_heap || 0) / 1024);
    let bwLabel = (data.current_bw === 40) ? 'HT40 Turbo (150M)' : (data.current_bw === 20 ? '20MHz (72M)' : 'HT40');
    if (coreSpecsEl) {
      coreSpecsEl.textContent = `${data.cpu_freq_mhz || 240}MHz • RAM: ${freeRamKb} KB • ${bwLabel}`;
    }

    let totalSpeedMbps = (parseFloat(downMbps) + parseFloat(upMbps)).toFixed(2);
    let speedWanEl = document.getElementById('ov-link-speed-wan');
    let speedLanEl = document.getElementById('ov-link-speed-lan');
    if (speedWanEl) speedWanEl.textContent = `Mbps ${totalSpeedMbps}`;
    if (speedLanEl) speedLanEl.textContent = `Mbps ${totalSpeedMbps}`;

    let lanCountEl = document.getElementById('ov-lan-count');
    let lanDetailsEl = document.getElementById('ov-lan-details');
    if (lanCountEl) lanCountEl.textContent = `${activeCount} أجهزة متصلة حالياً`;
    if (lanDetailsEl) lanDetailsEl.textContent = `${vipCount} جهاز VIP ألعاب 👑 • ${offlineCount} غير متصل`;

    // 2. Hardware Resources
    let ramValEl = document.getElementById('ov-ram-val');
    let ramBarEl = document.getElementById('ov-ram-bar');
    if (ramValEl) ramValEl.textContent = `${freeRamKb} KB (مستقر)`;
    if (ramBarEl) {
      let ramPct = Math.min(100, Math.max(10, Math.round((freeRamKb / 280) * 100)));
      ramBarEl.style.width = ramPct + '%';
    }

    let naptValEl = document.getElementById('ov-napt-val');
    let naptBarEl = document.getElementById('ov-napt-bar');
    let naptLoad = Math.min(100, Math.max(15, (activeCount * 8) + Math.round(parseFloat(totalSpeedMbps) * 2)));
    if (naptValEl) naptValEl.textContent = `${naptLoad}%`;
    if (naptBarEl) naptBarEl.style.width = `${naptLoad}%`;

    let rssiValEl = document.getElementById('ov-rssi-val');
    let rssiBarEl = document.getElementById('ov-rssi-bar');
    if (rssiValEl) rssiValEl.textContent = `${rssi} dBm (${rssiPct}%)`;
    if (rssiBarEl) rssiBarEl.style.width = `${rssiPct}%`;

    // 3. Quota & Balance Analysis
    let gbDiv = (data.gb_standard === 1000) ? 1000000000 : (1024 * 1024 * 1024);
    let consumedGb = ((data.total_up || 0) + (data.total_down || 0)) / gbDiv;
    let mainQuotaGb = parseFloat(data.main_quota_gb || 0);
    let remGb = (mainQuotaGb > 0) ? Math.max(0, mainQuotaGb - consumedGb) : 0;
    let quotaPct = (mainQuotaGb > 0) ? Math.min(100, (consumedGb / mainQuotaGb) * 100) : 0;

    let qRingEl = document.getElementById('ov-quota-ring');
    let qPctEl = document.getElementById('ov-quota-pct');
    if (qPctEl) qPctEl.textContent = quotaPct.toFixed(1) + '%';
    if (qRingEl) {
      let offset = 314 - (314 * (quotaPct / 100));
      qRingEl.style.strokeDashoffset = offset;
      if (quotaPct > 90) qRingEl.style.stroke = 'var(--red)';
      else if (quotaPct > 75) qRingEl.style.stroke = 'var(--amber)';
      else qRingEl.style.stroke = 'var(--green)';
    }

    let qConsEl = document.getElementById('ov-quota-consumed');
    let qTotEl = document.getElementById('ov-quota-total');
    let qRemEl = document.getElementById('ov-quota-rem');
    if (qConsEl) qConsEl.innerHTML = `${consumedGb.toFixed(2)} GB <span class="ov-stat-denom" id="ov-quota-total">/ ${mainQuotaGb.toFixed(1)} GB</span>`;
    if (qRemEl) qRemEl.textContent = `${remGb.toFixed(2)} GB`;

    let countdownEl = document.getElementById('ov-renew-countdown');
    let renewDateEl = document.getElementById('ov-renew-date');
    if (countdownEl) {
      if (data.rem_days >= 0) countdownEl.textContent = `${data.rem_days} يوم و ${data.rem_hours || 0} س`;
      else countdownEl.textContent = '--';
    }
    if (renewDateEl) renewDateEl.textContent = `التجديد: ${data.q_end || '--'}`;

    let safeRateEl = document.getElementById('ov-safe-rate');
    let safeBadgeEl = document.getElementById('ov-safe-badge');
    let safeDailyRate = (data.rem_days > 0) ? (remGb / data.rem_days) : remGb;
    if (safeRateEl) safeRateEl.textContent = `${safeDailyRate.toFixed(2)} GB / يوم`;
    
    let daysElapsed = Math.max(1, (data.elapsed_s || 86400) / 86400);
    let avgDailyUsage = consumedGb / daysElapsed;
    if (safeBadgeEl) {
      if (avgDailyUsage <= safeDailyRate || mainQuotaGb === 0) {
        safeBadgeEl.textContent = 'استهلاكك ضمن الآمن ✅';
        safeBadgeEl.style.color = 'var(--green)';
      } else {
        safeBadgeEl.textContent = 'تجاوز المعدل الآمن ⚠️';
        safeBadgeEl.style.color = 'var(--red)';
      }
    }

    let predictPillEl = document.getElementById('ov-predict-pill');
    if (predictPillEl) {
      if (remGb > 0 && avgDailyUsage <= safeDailyRate) {
        predictPillEl.textContent = '✅ توقع: تكفي لنهاية الشهر';
        predictPillEl.style.color = 'var(--green)';
        predictPillEl.style.borderColor = 'var(--green-border)';
      } else if (mainQuotaGb > 0 && remGb <= 0) {
        predictPillEl.textContent = '⚠️ انتهت الباقة بالكامل';
        predictPillEl.style.color = 'var(--red)';
        predictPillEl.style.borderColor = 'var(--red-border)';
      } else {
        predictPillEl.textContent = '⚠️ استهلاكك أسرع من المتوقع';
        predictPillEl.style.color = 'var(--amber)';
        predictPillEl.style.borderColor = 'var(--amber-border)';
      }
    }

    let meterAccEl = document.getElementById('ov-meter-acc');
    if (meterAccEl) {
      meterAccEl.textContent = `دقة العداد: ${data.meter_scale_pct || 100}% (معيار ${data.gb_standard || 1024}MB)`;
    }

    // 4. Live Bandwidth & Speed Meter
    let liveDownEl = document.getElementById('ov-live-down-mbps');
    let liveUpEl = document.getElementById('ov-live-up-mbps');
    let liveDownRawEl = document.getElementById('ov-live-down-raw');
    let liveUpRawEl = document.getElementById('ov-live-up-raw');
    if (liveDownEl) liveDownEl.textContent = downMbps;
    if (liveUpEl) liveUpEl.textContent = upMbps;
    if (liveDownRawEl) liveDownRawEl.textContent = `${Math.round((data.cur_down_bps || 0) / 1024)} KB/s`;
    if (liveUpRawEl) liveUpRawEl.textContent = `${Math.round((data.cur_up_bps || 0) / 1024)} KB/s`;

    let ringDownEl = document.getElementById('ov-speed-ring-down');
    let ringUpEl = document.getElementById('ov-speed-ring-up');
    let curDownF = parseFloat(downMbps);
    let curUpF = parseFloat(upMbps);
    let maxGaugeSpeed = Math.max(10, curDownF * 1.5, curUpF * 1.5);

    if (ringDownEl) {
      let downPct = Math.min(1, curDownF / maxGaugeSpeed);
      ringDownEl.style.strokeDashoffset = 251 - (251 * downPct);
    }
    if (ringUpEl) {
      let upPct = Math.min(1, curUpF / maxGaugeSpeed);
      ringUpEl.style.strokeDashoffset = 251 - (251 * upPct);
    }

    let curTotalMbps = curDownF + curUpF;
    if (curTotalMbps > ovPeakSpeedMbps) ovPeakSpeedMbps = curTotalMbps;
    let peakEl = document.getElementById('ov-peak-speed');
    if (peakEl) peakEl.textContent = `الذروة: Mbps ${ovPeakSpeedMbps.toFixed(2)}`;

    // Push into history and draw waveform
    ovSpeedHistory.push({ down: curDownF, up: curUpF });
    if (ovSpeedHistory.length > 30) ovSpeedHistory.shift();
    drawOvSparkline();

  } catch(err) {
    console.warn('updateOverviewTab error:', err);
  }
}

function drawOvSparkline() {
  try {
    let canvas = document.getElementById('ovLiveCanvas');
    if (!canvas || !canvas.getContext) return;
    let ctx = canvas.getContext('2d');
    if (!ctx) return;
    let pw = canvas.parentElement ? canvas.parentElement.clientWidth : 0;
    if (pw > 0 && Math.abs(canvas.width - pw) > 5) canvas.width = pw;
    let w = canvas.width;
    let h = canvas.height;

    ctx.clearRect(0, 0, w, h);

    if (!ovSpeedHistory || ovSpeedHistory.length < 2) return;

    let maxVal = 1.0;
    for (let pt of ovSpeedHistory) {
      if (pt.down > maxVal) maxVal = pt.down;
      if (pt.up > maxVal) maxVal = pt.up;
    }
    maxVal *= 1.25; // headroom

    let stepX = w / (ovSpeedHistory.length - 1);

    // Draw Download Curve (Emerald)
    ctx.beginPath();
    let ovDown = getToken('--green');
    ctx.strokeStyle = ovDown;
    ctx.lineWidth = 2.2;
    ctx.lineJoin = 'round';
    for (let i = 0; i < ovSpeedHistory.length; i++) {
      let x = i * stepX;
      let y = h - ((ovSpeedHistory[i].down / maxVal) * (h - 14)) - 6;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();

    // Solid single-tint fill without gradient
    if (ctx.closePath) {
      ctx.lineTo(w, h);
      ctx.lineTo(0, h);
      ctx.closePath();
      ctx.fillStyle = getToken('--chart-fill-down');
      ctx.fill();
    }

    // Draw Upload Curve (Cyan Accent)
    ctx.beginPath();
    let ovUp = getToken('--cyan');
    ctx.strokeStyle = ovUp;
    ctx.lineWidth = 2.0;
    ctx.lineJoin = 'round';
    for (let i = 0; i < ovSpeedHistory.length; i++) {
      let x = i * stepX;
      let y = h - ((ovSpeedHistory[i].up / maxVal) * (h - 14)) - 6;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
  } catch(e) {
    console.warn('drawOvSparkline error:', e);
  }
}
window.addEventListener('resize', drawOvSparkline);
setTimeout(drawOvSparkline, 200);

async function refreshData() {
  if (_actionInFlight || isOtaActive) return;
  try {
    let res = await fetch('/api/devices', { headers: getAuthHeaders() });
    if (!res.ok) return;
    let data = await res.json();

    isAdmin = !!data.is_admin;

    let authBtn = document.getElementById('auth-btn');
    if (authBtn) {
      if (isAdmin) {
        authBtn.className = 'auth-btn admin-active';
        authBtn.innerHTML = '<span>👑</span> <span>المشرف (تسجيل خروج)</span>';
        authBtn.title = 'أنت مسجل كمشرف كامل الصلاحيات. انقر لتسجيل الخروج والعودة لوضع المراقبة.';
      } else {
        authBtn.className = 'auth-btn';
        authBtn.innerHTML = '<span>👁️</span> <span>وضع المراقبة (دخول المشرف)</span>';
        authBtn.title = 'أنت في وضع المراقبة (عرض فقط). انقر لتسجيل الدخول كمشرف لتعديل الإعدادات.';
      }
    }

    // WAN Status
    let wanPulse = document.getElementById('wan-pulse');
    let wanText = document.getElementById('wan-text');
    if (data.wan) {
      wanPulse.className = 'pulse online';
      wanText.textContent = data.sta_ssid ? data.sta_ssid + ' (' + data.sta_rssi + ' dBm)' : 'متصل بالإنترنت';
    } else {
      wanPulse.className = 'pulse offline';
      wanText.textContent = 'غير متصل بالراوتر';
    }

    // Speeds
    let downMbps = (data.cur_down_bps / 1000000).toFixed(2);
    let upMbps = (data.cur_up_bps / 1000000).toFixed(2);
    document.getElementById('cur-down-mbps').textContent = downMbps + ' Mbps';
    document.getElementById('cur-down-raw').textContent = (data.cur_down_bps / 8000).toFixed(0) + ' KB/s';
    document.getElementById('cur-up-mbps').textContent = upMbps + ' Mbps';
    document.getElementById('cur-up-raw').textContent = (data.cur_up_bps / 8000).toFixed(0) + ' KB/s';

    speedHistoryDown.shift(); speedHistoryDown.push(data.cur_down_bps);
    speedHistoryUp.shift(); speedHistoryUp.push(data.cur_up_bps);
    drawSpeedChart();

    // Active Clients String
    let actCnt = data.active_clients || 0;
    document.getElementById('active-clients-str').textContent = actCnt + ' / 12 متصل';

    // Sync GB Standard & Meter Scale early so quota calculations use active standard
    if (data.meter_scale_pct) meterScalePct = data.meter_scale_pct;
    if (data.gb_standard) gbStandard = data.gb_standard;

    // Main Quota Summary & ISP Quota Live Monitor
    let qDiv = (gbStandard === 1000) ? 1000000000 : 1073741824;
    let usedGBNum = (data.total_up + data.total_down) / qDiv;
    let totalUsedGB = usedGBNum.toFixed(2);
    let mainCapGB = data.main_quota_gb > 0 ? data.main_quota_gb.toFixed(2) : '--';
    document.getElementById('quota-summary-val').textContent = totalUsedGB + ' / ' + mainCapGB + ' GB';
    let pct = (data.main_quota_gb > 0) ? Math.min(100, (usedGBNum / data.main_quota_gb) * 100) : 0;
    let bar = document.getElementById('quota-summary-bar');
    bar.style.width = pct + '%';
    if (pct > 90) bar.style.background = 'var(--red)';
    else if (pct >= 75) bar.style.background = 'var(--amber)';
    else bar.style.background = 'var(--accent-grad)';

    // Update Tab 2 ISP Quota Live Monitor Widget
    let ispUsedEl = document.getElementById('isp-used-txt');
    if (ispUsedEl) ispUsedEl.textContent = totalUsedGB + ' GB (' + pct.toFixed(1) + '%)';
    let ispBarEl = document.getElementById('isp-quota-bar');
    if (ispBarEl) {
      ispBarEl.style.width = pct + '%';
      if (pct > 90) ispBarEl.style.background = 'var(--red)';
      else if (pct >= 75) ispBarEl.style.background = 'var(--amber)';
      else ispBarEl.style.background = 'var(--green)';
    }
    let ispRemEl = document.getElementById('isp-rem-txt');
    if (ispRemEl) {
      if (data.main_quota_gb > 0) {
        let remGB = Math.max(0, data.main_quota_gb - usedGBNum).toFixed(2);
        ispRemEl.textContent = remGB + ' GB';
        ispRemEl.style.color = (pct > 90) ? 'var(--red)' : (pct >= 75 ? 'var(--amber)' : 'var(--green)');
      } else {
        ispRemEl.textContent = 'غير محدد';
        ispRemEl.style.color = 'var(--muted)';
      }
    }
    let ispTimeEl = document.getElementById('isp-time-txt');
    if (ispTimeEl) {
      if (data.q_end) {
        let endMs = new Date(data.q_end + 'T23:59:59').getTime();
        let diffMs = endMs - Date.now();
        if (!isNaN(diffMs)) {
          if (diffMs > 0) {
            let rDays = Math.floor(diffMs / 86400000);
            let rHours = Math.floor((diffMs % 86400000) / 3600000);
            ispTimeEl.textContent = rDays + ' يوم و ' + rHours + ' ساعة';
          } else {
            ispTimeEl.textContent = 'حان موعد التجديد';
          }
        } else {
          ispTimeEl.textContent = data.q_end;
        }
      } else {
        let totSec = (data.period_days || 30) * 86400;
        let remSec = Math.max(0, totSec - (data.elapsed_s || 0));
        let rDays = Math.floor(remSec / 86400);
        let rHours = Math.floor((remSec % 86400) / 3600);
        ispTimeEl.textContent = rDays + ' يوم و ' + rHours + ' ساعة';
      }
    }

    // System Metrics
    document.getElementById('free-heap-kb').textContent = Math.round(data.free_heap / 1024) + ' KB';
    document.getElementById('min-heap-kb').textContent = 'أدنى ذاكرة: ' + Math.round(data.min_free_heap / 1024) + ' KB (فائق الأداء)';
    let upDays = Math.floor(data.uptime_s / 86400);
    let upHours = Math.floor((data.uptime_s % 86400) / 3600);
    let upMins = Math.floor((data.uptime_s % 3600) / 60);
    document.getElementById('uptime-str').textContent = (upDays > 0 ? upDays + 'ي ' : '') + upHours + 'س ' + upMins + 'د';
    document.getElementById('cpu-clock-str').textContent = 'التردد: ' + data.cpu_freq_mhz + ' MHz (ESP32-C5 RISC-V)';
      if (typeof updateDashboardCpuFreq === 'function') updateDashboardCpuFreq(data.cpu_freq_mhz);
      let pSel = document.getElementById('cfg_cpu_freq');
      if (pSel && !document.activeElement?.isSameNode(pSel)) pSel.value = String(data.cpu_freq_mhz);
      let pChk = document.getElementById('cfg_wifi_ps');
      if (pChk && !document.activeElement?.isSameNode(pChk)) pChk.checked = !!data.wifi_power_save;
      if (typeof updatePresetUi === 'function') updatePresetUi(data.cpu_freq_mhz, !!data.wifi_power_save);

    // Parent Wi-Fi WAN Status
    if (document.getElementById('wan-ssid-val')) {
      document.getElementById('wan-ssid-val').textContent = data.sta_ssid || 'غير متصل';
    }
    if (document.getElementById('wan-info-sub')) {
      let bwText = data.current_bw ? ` | ${data.current_bw}MHz 🚀` : '';
      document.getElementById('wan-info-sub').textContent = data.sta_ssid ?
        ('الإشارة: ' + data.sta_rssi + ' dBm 📶 | IP: ' + (data.sta_ip || '--') + (data.current_band ? ' | ' + data.current_band + ' ⚡' : '') + bwText) : 'جاري الفحص...';
    }
    if (document.getElementById('parent-ssid-txt')) document.getElementById('parent-ssid-txt').textContent = data.sta_ssid || 'غير متصل';
    if (document.getElementById('parent-rssi-txt')) document.getElementById('parent-rssi-txt').textContent = data.sta_rssi ? (data.sta_rssi + ' dBm 📶') : '--';
    if (document.getElementById('parent-ip-txt')) document.getElementById('parent-ip-txt').textContent = data.sta_ip || '--';
    if (document.getElementById('parent-gw-txt')) document.getElementById('parent-gw-txt').textContent = (data.sta_gw || '--') + (data.sta_channel ? (' (قناة ' + data.sta_channel + (data.current_band ? ' - ' + data.current_band : '') + ')') : '');
    if (document.getElementById('parent-net-badge')) {
      document.getElementById('parent-net-badge').className = data.wan ? 'badge badge-online' : 'badge badge-blocked';
      document.getElementById('parent-net-badge').textContent = data.wan ? 'متصل بالإنترنت 🟢' : 'غير متصل 🔴';
    }

    // Populate Settings fields once if empty
    // Populate Static IP & WiFi fields
    if (data.sta_static_enabled !== undefined && !document.getElementById('static_toggle').dataset.loaded) {
      document.getElementById('static_toggle').checked = !!data.sta_static_enabled;
      document.getElementById('static_fields').style.display = data.sta_static_enabled ? 'block' : 'none';
      if (data.sta_ip) document.getElementById('cfg_sta_ip').value = data.sta_ip;
      if (data.sta_mask) document.getElementById('cfg_sta_mask').value = data.sta_mask;
      if (data.sta_gw) document.getElementById('cfg_sta_gw').value = data.sta_gw;
      if (data.sta_dns) document.getElementById('cfg_sta_dns').value = data.sta_dns;
      document.getElementById('static_toggle').dataset.loaded = '1';
    }

    if (!document.getElementById('cfg_router_ssid').value && data.router_ssid) {
      document.getElementById('cfg_router_ssid').value = data.router_ssid;
    }
    if (!document.getElementById('cfg_ap_ssid').value && data.ap_ssid) {
      document.getElementById('cfg_ap_ssid').value = data.ap_ssid;
    }
    if (!document.getElementById('cfg_main_quota_gb').value && data.main_quota_gb > 0) {
      document.getElementById('cfg_main_quota_gb').value = data.main_quota_gb;
      document.getElementById('cfg_start_date').value = data.q_start;
      document.getElementById('cfg_end_date').value = data.q_end;
    }
    if (data.meter_scale_pct) {
      meterScalePct = data.meter_scale_pct;
      let elSc = document.getElementById('cfg_meter_scale');
      if (elSc && !document.activeElement?.isSameNode(elSc)) elSc.value = String(data.meter_scale_pct);
    }
    if (data.gb_standard) {
      gbStandard = data.gb_standard;
      let elStd = document.getElementById('cfg_gb_std');
      if (elStd && !document.activeElement?.isSameNode(elStd)) elStd.value = String(data.gb_standard);
    }
    if (data.meter_scale_mode !== undefined) {
      meterScaleMode = data.meter_scale_mode;
      let elMode = document.getElementById('cfg_meter_mode');
      if (elMode && !document.activeElement?.isSameNode(elMode)) elMode.value = String(data.meter_scale_mode);
    }
    if (typeof updateMeterScaleBadges === 'function') {
      updateMeterScaleBadges(meterScalePct, gbStandard, meterScaleMode);
    }
    if (document.getElementById('cfg_wifi_protocol') && data.wifi_proto !== undefined && !document.getElementById('cfg_wifi_protocol').dataset.loaded) {
      document.getElementById('cfg_wifi_protocol').value = data.wifi_proto;
      document.getElementById('cfg_wifi_protocol').dataset.loaded = '1';
    }
    if (document.getElementById('cfg_wifi_band') && data.wifi_band !== undefined && !document.getElementById('cfg_wifi_band').dataset.loaded) {
      document.getElementById('cfg_wifi_band').value = data.wifi_band;
      document.getElementById('cfg_wifi_band').dataset.loaded = '1';
    }

    // Sync Daily Auto-Reboot settings from status
    if (data.rb_enable !== undefined && !document.getElementById('cfg_rb_enable').dataset.loaded) {
      document.getElementById('cfg_rb_enable').checked = !!data.rb_enable;
      if (data.rb_time) document.getElementById('cfg_rb_time').value = data.rb_time;
      if (data.rb_tz !== undefined) document.getElementById('cfg_rb_tz').value = String(data.rb_tz);
      document.getElementById('cfg_rb_enable').dataset.loaded = '1';
    }
    if (data.router_clock && document.getElementById('rb_router_clock')) {
      document.getElementById('rb_router_clock').textContent = data.router_clock;
    }
    if (data.time_synced !== undefined && document.getElementById('rb_sync_badge')) {
      let b = document.getElementById('rb_sync_badge');
      b.className = data.time_synced ? 'badge badge-online' : 'badge badge-offline';
      b.textContent = data.time_synced ? 'متزامن NTP 🟢' : 'غير متزامن 🔴';
    }

    // Hardware Boot & Power Diagnostics
    if (data.last_reset_reason) {
      let rTxt = document.getElementById('rst_reason_txt');
      let rBadge = document.getElementById('rst_reason_badge');
      let rAlert = document.getElementById('rst_brownout_alert');
      if (rTxt) rTxt.textContent = data.last_reset_reason;
      if (rBadge) {
        if (data.last_reset_code === 8) { // ESP_RST_BROWNOUT
          rBadge.className = 'badge badge-offline';
          rBadge.textContent = 'انخفاض جهد (Brownout) ⚠️';
          if (rAlert) rAlert.style.display = 'block';
        } else if (data.last_reset_code === 3 || data.last_reset_code === 4 || data.last_reset_code === 5) {
          rBadge.className = 'badge badge-offline';
          rBadge.textContent = 'كراش برمجي (WDT/Panic) 🔴';
          if (rAlert) rAlert.style.display = 'none';
        } else {
          rBadge.className = 'badge badge-online';
          rBadge.textContent = 'إقلاع سليم 🟢';
          if (rAlert) rAlert.style.display = 'none';
        }
      }
    }

    // Sync Telegram settings in both tabs (without overwriting unsaved user edits)
    if (data.tg_chat_id) {
      let c1 = document.getElementById('cfg_tg_chat_id');
      let c2 = document.getElementById('cfg_tg_chat_id_st');
      if (c1 && !document.activeElement?.isSameNode(c1) && !c1.dataset.dirty) c1.value = data.tg_chat_id;
      if (c2 && !document.activeElement?.isSameNode(c2) && !c2.dataset.dirty) c2.value = data.tg_chat_id;
    }
    if (data.tg_thresholds) {
      let t1 = document.getElementById('cfg_tg_thresholds');
      let t2 = document.getElementById('cfg_tg_thresholds_st');
      if (t1 && !document.activeElement?.isSameNode(t1) && !t1.dataset.dirty) t1.value = data.tg_thresholds;
      if (t2 && !document.activeElement?.isSameNode(t2) && !t2.dataset.dirty) t2.value = data.tg_thresholds;
    }
    if (data.tg_enable !== undefined) {
      let e1 = document.getElementById('cfg_tg_enable');
      let e2 = document.getElementById('cfg_tg_enable_st');
      if (e1 && !document.activeElement?.isSameNode(e1)) {
        if (e1.type === 'checkbox') e1.checked = !!data.tg_enable;
        else e1.value = data.tg_enable ? '1' : '0';
      }
      if (e2 && !document.activeElement?.isSameNode(e2)) {
        if (e2.type === 'checkbox') e2.checked = !!data.tg_enable;
        else e2.value = data.tg_enable ? '1' : '0';
      }
    }
    if (data.tg_interactive !== undefined) {
      let i1 = document.getElementById('cfg_tg_interactive');
      let i2 = document.getElementById('cfg_tg_interactive_st');
      if (i1 && !document.activeElement?.isSameNode(i1)) {
        if (i1.type === 'checkbox') i1.checked = !!data.tg_interactive;
        else i1.value = data.tg_interactive ? '1' : '0';
      }
      if (i2 && !document.activeElement?.isSameNode(i2)) {
        if (i2.type === 'checkbox') i2.checked = !!data.tg_interactive;
        else i2.value = data.tg_interactive ? '1' : '0';
      }
    }

    // Router Clock in Settings
    let syncBadge = document.getElementById('rb_sync_badge');
    let clockTxt = document.getElementById('rb_router_clock');
    if (data.time_synced) {
      syncBadge.className = 'badge badge-online';
      syncBadge.textContent = 'متزامن NTP';
      let nowLocal = new Date();
      clockTxt.textContent = nowLocal.toLocaleTimeString('ar-EG');
    } else {
      syncBadge.className = 'badge badge-offline';
      syncBadge.textContent = 'غير متزامن';
      clockTxt.textContent = 'جاري المزامنة...';
    }

    // OTA Firmware Info
    if (document.getElementById('ota-version-badge') && data.firmware_version) {
      document.getElementById('ota-version-badge').textContent = data.firmware_version;
    }
    if (document.getElementById('ota-running-bank') && data.running_partition) {
      document.getElementById('ota-running-bank').textContent = data.running_partition;
      document.getElementById('ota-target-bank').textContent = (data.running_partition === 'ota_0') ? 'ota_1' : 'ota_0';
    }
    if (document.getElementById('ota-build-date')) {
      document.getElementById('ota-build-date').textContent = new Date().toLocaleDateString('ar-EG');
    }
    if (typeof data.ota_auto !== 'undefined') {
      let chk = document.getElementById('cfg_ota_auto_update');
      let badge = document.getElementById('ota-auto-status-badge');
      if (chk && !chk.matches(':focus')) {
        chk.checked = !!data.ota_auto;
      }
      if (badge) {
        badge.textContent = data.ota_auto ? 'مفعّل ✅' : 'معطّل ⏸️';
        badge.className = data.ota_auto ? 'badge badge-gaming' : 'badge badge-secondary';
      }
    }

    renderDevices(data.devices || []);
    if (data.tg_users) {
      updateDiscoveredTgUsers(data.tg_users);
    }

    // UPDATE OVERVIEW TAB WITH LIVE REAL DATA!
    try {
      let devs = data.devices || [];
      let actDevs = devs.filter(d => d.connected);
      let vipDevs = devs.filter(d => (d.gaming || d.vip_gaming) && d.connected);
      let offDevs = devs.filter(d => !d.connected);
      let totalActive = Math.max(actDevs.length, data.active_clients || 0);
      if (typeof updateOverviewTab === 'function') {
        updateOverviewTab(data, downMbps, upMbps, totalActive, vipDevs.length, offDevs.length);
      }
    } catch(ovErr) {
      console.warn('Overview tab update error:', ovErr);
    }
  } catch (err) {
    console.warn('refreshData error:', err);
  }
}
