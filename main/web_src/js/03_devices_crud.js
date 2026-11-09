function renderDevices(devices) {
  // 1. Guarantee 100% unique devices by physical MAC to eliminate ghost duplicates
  let seen = new Set();
  let uniqueDevs = [];
  (devices || []).forEach(d => {
    let key = (d.canon_mac || d.mac || '').toUpperCase();
    if (!key || seen.has(key)) return;
    seen.add(key);
    uniqueDevs.push(d);
  });
  devices = uniqueDevs;

  document.getElementById('dev-count-txt').textContent = devices.length;

  let connectedDevs = devices.filter(d => d.connected);
  let offlineDevs = devices.filter(d => !d.connected);

  let connBadge = document.getElementById('connected-count-badge');
  if (connBadge) connBadge.textContent = `${connectedDevs.length} متصل`;

  let offBadge = document.getElementById('offline-count-badge');
  if (offBadge) offBadge.textContent = `${offlineDevs.length} أجهزة`;

  let toggleTxt = document.getElementById('offline-toggle-text');
  if (toggleTxt) toggleTxt.textContent = `${offlineSectionVisible ? 'إخفاء' : 'عرض'} الأجهزة غير المتصلة (${offlineDevs.length} أجهزة)`;

  let tbody = document.getElementById('devices-tbody');
  let cardsContainer = document.getElementById('cards-container');
  let isEditing = document.activeElement && (document.activeElement.tagName === 'INPUT' || document.activeElement.tagName === 'SELECT');
  const isTableMode = (viewMode === 'table');

  if (isTableMode) {
    let tableMatches = tbody && (tbody.children.length === connectedDevs.length) &&
        connectedDevs.every(d => document.getElementById('tbl_row_' + (d.canon_mac || d.mac)));

    // Fast In-Place Update: If rows already exist, update dynamic metrics without destroying DOM or wiping inputs!
    if (tableMatches) {
      connectedDevs.forEach(dev => {
        let canonMac = dev.canon_mac || dev.mac;
        let tr = document.getElementById('tbl_row_' + canonMac);
        if (!tr) return;

        let quotaGB = dev.quota_gb !== undefined ? dev.quota_gb : (dev.quota > 0 ? (dev.quota / 1000000000).toFixed(1) : 0);
        let quotaPct = dev.quota_pct !== undefined ? dev.quota_pct : (dev.quota > 0 ? (((dev.up + dev.down) / dev.quota) * 100) : 0);
        let quotaExceeded = dev.quota_exceeded !== undefined ? dev.quota_exceeded : (dev.quota > 0 && (dev.up + dev.down) >= dev.quota);
        let isBlocked = !!dev.blocked;
        let isGaming = !!dev.gaming;
        let quotaBarColor = quotaPct >= 100 ? 'var(--red)' : quotaPct >= 80 ? 'var(--amber)' : 'var(--green)';

        let statusKey = `${isBlocked}_${dev.connected}_${isGaming}`;
        if (tr.dataset.statusKey !== statusKey) {
          tr.dataset.statusKey = statusKey;
          let elStatus = tr.querySelector('.cell-status');
          if (elStatus) {
            elStatus.innerHTML = `
              <div style="display:flex;align-items:center;justify-content:center;gap:6px">
                <span class="status-dot ${isBlocked ? 'blocked' : (dev.connected ? 'online' : 'offline')}"></span>
                <span style="font-size:0.75rem;font-weight:700;color:${isBlocked ? 'var(--red)' : (dev.connected ? 'var(--green)' : 'var(--muted)')}">
                  ${isBlocked ? 'محظور' : (dev.connected ? 'متصل' : 'غير متصل')}
                </span>
                ${isGaming ? '<span title="وضع VIP للألعاب" style="font-size:0.75rem">👑</span>' : ''}
              </div>
            `;
          }
        }

        let elConnTime = tr.querySelector('.cell-conn-time');
        if (elConnTime) elConnTime.textContent = formatDuration(dev.conn_s);

        let elDownSpd = tr.querySelector('.cell-down-spd');
        if (elDownSpd) elDownSpd.textContent = formatLiveSpeed(dev.cur_down_bps);
        let elUpSpd = tr.querySelector('.cell-up-spd');
        if (elUpSpd) elUpSpd.textContent = formatLiveSpeed(dev.cur_up_bps);

        let elDownBytes = tr.querySelector('.cell-down-bytes');
        if (elDownBytes) elDownBytes.textContent = formatSmartBytes(dev.down);
        let elUpBytes = tr.querySelector('.cell-up-bytes');
        if (elUpBytes) elUpBytes.textContent = formatSmartBytes(dev.up);

        let elTotBytes = tr.querySelector('.cell-tot-bytes');
        if (elTotBytes) {
          elTotBytes.textContent = formatSmartBytes(dev.up + dev.down);
          elTotBytes.style.color = quotaBarColor;
        }
        let elBar = tr.querySelector('.quota-mini-fill');
        if (elBar) {
          elBar.style.width = Math.min(100, quotaPct) + '%';
          elBar.style.background = quotaBarColor;
        }
        let elPct = tr.querySelector('.cell-quota-pct');
        if (elPct) {
          elPct.textContent = quotaGB > 0 ? (quotaPct.toFixed(0) + '%') : 'مفتوح ∞';
          elPct.style.color = quotaBarColor;
        }

        // Safeguard: Never overwrite inputs if focused or modified by user!
        let qInput = document.getElementById('tbl_q_' + canonMac);
        if (qInput && document.activeElement !== qInput && !qInput.dataset.dirty) qInput.value = quotaGB;
        let dlInput = document.getElementById('tbl_dl_' + canonMac);
        if (dlInput && document.activeElement !== dlInput && !dlInput.dataset.dirty) dlInput.value = dev.download_mbps || 0;
        let ulInput = document.getElementById('tbl_ul_' + canonMac);
        if (ulInput && document.activeElement !== ulInput && !ulInput.dataset.dirty) ulInput.value = dev.upload_mbps || 0;
        let nInput = document.getElementById('tbl_n_' + canonMac);
        if (nInput && document.activeElement !== nInput && !nInput.dataset.dirty) nInput.value = dev.name || 'جهاز متصل';
        let tgInput = document.getElementById('tbl_tg_' + canonMac);
        if (tgInput && document.activeElement !== tgInput && !tgInput.dataset.dirty) tgInput.value = dev.telegram_chat_id || '';

        // Visual indicator on speed limit inputs when VIP is active
        if (dlInput) {
          dlInput.title = isGaming ? `الحد مضبوط على ${dev.download_mbps || 0} Mbps (مُعطل مؤقتاً بالـ VIP ويعود فور إلغائه)` : 'أقصى سرعة تحميل Mbps';
          if (isGaming) dlInput.style.borderColor = 'var(--amber)';
          else dlInput.style.borderColor = '';
        }
        if (ulInput) {
          ulInput.title = isGaming ? `الحد مضبوط على ${dev.upload_mbps || 0} Mbps (مُعطل مؤقتاً بالـ VIP ويعود فور إلغائه)` : 'أقصى سرعة رفع Mbps';
          if (isGaming) ulInput.style.borderColor = 'var(--amber)';
          else ulInput.style.borderColor = '';
        }

        // Dynamic Action Buttons state update for Table
        if (tr.dataset.gamingState !== String(isGaming)) {
          tr.dataset.gamingState = String(isGaming);
          let btnGaming = tr.querySelector('.btn-gaming');
          if (btnGaming) {
            btnGaming.setAttribute('onclick', `toggleGaming('${canonMac}', ${!isGaming})`);
            if (isGaming) {
              btnGaming.classList.add('active');
              btnGaming.innerHTML = '👑';
              btnGaming.title = 'إلغاء وضع VIP للألعاب';
            } else {
              btnGaming.classList.remove('active');
              btnGaming.innerHTML = '🎮';
              btnGaming.title = 'تفعيل وضع VIP للألعاب';
            }
          }
        }

        if (tr.dataset.blockedState !== String(isBlocked)) {
          tr.dataset.blockedState = String(isBlocked);
          let btnBlock = tr.querySelector('.btn-block, .btn-unblock');
          if (btnBlock) {
            btnBlock.setAttribute('onclick', `toggleBlock('${canonMac}', ${!isBlocked})`);
            if (isBlocked) {
              btnBlock.className = 'tbl-icon-btn btn-unblock';
              btnBlock.innerHTML = '🔓';
              btnBlock.title = 'الجهاز محظور حالياً (انقر لفك الحظر)';
            } else {
              btnBlock.className = 'tbl-icon-btn btn-block';
              btnBlock.innerHTML = '⛔';
              btnBlock.title = 'حظر هذا الجهاز عن الإنترنت';
            }
          }
        }
      });
    } else if (!isEditing) {
    let tableHtml = '';
    connectedDevs.forEach(dev => {
      let name = dev.name || 'جهاز متصل';
      let ip = dev.ip || '--';
      let mac = dev.mac || '--';
      let canonMac = dev.canon_mac || mac;
      let isGaming = !!dev.gaming;
      let isBlocked = !!dev.blocked;
      let quotaGB = dev.quota_gb !== undefined ? dev.quota_gb : (dev.quota > 0 ? (dev.quota / 1000000000).toFixed(1) : 0);
      let quotaPct = dev.quota_pct !== undefined ? dev.quota_pct : (dev.quota > 0 ? (((dev.up + dev.down) / dev.quota) * 100) : 0);
      let quotaExceeded = dev.quota_exceeded !== undefined ? dev.quota_exceeded : (dev.quota > 0 && (dev.up + dev.down) >= dev.quota);
      let upLimit = dev.upload_mbps !== undefined ? dev.upload_mbps : 0;
      let downLimit = dev.download_mbps !== undefined ? dev.download_mbps : 0;
      let tgChat = dev.telegram_chat_id || '';
      let quotaBarColor = quotaPct >= 100 ? 'var(--red)' : quotaPct >= 80 ? 'var(--amber)' : 'var(--green)';

      tableHtml += `
        <tr id="tbl_row_${canonMac}">
          <!-- 1. الحالة (7%) -->
          <td class="cell-status text-center">
            <div style="display:flex;align-items:center;justify-content:center;gap:6px">
              <span class="status-dot ${isBlocked ? 'blocked' : (dev.connected ? 'online' : 'offline')}"></span>
              <span style="font-size:0.75rem;font-weight:700;color:${isBlocked ? 'var(--red)' : (dev.connected ? 'var(--green)' : 'var(--muted)')}">
                ${isBlocked ? 'محظور' : (dev.connected ? 'متصل' : 'غير متصل')}
              </span>
              ${isGaming ? '<span title="وضع VIP للألعاب" style="font-size:0.75rem">👑</span>' : ''}
            </div>
          </td>

          <!-- 2. اسم الجهاز (16%) -->
          <td>
            <div class="tbl-device-info">
              <div class="tbl-device-icon" title="${name}">
                ${getDeviceIcon(name)}
              </div>
              <div class="tbl-device-text">
                <input class="tbl-device-name-input" value="${name}" id="tbl_n_${canonMac}" 
                       oninput="this.dataset.dirty='1'"
                       onchange="saveDeviceName('${canonMac}', this.value)" 
                       onkeydown="if(event.key==='Enter') this.blur()" 
                       title="انقر لتعديل اسم الجهاز ثم اضغط Enter">
                <div class="tbl-device-type">Wi-Fi • 2.4/5GHz</div>
              </div>
            </div>
          </td>

          <!-- 3. مدة الاتصال (8%) -->
          <td class="text-center">
            <div class="tbl-time-pill connected" title="مدة اتصال الجهاز">
              <span class="tbl-time-icon">⏱️</span>
              <span class="tbl-time-val cell-conn-time tabular-nums">${formatDuration(dev.conn_s)}</span>
            </div>
          </td>

          <!-- 4. IP / MAC (10%) -->
          <td class="text-center">
            <div class="tbl-net-info">
              <div class="tbl-ip-addr" title="عنوان IP">${ip}</div>
              <div class="tbl-mac-addr" title="عنوان MAC">${mac}</div>
            </div>
          </td>

          <!-- 5. السرعة اللحظية (13%) -->
          <td>
            <div class="tbl-speed-group">
              <div class="tbl-speed-pill down" title="سرعة التحميل اللحظية">
                <span>↓ تنزيل</span>
                <span class="tbl-speed-val cell-down-spd tabular-nums">${formatLiveSpeed(dev.cur_down_bps)}</span>
              </div>
              <div class="tbl-speed-pill up" title="سرعة الرفع اللحظية">
                <span>↑ رفع</span>
                <span class="tbl-speed-val cell-up-spd tabular-nums">${formatLiveSpeed(dev.cur_up_bps)}</span>
              </div>
            </div>
          </td>

          <!-- 6. الباقة والاستهلاك (14%) -->
          <td>
            <div class="tbl-quota-wrapper">
              <div class="tbl-quota-labels">
                <span class="tabular-nums" style="font-size:0.75rem">
                  المستهلك: <b class="cell-tot-bytes" style="color:${quotaBarColor}">${formatSmartBytes(dev.up + dev.down)}</b>
                </span>
                <div style="display:inline-flex;align-items:center;gap:2px">
                  <input type="number" step="0.5" min="0" value="${quotaGB}" id="tbl_q_${canonMac}" 
                         oninput="this.dataset.dirty='1'"
                         onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')"
                         style="width:40px;height:22px;padding:1px 2px;font-size:0.74rem;font-weight:700;text-align:center;border-radius:4px;border:1px solid var(--input-border);background:var(--input-bg);color:var(--input-text)" title="كوتة الجهاز بالجيجابايت (0 = غير محدود)">
                  <span style="font-size:0.68rem;color:var(--text-secondary)">GB</span>
                </div>
              </div>
              <div class="tbl-progress-track">
                <div class="tbl-progress-fill quota-mini-fill" style="width:${Math.min(100, quotaPct)}%;background:${quotaBarColor}"></div>
              </div>
              <div style="display:flex;justify-content:space-between;align-items:center;font-size:0.68rem;color:var(--text-secondary)">
                <span class="cell-quota-pct tabular-nums" style="color:${quotaBarColor};font-weight:700">${quotaGB > 0 ? quotaPct.toFixed(0) + '%' : 'مفتوح ∞'}</span>
                <span>⬇️ <span class="cell-down-bytes">${formatSmartBytes(dev.down)}</span> | ⬆️ <span class="cell-up-bytes">${formatSmartBytes(dev.up)}</span></span>
              </div>
            </div>
          </td>

          <!-- 7. تحديد السرعة (12%) -->
          <td class="text-center">
            <div class="tbl-limit-group">
              <div class="tbl-limit-box down" title="أقصى سرعة تحميل Mbps">
                <span style="color:var(--tbl-dl-color);font-size:0.68rem;font-weight:700">⬇️</span>
                <input type="number" step="0.5" min="0" value="${downLimit}" id="tbl_dl_${canonMac}" 
                       class="tbl-limit-input"
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')">
              </div>
              <div class="tbl-limit-box up" title="أقصى سرعة رفع Mbps">
                <span style="color:var(--tbl-ul-color);font-size:0.68rem;font-weight:700">⬆️</span>
                <input type="number" step="0.5" min="0" value="${upLimit}" id="tbl_ul_${canonMac}" 
                       class="tbl-limit-input"
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')">
              </div>
            </div>
            <div style="font-size:0.65rem;color:var(--text-secondary);margin-top:2px">${(downLimit > 0 || upLimit > 0) ? 'Mbps محدد' : 'مفتوح ∞'}</div>
          </td>

          <!-- 8. معرف التيليجرام (8%) -->
          <td class="text-center">
            <input type="text" list="tg_users_datalist" value="${tgChat}" id="tbl_tg_${canonMac}" 
                   oninput="this.dataset.dirty='1'"
                   onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')"
                   style="width:78px;height:24px;padding:1px 4px;direction:ltr;text-align:center;font-size:0.75rem;border-radius:5px;border:1px solid var(--tbl-tg-border);background:var(--surface-overlay);color:var(--tbl-tg-color)" 
                   placeholder="Chat ID" title="معرف التيليجرام">
          </td>

          <!-- 9. الإجراءات (17%) -->
          <td class="text-center">
            <div class="tbl-actions-group">
              <button class="tbl-icon-btn btn-save" onclick="saveDeviceTable('${canonMac}')" title="حفظ التعديلات 💾">💾</button>
              <button class="tbl-icon-btn btn-gaming ${isGaming ? 'active' : ''}" onclick="toggleGaming('${canonMac}', ${!isGaming})" title="${isGaming ? 'إلغاء وضع VIP' : 'تفعيل وضع VIP للألعاب'}">${isGaming ? '👑' : '🎮'}</button>
              <button class="tbl-icon-btn ${isBlocked ? 'btn-unblock' : 'btn-block'}" onclick="toggleBlock('${canonMac}', ${!isBlocked})" title="${isBlocked ? 'فك الحظر' : 'حظر الجهاز'}">${isBlocked ? '🔓' : '⛔'}</button>
              <button class="tbl-icon-btn btn-reset" onclick="resetDevice('${canonMac}')" title="تصفير العداد">🔄</button>
              <button class="tbl-icon-btn btn-delete" onclick="deleteDevice('${canonMac}')" title="حذف">🗑️</button>
            </div>
          </td>
        </tr>
      `;
    });
    if (tbody) tbody.innerHTML = tableHtml;
  }
  } else {
    // Cards rendering / updating (Only executed in Cards view mode)
    let cardsMatch = cardsContainer && (cardsContainer.children.length === connectedDevs.length) &&
        connectedDevs.every(d => document.getElementById('card_' + (d.canon_mac || d.mac)));

    if (cardsMatch) {
      connectedDevs.forEach(dev => {
        let canonMac = dev.canon_mac || dev.mac;
        let card = document.getElementById('card_' + canonMac);
        if (!card) return;

        let quotaGB = dev.quota_gb !== undefined ? dev.quota_gb : (dev.quota > 0 ? (dev.quota / 1000000000).toFixed(1) : 0);
        let quotaPct = dev.quota_pct !== undefined ? dev.quota_pct : (dev.quota > 0 ? (((dev.up + dev.down) / dev.quota) * 100) : 0);
        let quotaBarColor = quotaPct >= 100 ? 'var(--red)' : quotaPct >= 80 ? 'var(--amber)' : 'var(--green)';
        let quotaBarWidth = Math.min(quotaPct, 100);
        let quotaText = quotaGB > 0 ? `${formatBytes(dev.up + dev.down)} / ${quotaGB} GB (${quotaPct.toFixed(0)}%)` : `${formatBytes(dev.up + dev.down)} (غير محدود ♾️)`;

        let elDown = card.querySelector('.card-down');
        if (elDown) elDown.textContent = formatBytes(dev.down);
        let elUp = card.querySelector('.card-up');
        if (elUp) elUp.textContent = formatBytes(dev.up);
        let elTot = card.querySelector('.card-tot');
        if (elTot) elTot.textContent = formatBytes(dev.up + dev.down);
        let elSpd = card.querySelector('.card-live-spd');
        if (elSpd) elSpd.innerHTML = `السرعة اللحظية: ⬇️ ${(dev.cur_down_bps/1000000).toFixed(2)} Mbps | ⬆️ ${(dev.cur_up_bps/1000000).toFixed(2)} Mbps`;

        let qInput = document.getElementById('card_q_' + canonMac);
        if (qInput && document.activeElement !== qInput && !qInput.dataset.dirty) qInput.value = quotaGB;
        let dlInput = document.getElementById('card_dl_' + canonMac);
        if (dlInput && document.activeElement !== dlInput && !dlInput.dataset.dirty) dlInput.value = dev.download_mbps || 0;
        let ulInput = document.getElementById('card_ul_' + canonMac);
        if (ulInput && document.activeElement !== ulInput && !ulInput.dataset.dirty) ulInput.value = dev.upload_mbps || 0;
        let tgInput = document.getElementById('card_tg_' + canonMac);
        if (tgInput && document.activeElement !== tgInput && !tgInput.dataset.dirty) tgInput.value = dev.telegram_chat_id || '';

        let isGaming = !!dev.gaming;
        let isBlocked = !!dev.blocked;
        let quotaExceeded = dev.quota_exceeded !== undefined ? dev.quota_exceeded : (dev.quota > 0 && (dev.up + dev.down) >= dev.quota);

        card.classList.toggle('gaming', isGaming);
        card.classList.toggle('blocked', isBlocked);
        card.classList.toggle('quota-exceeded', quotaExceeded);

        let statusBadge = '';
        if (quotaExceeded) statusBadge = '<span class="badge badge-blocked">⚠️ تجاوز الكوتة</span>';
        else if (isBlocked) statusBadge = '<span class="badge badge-blocked">⛔ محظور</span>';
        else statusBadge = '<span class="badge badge-online">🟢 متصل</span>';
        if (isGaming) statusBadge += ' <span class="badge badge-gaming">👑 VIP ألعاب</span>';

        let headBadge = card.querySelector('.dev-card-head > div:last-child');
        if (headBadge) headBadge.innerHTML = statusBadge;

        let cardBtnGaming = card.querySelector('.btn-gaming');
        if (cardBtnGaming) {
          cardBtnGaming.setAttribute('onclick', `toggleGaming('${canonMac}', ${!isGaming})`);
          if (isGaming) {
            cardBtnGaming.classList.add('btn-gaming-active');
            cardBtnGaming.innerHTML = '👑 إلغاء VIP';
            cardBtnGaming.title = 'وضع VIP نشط (انقر للإلغاء والعودة للسرعة المحددة)';
          } else {
            cardBtnGaming.classList.remove('btn-gaming-active');
            cardBtnGaming.innerHTML = '🎮 ألعاب VIP';
            cardBtnGaming.title = 'تفعيل وضع الألعاب VIP (سرعة كاملة وأولوية Ping)';
          }
        }

        let cardBtnBlock = card.querySelector('.btn-block, .btn-unblock');
        if (cardBtnBlock) {
          cardBtnBlock.setAttribute('onclick', `toggleBlock('${canonMac}', ${!isBlocked})`);
          if (isBlocked) {
            cardBtnBlock.className = 'action-btn btn-unblock';
            cardBtnBlock.innerHTML = '🔓 فك الحظر';
            cardBtnBlock.title = 'الجهاز محظور حالياً (انقر لفك الحظر)';
          } else {
            cardBtnBlock.className = 'action-btn btn-block';
            cardBtnBlock.innerHTML = '⛔ حظر';
            cardBtnBlock.title = 'حظر هذا الجهاز عن الإنترنت';
          }
        }

        if (dlInput) {
          dlInput.title = isGaming ? `الحد مضبوط على ${dev.download_mbps || 0} Mbps (مُعطل مؤقتاً بالـ VIP ويعود فور إلغائه)` : 'أقصى سرعة تحميل Mbps';
          if (isGaming) dlInput.style.borderColor = 'var(--amber)';
          else dlInput.style.borderColor = '';
        }
        if (ulInput) {
          ulInput.title = isGaming ? `الحد مضبوط على ${dev.upload_mbps || 0} Mbps (مُعطل مؤقتاً بالـ VIP ويعود فور إلغائه)` : 'أقصى سرعة رفع Mbps';
          if (isGaming) ulInput.style.borderColor = 'var(--amber)';
          else ulInput.style.borderColor = '';
        }
      });
    } else if (!isEditing) {
      let cardsHtml = '';
      connectedDevs.forEach(dev => {
        let name = dev.name || 'جهاز متصل';
        let ip = dev.ip || '--';
        let mac = dev.mac || '--';
        let canonMac = dev.canon_mac || mac;
        let isGaming = !!dev.gaming;
        let isBlocked = !!dev.blocked;
        let quotaGB = dev.quota_gb !== undefined ? dev.quota_gb : (dev.quota > 0 ? (dev.quota / 1000000000).toFixed(1) : 0);
        let quotaPct = dev.quota_pct !== undefined ? dev.quota_pct : (dev.quota > 0 ? (((dev.up + dev.down) / dev.quota) * 100) : 0);
        let quotaExceeded = dev.quota_exceeded !== undefined ? dev.quota_exceeded : (dev.quota > 0 && (dev.up + dev.down) >= dev.quota);
        let upLimit = dev.upload_mbps !== undefined ? dev.upload_mbps : 0;
        let downLimit = dev.download_mbps !== undefined ? dev.download_mbps : 0;
        let tgChat = dev.telegram_chat_id || '';

        let statusBadge = '';
        if (quotaExceeded) statusBadge = '<span class="badge badge-blocked">⚠️ تجاوز الكوتة</span>';
        else if (isBlocked) statusBadge = '<span class="badge badge-blocked">⛔ محظور</span>';
        else statusBadge = '<span class="badge badge-online">🟢 متصل</span>';
        if (isGaming) statusBadge += ' <span class="badge badge-gaming">👑 VIP ألعاب</span>';

        let quotaBarColor = quotaPct >= 100 ? 'var(--red)' : quotaPct >= 80 ? 'var(--amber)' : 'var(--green)';
        let quotaBarWidth = Math.min(quotaPct, 100);
        let quotaText = quotaGB > 0 ? `${formatBytes(dev.up + dev.down)} / ${quotaGB} GB (${quotaPct.toFixed(0)}%)` : `${formatBytes(dev.up + dev.down)} (غير محدود ♾️)`;

        cardsHtml += `
          <div class="dev-card glass ${isBlocked ? 'blocked' : ''} ${quotaExceeded ? 'quota-exceeded' : ''} ${isGaming ? 'gaming' : ''}" id="card_${canonMac}">
            <div class="dev-card-head">
              <div>
                <div class="dev-title">${name}</div>
                <div class="dev-sub">${ip} | ${mac}</div>
              </div>
              <div>${statusBadge}</div>
            </div>
            <div class="stats-box">
              <div><div class="lbl">تحميل</div><div class="val card-down" style="color:var(--green)">${formatBytes(dev.down)}</div></div>
              <div><div class="lbl">رفع</div><div class="val card-up" style="color:var(--accent)">${formatBytes(dev.up)}</div></div>
              <div><div class="lbl">الإجمالي</div><div class="val card-tot" style="color:var(--cyan)">${formatBytes(dev.up + dev.down)}</div></div>
            </div>
            <div class="quota-box" style="margin-top:8px">
              <div style="display:flex;justify-content:space-between;margin-bottom:4px;font-size:0.78rem">
                <span>📊 كوتة الجهاز: <b>${quotaText}</b></span>
                <span>حصة الباقة: ${dev.share_pct ? dev.share_pct.toFixed(1) : 0}%</span>
              </div>
              ${quotaGB > 0 ? `
              <div class="progress-bg" style="height:8px"><div class="progress-fill" style="width:${quotaBarWidth}%;background:${quotaBarColor}"></div></div>
              ` : ''}
              ${quotaExceeded ? '<div style="color:var(--red);font-weight:700;margin-top:4px;font-size:0.75rem">⛔ تم تجاوز الكوتة وحظر النت عن الجهاز تلقائياً</div>' : ''}
              <div class="card-live-spd" style="margin-top:6px;font-size:0.76rem;color:var(--text)">
                السرعة اللحظية: ⬇️ ${(dev.cur_down_bps/1000000).toFixed(2)} Mbps | ⬆️ ${(dev.cur_up_bps/1000000).toFixed(2)} Mbps
              </div>
            </div>

            <div class="dev-controls-grid" style="grid-template-columns: repeat(auto-fit, minmax(100px, 1fr));">
              <div>
                <label>كوتة (GB):</label>
                <input type="number" step="0.5" min="0" value="${quotaGB}" id="card_q_${canonMac}" 
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                       title="0 = غير محدود">
              </div>
              <div>
                <label>⬇️ حد التحميل:</label>
                <input type="number" step="0.5" min="0" value="${downLimit}" id="card_dl_${canonMac}" 
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                       placeholder="Mbps" style="${isGaming ? 'border-color:var(--amber)' : ''}"
                       title="${isGaming ? 'الحد مضبوط على ' + downLimit + ' Mbps (مُعطل مؤقتاً بالـ VIP ويعود فور إلغائه)' : 'أقصى سرعة تحميل Mbps'}">
              </div>
              <div>
                <label>⬆️ حد الرفع:</label>
                <input type="number" step="0.5" min="0" value="${upLimit}" id="card_ul_${canonMac}" 
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                       placeholder="Mbps" style="${isGaming ? 'border-color:var(--amber)' : ''}"
                       title="${isGaming ? 'الحد مضبوط على ' + upLimit + ' Mbps (مُعطل مؤقتاً بالـ VIP ويعود فور إلغائه)' : 'أقصى سرعة رفع Mbps'}">
              </div>
              <div>
                <label>معرف التيليجرام:</label>
                <input type="text" list="tg_users_datalist" value="${tgChat}" id="card_tg_${canonMac}" 
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                       placeholder="Chat ID" style="direction:ltr;text-align:center" title="معرف محادثة التيليجرام الخاص بمالك الجهاز (اختر من القائمة المكتشفة)">
              </div>
            </div>
            <button type="button" class="primary-btn" style="width:100%;padding:6px 10px;font-size:0.8rem;margin-bottom:8px" onclick="saveDeviceCard('${canonMac}')">💾 حفظ الكوتة والسرعة</button>

            <div class="btn-row">
              <button type="button" class="action-btn btn-gaming ${isGaming ? 'btn-gaming-active' : ''}" onclick="toggleGaming('${canonMac}', ${!isGaming})" title="${isGaming ? 'وضع VIP نشط (انقر للإلغاء والعودة للسرعة المحددة)' : 'تفعيل وضع الألعاب VIP (أقصى سرعة وأولوية Ping)'}">
                ${isGaming ? '👑 إلغاء VIP' : '🎮 ألعاب VIP'}
              </button>
              <button type="button" class="action-btn ${isBlocked ? 'btn-unblock' : 'btn-block'}" onclick="toggleBlock('${canonMac}', ${!isBlocked})" title="${isBlocked ? 'الجهاز محظور حالياً (انقر لفك الحظر)' : 'حظر هذا الجهاز عن الإنترنت'}">
                ${isBlocked ? '🔓 فك الحظر' : '⛔ حظر'}
              </button>
              <button type="button" class="action-btn btn-reset" onclick="resetDevice('${canonMac}')">🔄 تصفير</button>
              <button type="button" class="action-btn btn-delete" onclick="deleteDevice('${canonMac}')">🗑️ حذف</button>
            </div>
          </div>
        `;
      });
      if (cardsContainer) cardsContainer.innerHTML = cardsHtml;
    }
  }

  // 2. OFFLINE / PREVIOUSLY CONNECTED DEVICES (Only rendered/queried when offline section is expanded)
  if (offlineSectionVisible) {
    if (isTableMode) {
      let otb = document.getElementById('offline-devices-tbody');
      let offTableMatches = otb && (otb.children.length === offlineDevs.length) &&
          offlineDevs.every(d => document.getElementById('off_tbl_row_' + (d.canon_mac || d.mac)));

      if (offTableMatches) {
        offlineDevs.forEach(dev => {
          let canonMac = dev.canon_mac || dev.mac;
          let tr = document.getElementById('off_tbl_row_' + canonMac);
          if (!tr) return;
          let qInput = document.getElementById('off_tbl_q_' + canonMac);
          if (qInput && document.activeElement !== qInput && !qInput.dataset.dirty) qInput.value = dev.quota_gb || 0;
          let dlInput = document.getElementById('off_tbl_dl_' + canonMac);
          if (dlInput && document.activeElement !== dlInput && !dlInput.dataset.dirty) dlInput.value = dev.download_mbps || 0;
          let ulInput = document.getElementById('off_tbl_ul_' + canonMac);
          if (ulInput && document.activeElement !== ulInput && !ulInput.dataset.dirty) ulInput.value = dev.upload_mbps || 0;
          let tgInput = document.getElementById('off_tbl_tg_' + canonMac);
          if (tgInput && document.activeElement !== tgInput && !tgInput.dataset.dirty) tgInput.value = dev.telegram_chat_id || '';
          let nInput = document.getElementById('off_tbl_n_' + canonMac);
          if (nInput && document.activeElement !== nInput && !nInput.dataset.dirty) nInput.value = dev.name || 'جهاز مسجل';

          let elLastSeen = tr.querySelector('.cell-last-seen');
          if (elLastSeen) elLastSeen.textContent = formatLastSeen(dev.last_seen_ago_s);

          let isBlocked = !!dev.blocked;
          if (tr.dataset.blockedState !== String(isBlocked)) {
            tr.dataset.blockedState = String(isBlocked);
            let btnBlock = tr.querySelector('.btn-block, .btn-unblock');
            if (btnBlock) {
              btnBlock.setAttribute('onclick', `toggleBlock('${canonMac}', ${!isBlocked})`);
              if (isBlocked) {
                btnBlock.className = 'tbl-icon-btn btn-unblock';
                btnBlock.innerHTML = '🔓';
                btnBlock.title = 'فك الحظر المسبق';
              } else {
                btnBlock.className = 'tbl-icon-btn btn-block';
                btnBlock.innerHTML = '⛔';
                btnBlock.title = 'حظر مسبق';
              }
            }
          }
        });
      } else if (!isEditing) {
        let offTableHtml = '';
        offlineDevs.forEach(dev => {
          let name = dev.name || 'جهاز مسجل';
          let ip = dev.ip || '--';
          let mac = dev.mac || '--';
          let canonMac = dev.canon_mac || mac;
          let isBlocked = !!dev.blocked;
          let quotaGB = dev.quota_gb !== undefined ? dev.quota_gb : (dev.quota > 0 ? (dev.quota / 1000000000).toFixed(1) : 0);
          let quotaPct = dev.quota_pct !== undefined ? dev.quota_pct : (dev.quota > 0 ? (((dev.up + dev.down) / dev.quota) * 100) : 0);
          let upLimit = dev.upload_mbps !== undefined ? dev.upload_mbps : 0;
          let downLimit = dev.download_mbps !== undefined ? dev.download_mbps : 0;
          let tgChat = dev.telegram_chat_id || '';

          let quotaBarColor = quotaPct >= 100 ? 'var(--red)' : quotaPct >= 80 ? 'var(--amber)' : 'var(--muted)';

          offTableHtml += `
            <tr id="off_tbl_row_${canonMac}">
              <!-- 1. الحالة (7%) -->
              <td class="cell-status text-center">
                <div style="display:flex;align-items:center;justify-content:center;gap:6px">
                  <span class="status-dot ${isBlocked ? 'blocked' : 'offline'}"></span>
                  <span style="font-size:0.75rem;font-weight:700;color:${isBlocked ? 'var(--red)' : 'var(--muted)'}">
                    ${isBlocked ? 'محظور' : 'غير متصل'}
                  </span>
                </div>
              </td>

              <!-- 2. اسم الجهاز (16%) -->
              <td>
                <div class="tbl-device-info">
                  <div class="tbl-device-icon" style="opacity:0.75" title="${name}">
                    ${getDeviceIcon(name)}
                  </div>
                  <div class="tbl-device-text">
                    <input class="tbl-device-name-input" value="${name}" id="off_tbl_n_${canonMac}" 
                           oninput="this.dataset.dirty='1'"
                           onchange="saveDeviceName('${canonMac}', this.value)" 
                           onkeydown="if(event.key==='Enter') this.blur()" 
                           style="color:var(--text-secondary)"
                           title="اسم الجهاز المسجل (انقر للتعديل)">
                    <div class="tbl-device-type">مسجل مسبقاً</div>
                  </div>
                </div>
              </td>

              <!-- 3. مدة الاتصال / آخر ظهور (8%) -->
              <td class="text-center">
                <div class="tbl-time-pill offline" title="آخر وقت شوهد فيه الجهاز">
                  <span class="tbl-time-icon">⌛</span>
                  <span class="tbl-time-val cell-last-seen tabular-nums">${formatLastSeen(dev.last_seen_ago_s)}</span>
                </div>
              </td>

              <!-- 4. IP / MAC (10%) -->
              <td class="text-center">
                <div class="tbl-net-info">
                  <div class="tbl-ip-addr" style="color:var(--text-secondary)" title="آخر عنوان IP">${ip}</div>
                  <div class="tbl-mac-addr" title="عنوان MAC">${mac}</div>
                </div>
              </td>

              <!-- 5. الاستهلاك السابق (13%) -->
              <td>
                <div class="tbl-speed-group">
                  <div class="tbl-speed-pill down" style="opacity:0.85" title="إجمالي التحميل السابق">
                    <span>↓ تنزيل</span>
                    <span class="tbl-speed-val tabular-nums">${formatSmartBytes(dev.down)}</span>
                  </div>
                  <div class="tbl-speed-pill up" style="opacity:0.85" title="إجمالي الرفع السابق">
                    <span>↑ رفع</span>
                    <span class="tbl-speed-val tabular-nums">${formatSmartBytes(dev.up)}</span>
                  </div>
                </div>
              </td>

              <!-- 6. الكوتة المسبقة (14%) -->
              <td>
                <div class="tbl-quota-wrapper">
                  <div class="tbl-quota-labels">
                    <span class="tabular-nums" style="font-size:0.75rem">
                      السابق: <b class="cell-tot-bytes" style="color:${quotaBarColor}">${formatSmartBytes(dev.up + dev.down)}</b>
                    </span>
                    <div style="display:inline-flex;align-items:center;gap:2px">
                      <input type="number" step="0.5" min="0" value="${quotaGB}" id="off_tbl_q_${canonMac}" 
                             oninput="this.dataset.dirty='1'"
                             onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')"
                             style="width:40px;height:22px;padding:1px 2px;font-size:0.74rem;font-weight:700;text-align:center;border-radius:4px;border:1px solid var(--input-border);background:var(--input-bg);color:var(--input-text)" title="كوتة الجهاز بالجيجابايت (0 = غير محدود)">
                      <span style="font-size:0.68rem;color:var(--text-secondary)">GB</span>
                    </div>
                  </div>
                  <div class="tbl-progress-track">
                    <div class="tbl-progress-fill quota-mini-fill" style="width:${Math.min(100, quotaPct)}%;background:${quotaBarColor}"></div>
                  </div>
                  <div style="display:flex;justify-content:space-between;align-items:center;font-size:0.68rem;color:var(--text-secondary)">
                    <span class="tabular-nums" style="color:${quotaBarColor};font-weight:700">${quotaGB > 0 ? quotaPct.toFixed(0) + '%' : 'مفتوح ∞'}</span>
                    <span>حصة الباقة: ${dev.share_pct ? dev.share_pct.toFixed(1) : 0}%</span>
                  </div>
                </div>
              </td>

              <!-- 7. تحديد السرعة (12%) -->
              <td class="text-center">
                <div class="tbl-limit-group">
                  <div class="tbl-limit-box down" title="أقصى سرعة تحميل Mbps">
                    <span style="color:var(--tbl-dl-color);font-size:0.68rem;font-weight:700">⬇️</span>
                    <input type="number" step="0.5" min="0" value="${downLimit}" id="off_tbl_dl_${canonMac}" 
                           class="tbl-limit-input"
                           oninput="this.dataset.dirty='1'"
                           onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')">
                  </div>
                  <div class="tbl-limit-box up" title="أقصى سرعة رفع Mbps">
                    <span style="color:var(--tbl-ul-color);font-size:0.68rem;font-weight:700">⬆️</span>
                    <input type="number" step="0.5" min="0" value="${upLimit}" id="off_tbl_ul_${canonMac}" 
                           class="tbl-limit-input"
                           oninput="this.dataset.dirty='1'"
                           onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')">
                  </div>
                </div>
                <div style="font-size:0.65rem;color:var(--text-secondary);margin-top:2px">${(downLimit > 0 || upLimit > 0) ? 'Mbps محدد' : 'مفتوح ∞'}</div>
              </td>

              <!-- 8. معرف التيليجرام (8%) -->
              <td class="text-center">
                <input type="text" list="tg_users_datalist" value="${tgChat}" id="off_tbl_tg_${canonMac}" 
                       oninput="this.dataset.dirty='1'"
                       onkeydown="if(event.key==='Enter') saveDeviceTable('${canonMac}')"
                       style="width:78px;height:24px;padding:1px 4px;direction:ltr;text-align:center;font-size:0.75rem;border-radius:5px;border:1px solid var(--tbl-tg-border);background:var(--surface-overlay);color:var(--tbl-tg-color)" 
                       placeholder="Chat ID" title="معرف التيليجرام">
              </td>

              <!-- 9. الإجراءات (17%) -->
              <td class="text-center">
                <div class="tbl-actions-group">
                  <button class="tbl-icon-btn btn-save" onclick="saveDeviceTable('${canonMac}')" title="حفظ التعديلات 💾">💾</button>
                  <button class="tbl-icon-btn ${isBlocked ? 'btn-unblock' : 'btn-block'}" onclick="toggleBlock('${canonMac}', ${!isBlocked})" title="${isBlocked ? 'فك الحظر المسبق' : 'حظر مسبق'}">${isBlocked ? '🔓' : '⛔'}</button>
                  <button class="tbl-icon-btn btn-reset" onclick="resetDevice('${canonMac}')" title="تصفير العداد">🔄</button>
                  <button class="tbl-icon-btn btn-delete" onclick="deleteDevice('${canonMac}')" title="حذف من السجل">🗑️</button>
                </div>
              </td>
            </tr>
          `;
        });
        if (otb) otb.innerHTML = offTableHtml;
      }
    } else {
      // Offline Cards mode
      let occ = document.getElementById('offline-cards-container');
      let offCardsMatch = occ && (occ.children.length === offlineDevs.length) &&
          offlineDevs.every(d => document.getElementById('off_card_' + (d.canon_mac || d.mac)));

      if (offCardsMatch) {
        offlineDevs.forEach(dev => {
          let canonMac = dev.canon_mac || dev.mac;
          let card = document.getElementById('off_card_' + canonMac);
          if (!card) return;
          let isBlocked = !!dev.blocked;
          let btnBlock = card.querySelector('.btn-block, .btn-unblock');
          if (btnBlock) {
            btnBlock.setAttribute('onclick', `toggleBlock('${canonMac}', ${!isBlocked})`);
            btnBlock.className = isBlocked ? 'action-btn btn-unblock' : 'action-btn btn-block';
            btnBlock.textContent = isBlocked ? 'إلغاء الحظر المسبق' : 'حظر مسبق';
          }
        });
      } else if (!isEditing) {
        let offCardsHtml = '';
        offlineDevs.forEach(dev => {
          let name = dev.name || 'جهاز مسجل';
          let ip = dev.ip || '--';
          let mac = dev.mac || '--';
          let canonMac = dev.canon_mac || mac;
          let isBlocked = !!dev.blocked;
          let quotaGB = dev.quota_gb !== undefined ? dev.quota_gb : (dev.quota > 0 ? (dev.quota / 1000000000).toFixed(1) : 0);
          let quotaPct = dev.quota_pct !== undefined ? dev.quota_pct : (dev.quota > 0 ? (((dev.up + dev.down) / dev.quota) * 100) : 0);
          let upLimit = dev.upload_mbps !== undefined ? dev.upload_mbps : 0;
          let downLimit = dev.download_mbps !== undefined ? dev.download_mbps : 0;

          let statusBadge = isBlocked ? '<span class="badge badge-blocked">⛔ محظور مسبقاً</span>' : '<span class="badge badge-offline">⚪ غير متصل</span>';
          let quotaBarColor = quotaPct >= 100 ? 'var(--red)' : quotaPct >= 80 ? 'var(--amber)' : 'var(--muted)';
          let quotaBarWidth = Math.min(quotaPct, 100);
          let quotaText = quotaGB > 0 ? `${formatBytes(dev.up + dev.down)} / ${quotaGB} GB (${quotaPct.toFixed(0)}%)` : `${formatBytes(dev.up + dev.down)}`;

          offCardsHtml += `
            <div class="dev-card glass ${isBlocked ? 'blocked' : ''}" style="opacity:0.9;border-style:dashed" id="off_card_${canonMac}">
              <div class="dev-card-head">
                <div>
                  <div class="dev-title" style="color:var(--muted)">${name}</div>
                  <div class="dev-sub">آخر IP: ${ip} | ${mac}</div>
                </div>
                <div>${statusBadge}</div>
              </div>
              <div class="stats-box">
                <div><div class="lbl">تحميل سابق</div><div class="val" style="color:var(--green)">${formatBytes(dev.down)}</div></div>
                <div><div class="lbl">رفع سابق</div><div class="val" style="color:var(--accent)">${formatBytes(dev.up)}</div></div>
                <div><div class="lbl">الإجمالي</div><div class="val" style="color:var(--cyan)">${formatBytes(dev.up + dev.down)}</div></div>
              </div>
              <div class="quota-box" style="margin-top:8px">
                <div style="display:flex;justify-content:space-between;margin-bottom:4px;font-size:0.78rem">
                  <span>📊 الكوتة المسبقة: <b>${quotaText}</b></span>
                  <span>حصة الباقة: ${dev.share_pct ? dev.share_pct.toFixed(1) : 0}%</span>
                </div>
                ${quotaGB > 0 ? `
                <div class="progress-bg" style="height:8px"><div class="progress-fill" style="width:${quotaBarWidth}%;background:${quotaBarColor}"></div></div>
                ` : ''}
              </div>

              <div class="dev-controls-grid">
                <div>
                  <label>كوتة مسبقة (GB):</label>
                  <input type="number" step="0.5" min="0" value="${quotaGB}" id="card_q_${canonMac}" 
                         oninput="this.dataset.dirty='1'"
                         onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                         title="0 = غير محدود">
                </div>
                <div>
                  <label>⬇️ حد التحميل:</label>
                  <input type="number" step="0.5" min="0" value="${downLimit}" id="card_dl_${canonMac}" 
                         oninput="this.dataset.dirty='1'"
                         onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                         placeholder="Mbps">
                </div>
                <div>
                  <label>⬆️ حد الرفع:</label>
                  <input type="number" step="0.5" min="0" value="${upLimit}" id="card_ul_${canonMac}" 
                         oninput="this.dataset.dirty='1'"
                         onkeydown="if(event.key==='Enter') saveDeviceCard('${canonMac}')"
                         placeholder="Mbps">
                </div>
              </div>
              <button type="button" class="primary-btn" style="width:100%;padding:6px 10px;font-size:0.8rem;margin-bottom:8px" onclick="saveDeviceCard('${canonMac}')">💾 حفظ الإعدادات المسبقة</button>

              <div class="btn-row">
                <button type="button" class="action-btn ${isBlocked ? 'btn-unblock' : 'btn-block'}" onclick="toggleBlock('${canonMac}', ${!isBlocked})">${isBlocked ? 'إلغاء الحظر المسبق' : 'حظر مسبق'}</button>
                <button type="button" class="action-btn btn-reset" onclick="resetDevice('${canonMac}')">🔄 تصفير</button>
                <button type="button" class="action-btn btn-delete" onclick="deleteDevice('${canonMac}')">🗑️ حذف</button>
              </div>
            </div>
          `;
        });
        if (occ) occ.innerHTML = offCardsHtml;
      }
    }
  }
}

