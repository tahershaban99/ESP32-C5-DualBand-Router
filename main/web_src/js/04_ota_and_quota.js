// -----------------------------------------------------------------------------
// SECURE OVER-THE-AIR (OTA) AUTOMATED UPDATE JAVASCRIPT
// -----------------------------------------------------------------------------
async function uploadOtaFirmware(e) {
  e.preventDefault();
  if (!isAdmin) { openLoginModal(); return; }
  let fileInput = document.getElementById('ota_file');
  if (!fileInput.files || fileInput.files.length === 0) {
    alert('يرجى اختيار ملف التحديث (.bin) أولاً');
    return;
  }
  let file = fileInput.files[0];
  if (!file.name.endsWith('.bin')) {
    if (!confirm('الملف المختار ليس بامتداد .bin، هل أنت متأكد من الاستمرار؟')) return;
  }

  let progressBox = document.getElementById('ota-progress-box');
  let progressBar = document.getElementById('ota-progress-bar');
  let progressTxt = document.getElementById('ota-progress-text');
  let statusTxt = document.getElementById('ota-status-msg');

  isOtaActive = true;
  progressBox.style.display = 'block';
  progressBar.style.width = '0%';
  progressTxt.textContent = '0%';
  statusTxt.textContent = 'جاري نقل الفيرموير ومسح البارتيشن الهدف...';

  let xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota/update', true);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  xhr.upload.onprogress = function(evt) {
    if (evt.lengthComputable) {
      let pct = Math.round((evt.loaded / evt.total) * 100);
      progressBar.style.width = pct + '%';
      progressTxt.textContent = pct + '%';
      statusTxt.textContent = `جاري كتابة الفيرموير بالفلاش: ${pct}% (${(evt.loaded/1024/1024).toFixed(2)} MB / ${(evt.total/1024/1024).toFixed(2)} MB)...`;
    }
  };

  xhr.onload = function() {
    isOtaActive = false;
    if (xhr.status === 200) {
      progressBar.style.width = '100%';
      progressTxt.textContent = '100%';
      statusTxt.innerHTML = '🎉 <b style="color:var(--green)">تم تثبيت التحديث بنجاح!</b> جاري إعادة تشغيل الراوتر الآن على البارتيشن الجديد...';
      setTimeout(() => {
        location.reload();
      }, 5000);
    } else {
      statusTxt.innerHTML = '❌ <b style="color:var(--red)">فشل التحديث: ' + xhr.responseText + '</b>';
    }
  };

  xhr.onerror = function() {
    isOtaActive = false;
    statusTxt.innerHTML = '❌ <b style="color:var(--red)">انقطع الاتصال أثناء الرفع</b>';
  };

  xhr.send(file);
}

async function triggerUrlOta(e) {
  e.preventDefault();
  if (!isAdmin) { openLoginModal(); return; }
  let url = document.getElementById('ota_remote_url').value.trim();
  if (!url) {
    alert('يرجى إدخال رابط التحديث أولاً');
    return;
  }

  let progressBox = document.getElementById('ota-progress-box');
  let progressBar = document.getElementById('ota-progress-bar');
  let progressTxt = document.getElementById('ota-progress-text');
  let statusTxt = document.getElementById('ota-status-msg');

  progressBox.style.display = 'block';
  progressBar.style.width = '5%';
  progressTxt.textContent = 'جاري البدء...';
  statusTxt.textContent = 'تم إرسال طلب الترقية، جاري الاتصال بالخادم وتنزيل الفيرموير...';

  try {
    let res = await fetch('/api/ota/update', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: new URLSearchParams({url: url})
    });
    let data = await res.json();
    if (data.ok) {
      pollOtaStatus();
    } else {
      statusTxt.innerHTML = '❌ <b style="color:var(--red)">فشل بدء التحديث السحابي</b>';
    }
  } catch(e) {
    statusTxt.innerHTML = '❌ <b style="color:var(--red)">خطأ في الاتصال بالراوتر</b>';
  }
}

function pollOtaStatus() {
  isOtaActive = true;
  let timer = setInterval(async () => {
    try {
      let res = await fetch('/api/ota/status');
      let st = await res.json();
      let progressBar = document.getElementById('ota-progress-bar');
      let progressTxt = document.getElementById('ota-progress-text');
      let statusTxt = document.getElementById('ota-status-msg');

      progressBar.style.width = st.progress + '%';
      progressTxt.textContent = st.progress + '%';

      if (st.state === 'downloading' || st.state === 'flashing') {
        statusTxt.textContent = `جاري التنزيل والبرمجة: ${st.progress}% (${(st.written/1024).toFixed(0)} KB)...`;
      } else if (st.state === 'success') {
        isOtaActive = false;
        clearInterval(timer);
        progressBar.style.width = '100%';
        statusTxt.innerHTML = '🎉 <b style="color:var(--green)">اكتملت الترقية بنجاح!</b> جاري إعادة التشغيل على البارتيشن الجديد...';
        setTimeout(() => location.reload(), 5000);
      } else if (st.state === 'failed') {
        isOtaActive = false;
        clearInterval(timer);
        statusTxt.innerHTML = '❌ <b style="color:var(--red)">فشل التحديث: ' + st.error + '</b>';
      }
    } catch(e) {}
  }, 1000);
}

