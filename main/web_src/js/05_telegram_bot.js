// Telegram Helpers & Interactive Chat
function escapeHtml(str) {
  if (!str) return '';
  return String(str).replace(/[&<>"']/g, function(m) {
    return {'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[m];
  });
}

function formatTimeNow() {
  let now = new Date();
  let h = now.getHours();
  let m = String(now.getMinutes()).padStart(2, '0');
  let ampm = h >= 12 ? 'م' : 'ص';
  h = h % 12 || 12;
  return `${h}:${m} ${ampm}`;
}

function appendUserChatMessage(text) {
  let container = document.getElementById('tg_chat_history');
  if (!container) return;
  let bubble = document.createElement('div');
  bubble.className = 'tg-bubble tg-bubble-user';
  bubble.innerHTML = `<div>${escapeHtml(text)}</div><span class="tg-bubble-time">${formatTimeNow()}</span>`;
  container.appendChild(bubble);
  container.scrollTop = container.scrollHeight;
}

function appendBotChatMessage(htmlContent) {
  let container = document.getElementById('tg_chat_history');
  if (!container) return;
  let bubble = document.createElement('div');
  bubble.className = 'tg-bubble tg-bubble-bot';
  bubble.innerHTML = `${htmlContent}<span class="tg-bubble-time">${formatTimeNow()}</span>`;
  container.appendChild(bubble);
  container.scrollTop = container.scrollHeight;
}

let tgTypingCounter = 0;
function appendTypingIndicator() {
  let container = document.getElementById('tg_chat_history');
  if (!container) return null;
  tgTypingCounter++;
  let id = 'tg_typing_' + tgTypingCounter;
  let bubble = document.createElement('div');
  bubble.id = id;
  bubble.className = 'tg-bubble tg-bubble-bot';
  bubble.style.cssText = 'color:var(--muted);font-size:0.8rem;padding:6px 12px;font-style:italic';
  bubble.innerHTML = '🤖 جاري معالجة الأمر...';
  container.appendChild(bubble);
  container.scrollTop = container.scrollHeight;
  return id;
}

function removeTypingIndicator(id) {
  if (!id) return;
  let el = document.getElementById(id);
  if (el) el.remove();
}

async function sendInteractiveBotCmd(customCmd) {
  let input = document.getElementById('tg_sim_input');
  let cmd = customCmd || (input ? input.value : '');
  if (cmd) cmd = cmd.trim();
  if (!cmd) return;
  if (input && !customCmd) input.value = '';

  appendUserChatMessage(cmd);
  let typId = appendTypingIndicator();

  await new Promise(r => setTimeout(r, 350));

  try {
    let lower = cmd.toLowerCase().trim();

    if (lower === '/quota' || lower === 'استهلاك' || lower === 'باقة' || lower === 'باقي' || lower === 'كوتا') {
      let qStr = document.getElementById('main-quota-metric')?.textContent || 'غير محدد';
      let period = document.getElementById('period_info')?.textContent || 'الدورة نشطة';
      let downSpeed = document.getElementById('cur-down-mbps')?.textContent || '0.00';
      let upSpeed = document.getElementById('cur-up-mbps')?.textContent || '0.00';
      appendBotChatMessage(
        `<div style="color:var(--cyan);font-weight:800;margin-bottom:6px">📊 إحصائيات باقة الإنترنت الحالية 🌐</div>` +
        `<div>📥 <b>استهلاك الباقة:</b> ${qStr}</div>` +
        `<div>⏱️ <b>بيانات الدورة:</b> ${period}</div>` +
        `<div>⚡ <b>معدل التدفق الحالي:</b> ↓ ${downSpeed} Mbps | ↑ ${upSpeed} Mbps</div>`
      );
    } else if (lower === '/devices' || lower === 'الأجهزة' || lower === 'اجهزة' || lower === 'devices' || lower === 'المتصلين') {
      let res = await fetch('/api/devices', {headers: getAuthHeaders()});
      let data = await res.json();
      let devs = (data && data.devices) ? data.devices.filter(d => d.online) : [];
      if (!devs.length) {
        appendBotChatMessage(`ℹ️ لا توجد أجهزة متصلة بالراوتر حالياً.`);
      } else {
        let rows = devs.map(d => {
          let name = d.name || 'جهاز بدون اسم';
          let used = formatBytes(d.usage_total || (d.rx_bytes + d.tx_bytes));
          let blockStr = d.blocked ? '🔴 محظور' : '🟢 متصل';
          let vipStr = d.vip ? '⭐ VIP' : '';
          return `• <b>${escapeHtml(name)}</b> (<code>${d.ip}</code>)<br>&nbsp;&nbsp;الحالة: ${blockStr} ${vipStr} | المستهلك: ${used}`;
        }).join('<br>');
        appendBotChatMessage(
          `<div style="color:var(--green);font-weight:800;margin-bottom:6px">📱 الأجهزة المتصلة حالياً (${devs.length}):</div>` + rows
        );
      }
    } else if (lower === '/speed' || lower === 'سرعة' || lower === 'السرعة') {
      let d = document.getElementById('cur-down-mbps')?.textContent || '0.00';
      let u = document.getElementById('cur-up-mbps')?.textContent || '0.00';
      let pSsid = document.getElementById('parent-ssid-txt')?.textContent || '--';
      let pRssi = document.getElementById('parent-rssi-txt')?.textContent || '--';
      appendBotChatMessage(
        `<div style="color:var(--accent);font-weight:800;margin-bottom:6px">🚀 سرعة نقل البيانات اللحظية:</div>` +
        `<div>📥 <b>تحميل (Download):</b> ${d} Mbps</div>` +
        `<div>📤 <b>رفع (Upload):</b> ${u} Mbps</div>` +
        `<div>📶 <b>إشارة المصدر:</b> ${pSsid} (${pRssi})</div>`
      );
    } else if (lower.startsWith('/block ') || lower.startsWith('حظر ')) {
      let targetIp = cmd.split(' ')[1]?.trim();
      if (!targetIp) {
        appendBotChatMessage(`⚠️ يرجى تحديد الـ IP، مثال: <code>/block 192.168.4.2</code>`);
      } else {
        let res = await fetch('/api/device/block', {
          method: 'POST',
          headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
          body: new URLSearchParams({ip: targetIp, block: '1'})
        });
        if (res.ok) {
          appendBotChatMessage(`🚫 <b>تم حظر الجهاز بنجاح!</b> تم قطع الإنترنت عن <code>${targetIp}</code>.`);
          showToast(`تم حظر الجهاز ${targetIp}`);
          refreshData();
        } else {
          appendBotChatMessage(`❌ تعذر حظر الجهاز ${targetIp}. تأكد من صحة الـ IP.`);
        }
      }
    } else if (lower.startsWith('/unblock ') || lower.startsWith('فك حظر ') || lower.startsWith('فك_حظر ')) {
      let targetIp = cmd.split(' ')[1]?.trim();
      if (!targetIp) {
        appendBotChatMessage(`⚠️ يرجى تحديد الـ IP، مثال: <code>/unblock 192.168.4.2</code>`);
      } else {
        let res = await fetch('/api/device/block', {
          method: 'POST',
          headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
          body: new URLSearchParams({ip: targetIp, block: '0'})
        });
        if (res.ok) {
          appendBotChatMessage(`✅ <b>تم فك حظر الجهاز بنجاح!</b> تم استعادة الإنترنت لـ <code>${targetIp}</code>.`);
          showToast(`تم فك حظر الجهاز ${targetIp}`);
          refreshData();
        } else {
          appendBotChatMessage(`❌ تعذر فك حظر الجهاز ${targetIp}.`);
        }
      }
    } else if (lower === '/status' || lower === 'حالة' || lower === 'الحالة') {
      let up = document.getElementById('uptime-metric')?.textContent || '--';
      let ram = document.getElementById('free-heap-metric')?.textContent || '--';
      let parentSsid = document.getElementById('parent-ssid-txt')?.textContent || '--';
      let parentRssi = document.getElementById('parent-rssi-txt')?.textContent || '--';
      appendBotChatMessage(
        `<div style="color:var(--cyan);font-weight:800;margin-bottom:6px">⚙️ حالة نظام الراوتر (ESP32-C5 Dual-Band)</div>` +
        `<div>⏱️ <b>مدة التشغيل:</b> ${up}</div>` +
        `<div>🧠 <b>الذاكرة المتاحة (RAM):</b> ${ram}</div>` +
        `<div>📡 <b>الشبكة المزودة (WAN):</b> ${parentSsid} (${parentRssi})</div>` +
        `<div>🛡️ <b>جدار الحماية والتسريع:</b> NAPT Hardware Acceleration مفعل</div>`
      );
    } else if (lower === '/ota' || lower === 'تحديث') {
      let v = document.getElementById('ota-version-badge')?.textContent || 'v2.0-C5';
      let b = document.getElementById('ota-running-bank')?.textContent || 'ota_0';
      appendBotChatMessage(
        `<div style="color:var(--cyan);font-weight:800;margin-bottom:6px">🚀 معلومات النظام والتحديث الهوائي (Dual-Bank OTA)</div>` +
        `<div>🔖 <b>الإصدار الحالي:</b> ${v}</div>` +
        `<div>💾 <b>القسم النشط:</b> ${b}</div>` +
        `<div>☁️ <b>جاهزية التحديث:</b> جاهز لاستقبال ملفات الفيرموير الآمنة.</div>`
      );
    } else {
      appendBotChatMessage(
        `🤖 <b>الأوامر الإدارية المتاحة:</b><br>` +
        `• <code>/quota</code> - عرض رصيد واستهلاك باقة الإنترنت<br>` +
        `• <code>/devices</code> - الأجهزة المتصلة وسرعاتها<br>` +
        `• <code>/speed</code> - قياس السرعة اللحظية للرفع والتحميل<br>` +
        `• <code>/block &lt;IP&gt;</code> - حظر جهاز محدد وقطع النت عنه<br>` +
        `• <code>/unblock &lt;IP&gt;</code> - فك حظر جهاز واستعادة الإنترنت<br>` +
        `• <code>/status</code> - عرض حالة الراوتر والذاكرة والمعالج<br>` +
        `• <code>/ota</code> - فحص إصدار الفيرموير وقسم الإقلاع`
      );
    }
  } catch (err) {
    appendBotChatMessage(`⚠️ حدث خطأ أثناء تنفيذ الأمر.`);
  } finally {
    removeTypingIndicator(typId);
  }
}

async function submitTelegramSettings(e, isSt = false) {
  if (e) e.preventDefault();
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  let en = '1';
  let inter = '1';
  let tok = '';
  let cid = '';
  let th = '';

  if (isSt) {
    en = document.getElementById('cfg_tg_enable_st')?.checked ? '1' : '0';
    inter = document.getElementById('cfg_tg_interactive_st')?.checked ? '1' : '0';
    tok = (document.getElementById('cfg_tg_token_st')?.value || '').trim();
    cid = (document.getElementById('cfg_tg_chat_id_st')?.value || '').trim();
    th = (document.getElementById('cfg_tg_thresholds_st')?.value || '').trim();
  } else {
    let e1 = document.getElementById('cfg_tg_enable');
    en = (e1 && e1.type === 'checkbox') ? (e1.checked ? '1' : '0') : (e1 ? e1.value : '1');
    let i1 = document.getElementById('cfg_tg_interactive');
    inter = (i1 && i1.type === 'checkbox') ? (i1.checked ? '1' : '0') : (i1 ? i1.value : '1');
    tok = (document.getElementById('cfg_tg_token')?.value || document.getElementById('cfg_tg_token_st')?.value || '').trim();
    cid = (document.getElementById('cfg_tg_chat_id')?.value || document.getElementById('cfg_tg_chat_id_st')?.value || '').trim();
    th = (document.getElementById('cfg_tg_thresholds')?.value || document.getElementById('cfg_tg_thresholds_st')?.value || '').trim();
  }

  // Sync inputs across tabs
  if (document.getElementById('cfg_tg_chat_id')) document.getElementById('cfg_tg_chat_id').value = cid;
  if (document.getElementById('cfg_tg_chat_id_st')) document.getElementById('cfg_tg_chat_id_st').value = cid;
  if (document.getElementById('cfg_tg_thresholds')) document.getElementById('cfg_tg_thresholds').value = th;
  if (document.getElementById('cfg_tg_thresholds_st')) document.getElementById('cfg_tg_thresholds_st').value = th;

  let btn = document.getElementById('tg_save_btn');
  if (btn) btn.disabled = true;
  try {
    let res = await fetch('/api/telegram', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({enable: en, interactive: inter, token: tok, chat_id: cid, thresholds: th})
    });
    let data = await res.json().catch(() => ({}));
    if (res.ok && data.ok) {
      showToast(data.msg || 'تم حفظ وتفعيل إعدادات التيليجرام بنجاح! 💾');
      appendBotChatMessage('⚙️ <b>تم حفظ وتفعيل إعدادات التيليجرام بنجاح!</b> بوت الراوتر جاهز لاستقبال الأوامر وإرسال التنبيهات.');
    } else {
      showToast(data.msg || 'تعذر حفظ إعدادات التيليجرام', 'error');
    }
  } catch (e) {
    showToast('خطأ في الاتصال بالراوتر', 'error');
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function testTelegramAlert(btn) {
  if (!isAdmin) {
    let ok = await ensureAdminAuth();
    if (!ok) { openLoginModal(); return; }
  }
  if (btn) btn.disabled = true;
  let old = btn ? btn.textContent : '';
  if (btn) btn.textContent = 'جاري الإرسال...';

  let tok = (document.getElementById('cfg_tg_token')?.value || document.getElementById('cfg_tg_token_st')?.value || '').trim();
  let cid = (document.getElementById('cfg_tg_chat_id')?.value || document.getElementById('cfg_tg_chat_id_st')?.value || '').trim();
  let th = (document.getElementById('cfg_tg_thresholds')?.value || document.getElementById('cfg_tg_thresholds_st')?.value || '').trim();

  appendUserChatMessage('🧪 اختبار إرسال تنبيه تجريبي إلى التيليجرام');
  let typId = appendTypingIndicator();

  try {
    let res = await fetch('/api/telegram', {
      method: 'POST',
      headers: getAuthHeaders({'Content-Type': 'application/x-www-form-urlencoded'}),
      body: new URLSearchParams({test: '1', token: tok, chat_id: cid, thresholds: th})
    });
    let data = await res.json().catch(() => ({}));
    removeTypingIndicator(typId);

    if (res.ok && data.ok) {
      showToast(data.msg || 'تم إرسال الرسالة التجريبية إلى التيليجرام بنجاح! تفقد محادثتك.');
      appendBotChatMessage('✅ <b>نجاح الإرسال:</b><br>' + (data.msg || 'تم إرسال رسالة التنبيه التجريبية بنجاح إلى حساب التيليجرام الخاص بك عبر السيرفر. تفقد تطبيق تيليجرام لديك!'));
    } else {
      let errMsg = data.msg || 'فشل الإرسال: تأكد من صحة التوكن والـ Chat ID واتصال الراوتر بالإنترنت.';
      showToast(errMsg, 'error');
      appendBotChatMessage('❌ <b>فشل في إرسال التنبيه:</b><br>' + errMsg);
    }
  } catch (e) {
    removeTypingIndicator(typId);
    showToast('خطأ في الاتصال بالراوتر', 'error');
    appendBotChatMessage('⚠️ خطأ في الاتصال بالراوتر، يرجى المحاولة لاحقاً.');
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = old;
    }
  }
}

// Discovered Telegram Users & Multi-Admin Management
function updateDiscoveredTgUsers(tgUsers) {
  if (!Array.isArray(tgUsers)) return;
  window._lastTgUsers = tgUsers;
  let dl = document.getElementById('tg_users_datalist');
  if (dl) {
    dl.innerHTML = tgUsers.map(u => {
      let label = u.name || '';
      if (u.username) label += ` (@${u.username})`;
      return `<option value="${u.chat_id}">${label} [${u.chat_id}]</option>`;
    }).join('');
  }

  let box = document.getElementById('tg_discovered_users_list');
  if (box) {
    if (tgUsers.length === 0) {
      box.innerHTML = '<div style="color:var(--muted);font-size:0.8rem;text-align:center;padding:8px">لم يدخل أي مستخدم على البوت بعد. عندما يرسل أي شخص رسالة للبوت ستظهر هويته هنا فوراً لتعيينه كمشرف أو لربط جهازه بسهولة.</div>';
    } else {
      box.innerHTML = tgUsers.map(u => {
        let name = u.name || 'مستخدم';
        let un = u.username ? `@${u.username}` : 'بدون معرف';
        return `
          <div style="display:flex;align-items:center;justify-content:space-between;padding:8px 12px;background:var(--bg-subtle);border:1px solid var(--border-subtle);border-radius:8px;margin-bottom:6px;gap:8px;flex-wrap:wrap">
            <div style="display:flex;flex-direction:column">
              <span style="font-weight:700;font-size:0.85rem;color:var(--text)">👤 ${name} <span style="font-weight:normal;color:var(--accent);font-size:0.78rem">(${un})</span></span>
              <span style="font-size:0.75rem;color:var(--muted);direction:ltr;text-align:left">Chat ID: <code style="color:var(--cyan)">${u.chat_id}</code></span>
            </div>
            <div style="display:flex;gap:6px">
              <button type="button" class="action-btn btn-gaming" style="padding:4px 9px;font-size:0.75rem" onclick="addTgAdmin('${u.chat_id}')" title="إضافة هذا المعرف لقائمة المشرفين">👑 تعيين كمشرف</button>
              <button type="button" class="primary-btn" style="padding:4px 9px;font-size:0.75rem" onclick="navigator.clipboard.writeText('${u.chat_id}');showToast('تم نسخ Chat ID: ' + '${u.chat_id}')" title="نسخ المعرف">📋 نسخ</button>
            </div>
          </div>
        `;
      }).join('');
    }
  }
}

function addTgAdmin(chatId) {
  let inp = document.getElementById('cfg_tg_chat_id');
  let inpSt = document.getElementById('cfg_tg_chat_id_st');
  if (!inp) inp = inpSt;
  if (!inp) return;
  let cur = inp.value.trim();
  if (!cur) {
    inp.value = chatId;
  } else {
    let ids = cur.split(',').map(s => s.trim()).filter(Boolean);
    if (!ids.includes(chatId)) {
      ids.push(chatId);
      inp.value = ids.join(', ');
    } else {
      showToast('هذا المعرف مضاف مسبقاً كمشرف.');
      return;
    }
  }
  if (inpSt) inpSt.value = inp.value;
  showToast('تمت إضافة ' + chatId + ' إلى قائمة المشرفين! اضغط على "حفظ الإعدادات" لتطبيق التغيير.');
}
