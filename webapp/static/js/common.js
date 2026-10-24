// ============================================================
// 通用工具 —— 所有页面共享
// ============================================================

async function api(url, options = {}) {
  const opts = Object.assign(
    { headers: { 'Content-Type': 'application/json' } }, options
  );
  const res = await fetch(url, opts);
  const text = await res.text();
  let data;
  try { data = text ? JSON.parse(text) : {}; }
  catch (e) { data = { error: text || ('HTTP ' + res.status) }; }
  if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
  return data;
}

function escapeHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

function notify(msg) {
  console.log('[msg]', msg);
  const bar = document.getElementById('statsBar');
  if (!bar) return;
  const orig = bar.dataset.orig || bar.innerHTML;
  bar.dataset.orig = orig;
  bar.innerHTML = '<span class="flash">' + escapeHtml(msg) + '</span>';
  setTimeout(() => { bar.innerHTML = bar.dataset.orig; }, 2500);
}

function alertErr(e) {
  alert('错误: ' + (e && e.message ? e.message : e));
}

async function loadStats() {
  try {
    const s = await api('/api/storage/stats');
    const bar = document.getElementById('statsBar');
    if (!bar) return;
    const text = '库 ' + s.warehouses + ' · 箱 ' + s.boxes +
                 ' / ' + s.total_capacity + ' 格 · 盒 ' + s.bins;
    bar.dataset.orig = text;
    bar.innerHTML = text;
  } catch (e) { /* ignore */ }
}

// ============================================================
// 通用 Modal
// ============================================================
let _modalOnSave = null;
let _modalOnDelete = null;

function openModal(title, bodyHtml, onSave, onDelete) {
  _modalOnSave = onSave || null;
  _modalOnDelete = onDelete || null;

  document.getElementById('modalTitle').textContent = title;
  document.getElementById('modalBody').innerHTML = bodyHtml;

  // 每次重建 footer，避免被 components 详情弹窗改坏
  const footer = document.querySelector('.modal-footer');
  footer.innerHTML = `
    <button class="btn btn-danger" id="modalDelete" style="margin-right:auto;">删除</button>
    <button class="btn btn-secondary" id="modalCancel">取消</button>
    <button class="btn btn-primary" id="modalSave">保存</button>
  `;

  const delBtn = document.getElementById('modalDelete');
  if (onDelete) {
    delBtn.style.display = 'inline-flex';
    delBtn.onclick = async () => {
      if (!_modalOnDelete) return;
      if (!confirm('确定要删除吗？此操作不可撤销。')) return;
      try {
        await _modalOnDelete();
        closeModal();
      } catch (e) {
        if (e && e.message !== '已取消') alert('删除失败: ' + (e.message || e));
      }
    };
  } else {
    delBtn.style.display = 'none';
  }

  document.getElementById('modalCancel').onclick = closeModal;
  document.getElementById('modalSave').onclick = async () => {
    if (!_modalOnSave) { closeModal(); return; }
    try {
      await _modalOnSave();
      closeModal();
    } catch (e) {
      if (e && e.message !== '已取消') alert('保存失败: ' + (e.message || e));
    }
  };

  document.getElementById('modalOverlay').classList.add('show');
}

function closeModal() {
  _modalOnSave = null;
  _modalOnDelete = null;
  document.getElementById('modalOverlay').classList.remove('show');
}

// 重置弹窗底栏
function resetModalFooter() {
  const footer = document.querySelector('.modal-footer');
  if (!footer) return;
  footer.innerHTML = `
    <button class="btn btn-danger" id="modalDelete" style="display:none; margin-right:auto;">删除</button>
    <button class="btn btn-secondary" id="modalCancel">取消</button>
    <button class="btn btn-primary" id="modalSave">保存</button>
  `;
  
  document.getElementById('modalCancel').onclick = closeModal;
  document.getElementById('modalSave').onclick = async () => {
    if (!_modalOnSave) return;
    try { await _modalOnSave(); closeModal(); } catch(e) { alert(e.message); }
  };
}

async function loadModalMeta(datalistId, apiPath) {
  try {
    const items = await api(apiPath);
    const dl = document.getElementById(datalistId);
    if (dl) {
      dl.innerHTML = items.map(v =>
        '<option value="' + escapeHtml(v) + '">'
      ).join('');
    }
  } catch (e) { console.warn('loadModalMeta 失败', e); }
}