// Device Actions with Auto-Auth & Toast Notifications
async function toggleGaming(mac, val) {
  let res = await postWithRetry('/api/device/gaming', {mac: mac, gaming: val ? '1' : '0'});
  if (res.ok) {
    showToast(val ? 'تم تفعيل وضع ألعاب VIP (أقصى سرعة وتخطي الحدود)! 👑' : 'تم إلغاء وضع VIP والعودة للسرعة المحددة بنجاح! 🚀', val ? 'warn' : 'success');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => toggleGaming(mac, val);
    openLoginModal(val ? 'يرجى تسجيل الدخول كمشرف لتفعيل وضع الألعاب VIP' : 'يرجى تسجيل الدخول كمشرف لإلغاء وضع VIP');
  } else {
    showToast('تعذر تغيير وضع VIP، يرجى المحاولة مجدداً', 'error');
  }
}

async function toggleBlock(mac, val) {
  let res = await postWithRetry('/api/device/block', {mac: mac, blocked: val ? '1' : '0'});
  if (res.ok) {
    showToast(val ? 'تم حظر الجهاز بنجاح! ⛔' : 'تم فك الحظر عن الجهاز بنجاح! 🟢', val ? 'warn' : 'success');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => toggleBlock(mac, val);
    openLoginModal(val ? 'يرجى تسجيل الدخول كمشرف لحظر هذا الجهاز' : 'يرجى تسجيل الدخول كمشرف لفك الحظر عن هذا الجهاز');
  } else {
    showToast('تعذر تغيير حالة الحظر، يرجى المحاولة مجدداً', 'error');
  }
}