async function checkCloudUpdates(btn) {
  let oldText = btn.innerHTML;
  btn.disabled = true;
  btn.innerHTML = '⏳ جاري الفحص...';
  let box = document.getElementById('cloud-update-result');
  box.style.display = 'block';
  box.innerHTML = '🔍 جاري الاتصال بالسحابة والتحقق من الإصدارات...';

  try {
    let res = await fetch('/api/ota/check');
    let data = await res.json();
    if (data.ok) {
      if (data.update_available) {
        box.style.border = '1px solid var(--green)';
        box.innerHTML = `
          <div style="color:var(--green);font-weight:700;font-size:1rem;margin-bottom:6px">🚀 يتوفر إصدار أحدث للراوتر (${data.latest_version})!</div>
          <div style="font-size:0.85rem;color:var(--text);margin-bottom:8px"><b>سجل التحديثات:</b> ${data.changelog || 'تحسينات في الأداء والاستقرار.'}</div>
          <button type="button" class="primary-btn" onclick="startAutoCloudOta('${data.firmware_url}')" style="background:var(--green);color:var(--btn-text);font-weight:700">⚡ تثبيت التحديث الهوائي الآن بنقرة واحدة</button>
        `;
      } else {
        box.style.border = '1px solid var(--accent)';
        box.innerHTML = `✅ <b style="color:var(--accent)">النظام محدث لأحدث إصدار مستقر (${data.current_version})</b>. لا توجد تحديثات جديدة حالياً.`;
      }
    } else {
      box.style.border = '1px solid var(--amber)';
      box.innerHTML = `⚠️ تعذر استرداد معلومات التحديث من السحابة. تأكد من اتصال الراوتر بالإنترنت (WAN).`;
    }
  } catch(e) {
    box.style.border = '1px solid var(--red)';
    box.innerHTML = `❌ خطأ في الاتصال أثناء التحقق من التحديثات.`;
  }
  btn.disabled = false;
  btn.innerHTML = oldText;
}

function startAutoCloudOta(url) {
  document.getElementById('ota_remote_url').value = url;
  triggerUrlOta({preventDefault: ()=>{}});
}

async function toggleOtaAutoUpdate(enabled) {
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) {
      openLoginModal();
      let chk = document.getElementById('cfg_ota_auto_update');
      if (chk) chk.checked = !enabled;
      return;
    }
  }
  let badge = document.getElementById('ota-auto-status-badge');
  try {
    let res = await fetch('/api/ota/update', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded', ...getAuthHeaders()},
      body: new URLSearchParams({auto_update: enabled ? '1' : '0'})
    });
    let d = await res.json();
    if (d.ok) {
      if (badge) {
        badge.textContent = enabled ? 'مفعّل ✅' : 'معطّل ⏸️';
        badge.className = enabled ? 'badge badge-gaming' : 'badge badge-secondary';
      }
    } else {
      alert('تعذر حفظ إعداد التحديث التلقائي');
      let chk = document.getElementById('cfg_ota_auto_update');
      if (chk) chk.checked = !enabled;
    }
  } catch(e) {
    alert('خطأ في الاتصال بالراوتر أثناء حفظ إعداد التحديث');
    let chk = document.getElementById('cfg_ota_auto_update');
    if (chk) chk.checked = !enabled;
  }
}

// Main Quota & Period
async function saveMainQuotaConfig(btn) {
  let gb = document.getElementById('cfg_main_quota_gb').value;
  let sDate = document.getElementById('cfg_start_date').value;
  let eDate = document.getElementById('cfg_end_date').value;
  btn.disabled = true;
  let res = await postWithRetry('/api/main_quota', {quota_gb: gb, start_date: sDate, end_date: eDate});
  btn.disabled = false;
  if (res.ok) {
    showToast('تم حفظ إعدادات باقة النت بنجاح! 💾');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => saveMainQuotaConfig(btn);
    openLoginModal('يرجى تسجيل الدخول كمشرف لتعديل إعدادات الباقة');
  } else {
    showToast('تعذر حفظ الإعدادات', 'error');
  }
}

async function submitPeriodDays(e) {
  e.preventDefault();
  let d = document.getElementById('period_days').value;
  let res = await postWithRetry('/meter/period', {period_days: d});
  if (res.ok) {
    showToast('تم تطبيق مدة الدورة الاحتياطية بنجاح!');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => submitPeriodDays(e);
    openLoginModal('يرجى تسجيل الدخول كمشرف لتعديل مدة الدورة');
  } else {
    showToast('تعذر حفظ مدة الدورة', 'error');
  }
}

async function resetMainQuota() {
  if (!confirm('تحذير: سيتم تصفير جميع العدادات وإعادة بدء الدورة ورفع الحظر. هل أنت متأكد؟')) return;
  let res = await postWithRetry('/meter/reset', {});
  if (res.ok) {
    showToast('تم تصفير العدادات وتجديد الدورة بنجاح!');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => resetMainQuota();
    openLoginModal('يرجى تسجيل الدخول كمشرف لتصفير العدادات');
  } else {
    showToast('تعذر تصفير العدادات', 'error');
  }
}