function _initModal() {
  const close = document.getElementById('modalClose');
  const overlay = document.getElementById('modalOverlay');
  if (!close || !overlay) return;

  close.addEventListener('click', closeModal);
  overlay.addEventListener('click', e => {
    if (e.target.id === 'modalOverlay') closeModal();
  });
}

function _onReady(fn) {
  if (document.readyState !== 'loading') fn();
  else document.addEventListener('DOMContentLoaded', fn);
}

_onReady(() => {
  _initModal();
  resetModalFooter();
  loadStats();
});

// ============================================================
// SSE 实时事件 —— 单例 + 自动重连 + 就绪反馈
// ============================================================
let _sseSource = null;

function _initSSE() {
  if (_sseSource) return;

  _sseSource = new EventSource('/api/events');

  _sseSource.onopen = () => {
    console.log('[SSE] 已连接');
    _setSseIndicator(true);
  };

  _sseSource.onerror = (e) => {
    console.warn('[SSE] 断开，浏览器将自动重连', e);
    _setSseIndicator(false);
  };

  _sseSource.onmessage = async (e) => {
    let data;
    try { data = JSON.parse(e.data); }
    catch (err) { return console.warn('[SSE] 数据异常', e.data); }

    if (data.type === 'connected') return;

    if (data.type === 'nfc_scan') {
      notify(`📡 收到 NFC 刷卡: ${data.uid}`);
      try {
        const res = await api(`/api/nfc/${data.uid}`);
        
        // 找到对应物品，自动触发定位
        if (res.type === 'bin') api(`/api/components/${res.data.id}/locate`, { method: 'POST' }).catch(console.error);
        else if (res.type === 'box') api(`/api/boxes/${res.data.id}/locate`, { method: 'POST' }).catch(console.error);
        else if (res.type === 'warehouse') api(`/api/warehouses/${res.data.id}/locate`, { method: 'POST' }).catch(console.error);

        showNfcResultModal(res);
      } catch (err) {
        // =========================================================
        // 404 说明是新标签
        // 核心优化：如果当前正在“添加元件”页面，直接填充 UID，不弹窗！
        // =========================================================
        if (window.location.pathname === '/components/add') {
            const nfcInput = document.getElementById('cNfc');
            if (nfcInput) {
                nfcInput.value = data.uid;
                notify('✅ 已自动填入新的 NFC UID');
                
                // 如果卡片里有旧文本，自动填入备注
                if (data.text) {
                    const noteInput = document.getElementById('cNote');
                    if (noteInput && !noteInput.value) noteInput.value = data.text;
                }
                return; // 结束执行，不弹窗
            }
        }
        
        // 其他页面，照常弹出新标签窗口
        showNewTagModal(data.uid, data.text, data.node);
      }
    }
  };
}

function _setSseIndicator(ok) {
  const el = document.getElementById('sseIndicator');
  if (!el) return;
  el.style.color = ok ? '#27ae60' : '#e74c3c';
  el.textContent = ok ? '● 硬件监听：已就绪' : '● 硬件监听：重连中…';
}

// 页面从后台切回来时检查一次，必要时重建连接
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState !== 'visible') return;
  if (!_sseSource || _sseSource.readyState === EventSource.CLOSED) {
    if (_sseSource) { _sseSource.close(); _sseSource = null; }
    _initSSE();
  }
});

window.addEventListener('beforeunload', () => {
  if (_sseSource) { _sseSource.close(); _sseSource = null; }
});

_onReady(() => {
  _initModal();
  loadStats();
  _initSSE();     // ← 在这里启动
});