async function resetDevice(mac) {
  if (!confirm('هل تريد تصفير عدادات هذا الجهاز؟')) return;
  let res = await postWithRetry('/api/device/reset', {mac: mac});
  if (res.ok) {
    showToast('تم تصفير استهلاك الجهاز بنجاح! 🔄', 'success');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => resetDevice(mac);
    openLoginModal('يرجى تسجيل الدخول كمشرف لتصفير عداد الجهاز');
  } else {
    showToast('تعذر تصفير العداد، يرجى المحاولة مجدداً', 'error');
  }
}

async function deleteDevice(mac) {
  if (!confirm('هل تريد حذف هذا الجهاز من السجل نهائياً؟')) return;
  let res = await postWithRetry('/api/device/delete', {mac: mac});
  if (res.ok) {
    showToast('تم حذف الجهاز من السجل بنجاح! 🗑️', 'info');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => deleteDevice(mac);
    openLoginModal('يرجى تسجيل الدخول كمشرف لحذف هذا الجهاز');
  } else {
    showToast('تعذر حذف الجهاز، يرجى المحاولة مجدداً', 'error');
  }
}

async function saveDeviceTable(mac) {
  let nameEl = document.getElementById('tbl_n_' + mac) || document.getElementById('off_tbl_n_' + mac);
  let qEl = document.getElementById('tbl_q_' + mac) || document.getElementById('off_tbl_q_' + mac);
  let dlEl = document.getElementById('tbl_dl_' + mac) || document.getElementById('off_tbl_dl_' + mac);
  let ulEl = document.getElementById('tbl_ul_' + mac) || document.getElementById('off_tbl_ul_' + mac);
  let tgEl = document.getElementById('tbl_tg_' + mac) || document.getElementById('off_tbl_tg_' + mac);
  let name = nameEl ? nameEl.value : null;
  let quota = qEl ? qEl.value : null;
  let down = dlEl ? dlEl.value : null;
  let up = ulEl ? ulEl.value : null;
  let tgChat = tgEl ? tgEl.value : null;

  if (qEl) qEl.dataset.dirty = '';
  if (dlEl) dlEl.dataset.dirty = '';
  if (ulEl) ulEl.dataset.dirty = '';
  if (tgEl) tgEl.dataset.dirty = '';
  if (nameEl) nameEl.dataset.dirty = '';
  await saveDeviceFull(mac, name, quota, down, up, tgChat);
}

