// ============================================================
// 添加元件页
// ============================================================
async function loadMeta() {
  try {
    const [cats, pkgs] = await Promise.all([
      api('/api/meta/categories'),
      api('/api/meta/packages'),
    ]);
    document.getElementById('categoryList').innerHTML =
      cats.map(c => '<option value="' + escapeHtml(c) + '">').join('');
    document.getElementById('packageList').innerHTML =
      pkgs.map(p => '<option value="' + escapeHtml(p) + '">').join('');
  } catch (e) { console.warn('加载元数据失败', e); }
}

async function loadBoxOptions() {
  try {
    const list = await api('/api/boxes');
    const sel = document.getElementById('cBoxId');
    sel.innerHTML = '<option value="">自动分配</option>' +
      list.map(b =>
        `<option value="${b.id}">
          ${escapeHtml(b.name)} (${escapeHtml(b.warehouse_name)} / ${b.used_count}/${b.capacity})
          ${b.pref_category ? ' · 偏好:' + escapeHtml(b.pref_category) : ''}
          ${b.pref_package  ? '/' + escapeHtml(b.pref_package)  : ''}
        </option>`).join('');
    return list; // 返回列表供匹配使用
  } catch (e) { 
    console.warn(e); 
    return [];
  }
}

document.getElementById('addComponentForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const payload = {
    name:     document.getElementById('cName').value.trim(),
    category: document.getElementById('cCategory').value.trim() || null,
    package:  document.getElementById('cPackage').value.trim() || null,
    value:    document.getElementById('cValue').value.trim() || null,
    quantity: parseInt(document.getElementById('cQuantity').value || '0', 10),
    nfc_uid:  document.getElementById('cNfc').value.trim() || null,
    note:     document.getElementById('cNote').value.trim() || null,
  };
  const boxId = document.getElementById('cBoxId').value;
  if (boxId) payload.box_id = parseInt(boxId, 10);

  const res = document.getElementById('addResult');
  try {
    const r = await api('/api/components', {
      method: 'POST',
      body: JSON.stringify(payload)
    });
    res.innerHTML = `
      <div class="success">
        已添加 ✓ <br>存储盒 ID: <strong>${r.id}</strong> <br>所在箱: <strong>#${r.box_id}</strong> <br>格位: <strong>#${r.slot_index + 1}</strong>
      </div>
    `;
    document.getElementById('addComponentForm').reset();
    notify('已添加到箱 #' + r.box_id + ' 格位 #' + (r.slot_index + 1));
    sessionStorage.removeItem('prefill_nfc'); // 添加完清空缓存
    loadStats();
    loadBoxOptions();
    loadMeta();
  } catch (err) {
    res.innerHTML = '<div class="error">添加失败: ' + escapeHtml(err.message) + '</div>';
  }
});

document.getElementById('btnWriteNfc').onclick = async () => {
    const name = document.getElementById('cName').value.trim();
    if (!name) return alert('请至少填写元件名称');
    
    const category = document.getElementById('cCategory').value.trim();
    const package = document.getElementById('cPackage').value.trim();
    const value = document.getElementById('cValue').value.trim();
    const quantity = document.getElementById('cQuantity').value.trim();
    const note = document.getElementById('cNote').value.trim();
    
    const text = `MeX电仓\n名称: ${name}\n分类: ${category}\n封装: ${package}\n值: ${value}\n数量: ${quantity}\n备注: ${note}`;
    
    try {
        await api('/api/nfc/write', { method: 'POST', body: JSON.stringify({ text: text }) });
        notify('已下发写卡指令，请将NFC标签靠近板子');
    } catch (e) {
        alert('写卡失败: ' + e.message);
    }
};

// 将 MAC 地址转为纯字母数字并大写，方便比对
function normalizeMac(mac) {
    if (!mac) return '';
    return String(mac).replace(/[^a-zA-Z0-9]/g, '').toUpperCase();
}

// ============================================================
// 初始化 & 预填逻辑
// ============================================================
(async () => {
  await loadMeta();
  const boxes = await loadBoxOptions(); // 获取所有箱子的数据
  
  // 读取 NFC 扫码带来的预填数据
    const prefillStr = sessionStorage.getItem('prefill_nfc');
    if (prefillStr) {
        sessionStorage.removeItem('prefill_nfc'); // 用后即焚
        try {
            const prefill = JSON.parse(prefillStr);
            document.getElementById('cNfc').value = prefill.uid || '';
            
            if (prefill.text) {
                // 判断是否是我们系统写入的专用格式
                if (prefill.text.includes('MeX电仓')) {
                    const lines = prefill.text.split('\n');
                    lines.forEach(line => {
                        if (line.startsWith('名称: '))   document.getElementById('cName').value     = line.substring(4).trim();
                        if (line.startsWith('分类: '))   document.getElementById('cCategory').value = line.substring(4).trim();
                        if (line.startsWith('封装: '))   document.getElementById('cPackage').value  = line.substring(4).trim();
                        if (line.startsWith('值: '))     document.getElementById('cValue').value    = line.substring(3).trim();
                        if (line.startsWith('数量: '))   document.getElementById('cQuantity').value = line.substring(4).trim();
                        if (line.startsWith('备注: '))   document.getElementById('cNote').value     = line.substring(4).trim();
                    });
                    notify('已自动解析并填入元件信息');
                } else {
                    // 普通的非标文本，全部放入备注
                    document.getElementById('cNote').value = prefill.text;
                }
            }
            
            // 根据 MAC 匹配存储箱
            if (prefill.nodeMac) {
                const matchBox = boxes.find(b => b.ble_mac && b.ble_mac.toUpperCase() === prefill.nodeMac.toUpperCase());
                if (matchBox) {
                    boxSelect.value = matchBox.id;
                    notify(`已根据节点 MAC 匹配到存储箱: ${matchBox.name}`);
                }
            }
        } catch (e) { 
            console.error('解析预填数据失败', e); 
        }
    }
})();