// ============================================================
// 统一元件详情弹窗 (保持原有逻辑，增加异常捕获)
// ============================================================
async function openComponentDetailModal(c, mode = 'view') {
    try {
        let boxes = [];
        if (mode === 'edit') {
            try { boxes = await api('/api/boxes'); } catch (e) { console.warn(e); }
        }

        const html = `
            <div id="detailViewMode" style="display: ${mode === 'view' ? 'block' : 'none'};">
                <div class="form-info">
                    <strong>当前位置：</strong>${escapeHtml(c.warehouse_name || '未知')} / ${escapeHtml(c.box_name || '未知')} · 格位 #${(c.slot_index || 0) + 1}
                </div>
                <div class="form-grid">
                    <label class="full">名称<input type="text" value="${escapeHtml(c.name)}" readonly></label>
                    <label>分类<input type="text" value="${escapeHtml(c.category || '')}" readonly></label>
                    <label>封装<input type="text" value="${escapeHtml(c.package || '')}" readonly></label>
                    <label>值<input type="text" value="${escapeHtml(c.value || '')}" readonly></label>
                    <label>数量<input type="text" value="${c.quantity}" readonly></label>
                    <label>NFC<input type="text" value="${escapeHtml(c.nfc_uid || '')}" readonly></label>
                    <label class="full">备注<input type="text" value="${escapeHtml(c.note || '')}" readonly></label>
                </div>
            </div>

            <div id="detailEditMode" style="display: ${mode === 'edit' ? 'block' : 'none'};">
                <div class="form-grid">
                    <label class="full">名称<input type="text" id="m_name" value="${escapeHtml(c.name)}"></label>
                    <label>分类<input type="text" id="m_cat" value="${escapeHtml(c.category || '')}"></label>
                    <label>封装<input type="text" id="m_pkg" value="${escapeHtml(c.package || '')}"></label>
                    <label>值<input type="text" id="m_val" value="${escapeHtml(c.value || '')}"></label>
                    <label>数量<input type="number" id="m_qty" value="${c.quantity}"></label>
                    <label>NFC<input type="text" id="m_nfc" value="${escapeHtml(c.nfc_uid || '')}"></label>
                    <label class="full">转移到其他箱
                        <select id="m_box">
                            <option value="">(保持原位)</option>
                            ${boxes.filter(b => b.id !== c.box_id).map(b => `<option value="${b.id}">${escapeHtml(b.warehouse_name)} / ${escapeHtml(b.name)}</option>`).join('')}
                        </select>
                    </label>
                    <label class="full">备注<input type="text" id="m_note" value="${escapeHtml(c.note || '')}"></label>
                </div>
            </div>
        `;

        openModal('元件详情', html, null);

        const footer = document.querySelector('.modal-footer');
        footer.innerHTML = `
            <button class="btn btn-secondary" id="btnStock" style="margin-right:auto; display: ${mode === 'view' ? 'inline-flex' : 'none'};">库存</button>
            <button class="btn btn-secondary" id="btnWriteNfc" style="display: ${mode === 'view' ? 'inline-flex' : 'none'};">写入NFC</button>
            <button class="btn btn-danger" id="btnDelete" style="display: ${mode === 'view' ? 'inline-flex' : 'none'};">删除</button>
            <button class="btn btn-primary" id="btnEditSave">${mode === 'edit' ? '保存' : '编辑'}</button>
            <button class="btn btn-secondary" id="btnLocate" style="display: ${mode === 'view' ? 'inline-flex' : 'none'};">定位</button>
            <button class="btn btn-secondary" id="btnClose">关闭</button>
        `;

        document.getElementById('btnClose').onclick = closeModal;
        
        document.getElementById('btnWriteNfc').onclick = async () => {
            const text = `MeX电仓\n名称: ${c.name}\n分类: ${c.category || ''}\n封装: ${c.package || ''}\n值: ${c.value || ''}\n数量: ${c.quantity || 0}\n备注: ${c.note || ''}`;
            try {
                await api('/api/nfc/write', { method: 'POST', body: JSON.stringify({ text: text }) });
                notify('已下发写卡指令，请将NFC标签靠近板子');
            } catch (e) {
                alert('写卡失败: ' + e.message);
            }
        };
        
        document.getElementById('btnLocate').onclick = () => {
            api('/api/components/' + c.id + '/locate', { method: 'POST' })
                .then(r => notify('定位指令：' + JSON.stringify(r.lights)))
                .catch(e => alert(e.message));
        };

        document.getElementById('btnEditSave').onclick = async () => {
            if (mode === 'view') {
                openComponentDetailModal(c, 'edit');
            } else {
                const payload = {
                    name: document.getElementById('m_name').value.trim(),
                    category: document.getElementById('m_cat').value.trim() || null,
                    package: document.getElementById('m_pkg').value.trim() || null,
                    value: document.getElementById('m_val').value.trim() || null,
                    quantity: parseInt(document.getElementById('m_qty').value || '0', 10),
                    nfc_uid: document.getElementById('m_nfc').value.trim() || null,
                    note: document.getElementById('m_note').value.trim() || null
                };
                const nb = document.getElementById('m_box').value;
                if (nb) payload.box_id = parseInt(nb);

                try {
                    await api('/api/bins/' + c.id, { method: 'PUT', body: JSON.stringify(payload) });
                    notify('保存成功');
                    closeModal();
                    if (typeof render === 'function') render();
                } catch (e) { alert('保存失败: ' + e.message); }
            }
        };

        document.getElementById('btnDelete').onclick = () => {
            if (!confirm(`确定要删除元件「${c.name}」吗？此操作不可恢复。`)) return;
            api('/api/bins/' + c.id, { method: 'DELETE' }).then(() => {
                notify('元件已删除');
                closeModal();
                if (typeof render === 'function') render();
            }).catch(e => alert('删除失败: ' + e.message));
        };

        document.getElementById('btnStock').onclick = () => {
            const delta = prompt(`当前库存: ${c.quantity}\n请输入调整数量 (正数增加，负数减少):`, '');
            if (delta === null || delta === '') return;
            const num = parseInt(delta, 10);
            if (isNaN(num)) return alert('请输入有效数字');
            
            const newQty = Math.max(0, c.quantity + num);
            api('/api/bins/' + c.id, { method: 'PUT', body: JSON.stringify({ quantity: newQty }) }).then(() => {
                notify('库存已更新');
                c.quantity = newQty;
                openComponentDetailModal(c, 'view');
                if (typeof render === 'function') render();
            }).catch(e => alert('更新失败: ' + e.message));
        };
    } catch (err) {
        console.error("详情弹窗渲染失败", err);
    }
}
window.openComponentDetailModal = openComponentDetailModal;