async function saveDeviceCard(mac) {
  let qEl = document.getElementById('card_q_' + mac) || document.getElementById('off_card_q_' + mac);
  let dlEl = document.getElementById('card_dl_' + mac) || document.getElementById('off_card_dl_' + mac);
  let ulEl = document.getElementById('card_ul_' + mac) || document.getElementById('off_card_ul_' + mac);
  let tgEl = document.getElementById('card_tg_' + mac) || document.getElementById('off_card_tg_' + mac);
  let quota = qEl ? qEl.value : null;
  let down = dlEl ? dlEl.value : null;
  let up = ulEl ? ulEl.value : null;
  let tgChat = tgEl ? tgEl.value : null;

  await saveDeviceFull(mac, null, quota, down, up, tgChat);
}

async function saveDeviceFull(mac, name, quotaGb, downMbps, upMbps, tgChatId) {
  let params = new URLSearchParams({mac: mac});
  if (name) params.append('name', name);
  if (quotaGb !== undefined && quotaGb !== null) params.append('quota_gb', quotaGb);
  if (downMbps !== undefined && downMbps !== null) params.append('download_mbps', downMbps);
  if (upMbps !== undefined && upMbps !== null) params.append('upload_mbps', upMbps);
  if (tgChatId !== undefined && tgChatId !== null) params.append('telegram_chat_id', tgChatId);

  let res = await postWithRetry('/api/device/save', params);
  if (res.ok) {
    showToast('تم حفظ إعدادات الجهاز والكوتة والسرعة بنجاح! 💾', 'success');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => saveDeviceFull(mac, name, quotaGb, downMbps, upMbps, tgChatId);
    openLoginModal('يرجى تسجيل الدخول كمشرف لحفظ تعديلات الجهاز.');
  } else {
    let errTxt = res.error ? ' (' + res.error + ')' : '';
    showToast('فشل حفظ الإعدادات' + errTxt + '، يرجى المحاولة لاحقاً', 'error');
  }
}

async function saveDeviceName(mac, newName) {
  newName = (newName || '').trim();
  if (!newName) return;
  let res = await postWithRetry('/api/device/save', {mac: mac, name: newName});
  if (res.ok) {
    showToast(`تم تعديل اسم الجهاز إلى (${newName}) بنجاح! ✏️`, 'success');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => saveDeviceName(mac, newName);
    openLoginModal('لتعديل اسم الجهاز، يرجى تسجيل الدخول كمشرف');
  } else {
    showToast('تعذر حفظ اسم الجهاز', 'error');
  }
}

async function saveDeviceSpeed(mac, upMbps, downMbps) {
  let res = await postWithRetry('/api/device/save', {mac: mac, upload_mbps: upMbps, download_mbps: downMbps});
  if (res.ok) {
    showToast('تم تحديث حدود السرعة للجهاز بنجاح! 🚀', 'success');
    refreshData();
  } else if (res.status === 401) {
    _pendingAction = () => saveDeviceSpeed(mac, upMbps, downMbps);
    openLoginModal('يرجى تسجيل الدخول كمشرف لضبط السرعة');
  } else {
    showToast('تعذر تحديث السرعة', 'error');
  }
}