// ============================================================
// NFC 弹窗处理
// ============================================================
function showNfcResultModal(res) {
  const d = res.data;
  
  if (res.type === 'bin') {
    const c = {
      id: d.id, name: d.name, category: d.category, package: d.package,
      value: d.value, quantity: d.quantity, nfc_uid: d.nfc_uid,
      slot_index: d.slot_index, box_id: d.box_id, box_name: d.box_name,
      warehouse_name: d.wh_name, note: d.note || ''
    };
    window.openComponentDetailModal(c, 'view');
    return;
  }

  const html = `
    <div class="form-info" style="border-left-color:#27ae60;"><strong>发现已知设备 (${res.type})</strong></div>
    <div class="form-grid">
      <label class="full">名称<input type="text" value="${escapeHtml(d.name)}" readonly></label>
    </div>
  `;
  openModal('NFC 扫描结果', html, null);
  
  const footer = document.querySelector('.modal-footer');
  footer.innerHTML = `<button class="btn btn-secondary" id="btnNfcClose">关闭</button>`;
  document.getElementById('btnNfcClose').onclick = closeModal;
}

function showNewTagModal(uid, text, nodeMac) {
  const html = `
    <div class="form-info" style="border-left-color:#e74c3c;">
      <strong>未录入的 NFC 标签</strong>
    </div>
    <div class="form-grid">
      <label class="full">NFC UID<input type="text" value="${escapeHtml(uid)}" readonly></label>
      <label class="full">标签文本<input type="text" value="${escapeHtml(text || '无')}" readonly></label>
      <label class="full">扫描节点 MAC<input type="text" value="${escapeHtml(nodeMac || '未知')}" readonly></label>
    </div>
  `;
  
  openModal('发现新标签', html, null);
  
  const footer = document.querySelector('.modal-footer');
  footer.innerHTML = `
    <button class="btn btn-primary" id="btnNewAdd" style="margin-right:auto;">添加元件</button>
    <button class="btn btn-secondary" id="btnNfcClose">关闭</button>
  `;

  document.getElementById('btnNfcClose').onclick = closeModal;
  document.getElementById('btnNewAdd').onclick = () => {
    sessionStorage.setItem('prefill_nfc', JSON.stringify({ uid: uid, text: text, nodeMac: nodeMac }));
    window.location.href = '/components/add';
  };
}

// ============================================================
// 移动端导航菜单交互
// ============================================================
_onReady(() => {
  const menuToggle = document.getElementById('menuToggle');
  const navLinks = document.getElementById('navLinks');

  if (!menuToggle || !navLinks) return;

  // 1. 点击汉堡按钮切换菜单
  menuToggle.addEventListener('click', (e) => {
    e.stopPropagation();
    navLinks.classList.toggle('show');
  });

  // 2. 点击菜单项后自动收起
  navLinks.querySelectorAll('a').forEach(a => {
    a.addEventListener('click', () => {
      navLinks.classList.remove('show');
    });
  });

  // 3. 点击页面其他地方收起菜单
  document.addEventListener('click', (e) => {
    if (!navLinks.contains(e.target) && !menuToggle.contains(e.target)) {
      navLinks.classList.remove('show');
    }
  });
});