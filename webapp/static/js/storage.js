let whCache = {}, boxCache = {}, specCache = {}, binSpecCache = {};

(async function init() {
    loadTree(); loadWarehouses(); loadBoxes(); loadSpecs(); loadBinSpecs();
    const cats = await api('/api/meta/categories');
    const pkgs = await api('/api/meta/packages');
    document.getElementById('categoryListStorage').innerHTML = cats.map(c => `<option value="${c}">`).join('');
    document.getElementById('packageListStorage').innerHTML = pkgs.map(p => `<option value="${p}">`).join('');
})();

async function loadTree() {
    try {
        const tree = await api('/api/storage/tree').catch(() => []); // 如果后端没这个接口，跳过
        // 若后端无tree接口，可直接隐藏左侧树，此处简写
        document.getElementById('storageTree').innerHTML = '<div class="dim">暂不支持树形视图(需后端支持)</div>';
    } catch (e) {}
}
document.getElementById('refreshTree').onclick = loadTree;

// --- 存储库 ---
async function loadWarehouses() {
    const list = await api('/api/warehouses');
    list.forEach(w => whCache[w.id] = w);
    document.getElementById('warehouseList').innerHTML = list.map(w => `
        <div class="mini-item">
            <div><a class="name-link" onclick="editWH(${w.id})">${w.name}</a> <span class="dim">${w.box_count}箱</span></div>
            <button class="btn btn-secondary btn-sm" onclick="locateWH(${w.id})">定位</button>
        </div>
    `).join('');
    // 填充下拉框
    document.getElementById('bWarehouse').innerHTML = list.map(w => `<option value="${w.id}">${w.name}</option>`).join('');
    document.getElementById('sWarehouses').innerHTML = list.map(w => `<option value="${w.id}">${w.name}</option>`).join('');
}
// 添加库表单提交
document.getElementById('warehouseForm').onsubmit = async (e) => {
    e.preventDefault();
    try {
        await api('/api/warehouses', { method: 'POST', body: JSON.stringify({
            name: document.getElementById('wName').value.trim(),
            nfc_uid: document.getElementById('wNfc').value.trim() || null,
            ble_mac: document.getElementById('wMac').value.trim() || null
            // 注意：这里不再提交 led_index，因为输入框已经删了
        })});
        document.getElementById('warehouseForm').reset(); 
        notify('添加成功'); 
        loadWarehouses();
    } catch (err) {
        alert('添加失败: ' + err.message);
    }
};
// 添加库定位功能
window.locateWH = async (id) => {
    try {
        const r = await api(`/api/warehouses/${id}/locate`, { method: 'POST' });
        notify(`已发送库定位指令 (点亮 ${r.lights.length} 个位置)`);
    } catch (e) { alert('定位失败: ' + e.message); }
};

// ============ 编辑：存储库 ============
window.editWH = (id) => {
    const w = whCache[id];
    if (!w) return;

    openModal('编辑存储库', `
        <div class="form-info">
            <strong>关联存储箱：</strong>${w.box_count || 0} 个
        </div>
        <div class="form-grid">
            <label>名称 *<input type="text" id="m_wname" value="${escapeHtml(w.name)}"></label>
            <label>NFC UID<input type="text" id="m_wnfc" value="${escapeHtml(w.nfc_uid || '')}"></label>
            <label>BLE MAC<input type="text" id="m_wmac" value="${escapeHtml(w.ble_mac || '')}"></label>
            <label class="full">备注<input type="text" id="m_wnote" value="${escapeHtml(w.note || '')}"></label>
        </div>
    `, async () => {
        const name = document.getElementById('m_wname').value.trim();
        if (!name) throw new Error('名称不能为空');
        await api('/api/warehouses/' + id, { method: 'PUT', body: JSON.stringify({
            name,
            nfc_uid:    document.getElementById('m_wnfc').value.trim()  || null,
            ble_mac:    document.getElementById('m_wmac').value.trim()  || null,
            note:       document.getElementById('m_wnote').value.trim() || null
        })});
        notify('已保存'); loadWarehouses();
    }, async () => {
        if (w.box_count > 0) {
            if (!confirm(`该存储库下有 ${w.box_count} 个存储箱，删除后将同时删除所有关联的存储箱与元件！\n\n确定继续？`)) {
                throw new Error('已取消');
            }
        }
        await api('/api/warehouses/' + id, { method: 'DELETE' });
        notify('已删除'); loadWarehouses(); loadBoxes();
    });
};


// --- 存储箱 ---
async function loadBoxes() {
    const list = await api('/api/boxes');
    list.forEach(b => boxCache[b.id] = b);
    document.getElementById('boxList').innerHTML = list.map(b => `
        <div class="mini-item">
            <div>
                <a class="name-link" onclick="editBox(${b.id})">${b.name}</a>
                <span class="dim">${b.warehouse_name} | ${b.spec_name} (${b.used_count}/${b.capacity})</span>
            </div>
            <button class="btn btn-secondary btn-sm" onclick="locateBox(${b.id})">定位</button>
        </div>
    `).join('');
}
document.getElementById('boxForm').onsubmit = async (e) => {
    e.preventDefault();
    await api('/api/boxes', { method: 'POST', body: JSON.stringify({
        warehouse_id: parseInt(document.getElementById('bWarehouse').value),
        spec_id: parseInt(document.getElementById('bSpec').value),
        name: document.getElementById('bName').value,
        nfc_uid: document.getElementById('bNfc').value,
        pref_category: document.getElementById('bPrefCat').value,
        pref_package: document.getElementById('bPrefPkg').value,
        ble_mac: document.getElementById('bMac').value,
    })});
    document.getElementById('boxForm').reset(); notify('添加成功'); loadBoxes();
};
window.locateBox = async (id) => {
    const r = await api(`/api/boxes/${id}/locate`, { method: 'POST' });
    notify('定位指令: ' + JSON.stringify(r.lights));
};

// ============ 编辑：存储箱 ============
window.editBox = (id) => {
    const b = boxCache[id];
    if (!b) return;
    if (!Object.keys(whCache).length || !Object.keys(specCache).length) {
        alert('数据正在加载，请稍候再试');
        return;
    }

    const whOptions = Object.values(whCache).map(w =>
        `<option value="${w.id}" ${w.id == b.warehouse_id ? 'selected' : ''}>${escapeHtml(w.name)}</option>`
    ).join('');

    const specOptions = Object.values(specCache).map(s =>
        `<option value="${s.id}" ${s.id == b.spec_id ? 'selected' : ''}>${escapeHtml(s.name)} (${s.capacity}格)</option>`
    ).join('');

    openModal('编辑存储箱', `
        <div class="form-info">
            <strong>当前位置：</strong>${escapeHtml(b.warehouse_name || '')} · 已用 ${b.used_count}/${b.capacity} 格
        </div>
        <div class="form-grid">
            <label>名称 *<input type="text" id="m_bname" value="${escapeHtml(b.name)}"></label>
            <label>所属库<select id="m_bwh">${whOptions}</select></label>
            <label>规格<select id="m_bspec">${specOptions}</select></label>
            <label>NFC UID<input type="text" id="m_bnfc" value="${escapeHtml(b.nfc_uid || '')}"></label>
            <label>BLE MAC<input type="text" id="m_bmac" value="${escapeHtml(b.ble_mac || '')}"></label>
            <label>优先分类<input type="text" id="m_bcat" value="${escapeHtml(b.pref_category || '')}" list="editCatList"></label>
            <label>优先封装<input type="text" id="m_bpkg" value="${escapeHtml(b.pref_package || '')}" list="editPkgList"></label>
            <label class="full">备注<input type="text" id="m_bnote" value="${escapeHtml(b.note || '')}"></label>
        </div>
        <datalist id="editCatList"></datalist>
        <datalist id="editPkgList"></datalist>
    `, async () => {
        const name = document.getElementById('m_bname').value.trim();
        if (!name) throw new Error('名称不能为空');
        const newSpecId = parseInt(document.getElementById('m_bspec').value, 10);
        const newSpec = specCache[newSpecId];
        if (newSpec && b.used_count > newSpec.capacity) {
            if (!confirm(`新规格容量 ${newSpec.capacity} 小于当前已用 ${b.used_count} 格，是否仍然保存？`)) {
                throw new Error('已取消');
            }
        }
        await api('/api/boxes/' + id, { method: 'PUT', body: JSON.stringify({
            name,
            warehouse_id: parseInt(document.getElementById('m_bwh').value, 10),
            spec_id: newSpecId,
            nfc_uid:       document.getElementById('m_bnfc').value.trim()  || null,
            ble_mac:       document.getElementById('m_bmac').value.trim()  || null,
            pref_category: document.getElementById('m_bcat').value.trim()  || null,
            pref_package:  document.getElementById('m_bpkg').value.trim()  || null,
            note:          document.getElementById('m_bnote').value.trim() || null
        })});
        notify('已保存'); loadBoxes(); loadWarehouses();
    }, async () => {
        // 删除：先试直接删，若 409 说明非空，再询问
        try {
            await api('/api/boxes/' + id, { method: 'DELETE' });
            notify('已删除'); loadBoxes(); loadWarehouses();
            return;
        } catch (err) {
            if (!/非空|有元件|bin_count/.test(err.message)) throw err;
        }
        const choice = prompt(
            '该存储箱非空！\n\n' +
            '输入 1：删除所有元件\n' +
            '输入 2：自动转移到同规格箱\n' +
            '输入其他：取消',
            ''
        );
        if (choice === '1') {
            await api(`/api/boxes/${id}?mode=delete_all`, { method: 'DELETE' });
            notify('已连同元件删除'); loadBoxes(); loadWarehouses();
        } else if (choice === '2') {
            await api(`/api/boxes/${id}?mode=auto`, { method: 'DELETE' });
            notify('已转移并删除'); loadBoxes(); loadWarehouses();
        } else {
            throw new Error('已取消');
        }
    });

    // 异步补全 datalist（不阻塞弹窗）
    Promise.all([api('/api/meta/categories'), api('/api/meta/packages')]).then(([cats, pkgs]) => {
        const cd = document.getElementById('editCatList');
        if (cd) cd.innerHTML = cats.map(c => `<option value="${escapeHtml(c)}">`).join('');
        const pd = document.getElementById('editPkgList');
        if (pd) pd.innerHTML = pkgs.map(p => `<option value="${escapeHtml(p)}">`).join('');
    }).catch(() => {});
};

// --- 规格与盒规格 ---
async function loadSpecs() {
    const list = await api('/api/box-specs');
    list.forEach(s => specCache[s.id] = s);
    document.getElementById('specList').innerHTML = list.map(s => `
        <div class="mini-item"><div><a class="name-link" onclick="editSpec(${s.id})">${s.name}</a> <span class="dim">${s.capacity}格</span></div></div>
    `).join('');
    document.getElementById('bSpec').innerHTML = list.map(s => `<option value="${s.id}">${s.name} (${s.capacity})</option>`).join('');
    document.getElementById('bsBoxSpecs').innerHTML = list.map(s => `<option value="${s.id}">${s.name}</option>`).join('');
}
document.getElementById('specForm').onsubmit = async (e) => {
    e.preventDefault();
    const whs = [...document.getElementById('sWarehouses').selectedOptions].map(o => parseInt(o.value));
    await api('/api/box-specs', { method: 'POST', body: JSON.stringify({
        name: document.getElementById('sName').value,
        capacity: parseInt(document.getElementById('sCapacity').value),
        warehouse_ids: whs
    })});
    document.getElementById('specForm').reset(); notify('已添加'); loadSpecs();
};

// ============ 编辑：箱规格 ============
window.editSpec = (id) => {
    const s = specCache[id];
    if (!s) return;
    if (!Object.keys(whCache).length) {
        alert('数据正在加载，请稍候再试');
        return;
    }

    const whOptions = Object.values(whCache).map(w =>
        `<option value="${w.id}" ${(s.warehouse_ids || []).includes(w.id) ? 'selected' : ''}>${escapeHtml(w.name)}</option>`
    ).join('');

    openModal('编辑箱规格', `
        <div class="form-info">
            <strong>已使用：</strong>${s.box_count || 0} 个存储箱
        </div>
        <div class="form-grid">
            <label>名称 *<input type="text" id="m_sname" value="${escapeHtml(s.name)}"></label>
            <label>容量 *<input type="number" id="m_scap" value="${s.capacity}" min="1"></label>
            <label class="full">兼容的存储库（不选 = 全通用）
                <select id="m_swhs" multiple size="3">${whOptions}</select>
            </label>
            <label class="full">备注<input type="text" id="m_snote" value="${escapeHtml(s.note || '')}"></label>
        </div>
    `, async () => {
        const name = document.getElementById('m_sname').value.trim();
        const cap  = parseInt(document.getElementById('m_scap').value, 10);
        if (!name) throw new Error('名称不能为空');
        if (!cap || cap < 1) throw new Error('容量必须大于 0');
        const whs = [...document.getElementById('m_swhs').selectedOptions].map(o => parseInt(o.value, 10));
        await api('/api/box-specs/' + id, { method: 'PUT', body: JSON.stringify({
            name,
            capacity: cap,
            note: document.getElementById('m_snote').value.trim() || null,
            warehouse_ids: whs
        })});
        notify('已保存'); loadSpecs();
    }, async () => {
        await api('/api/box-specs/' + id, { method: 'DELETE' });
        notify('已删除'); loadSpecs();
    });
};


async function loadBinSpecs() {
    const list = await api('/api/bin-specs').catch(() => []);
    list.forEach(s => binSpecCache[s.id] = s);
    document.getElementById('binSpecList').innerHTML = list.map(s => `
        <div class="mini-item"><div><a class="name-link" onclick="editBinSpec(${s.id})">${s.name}</a></div></div>
    `).join('');
}
document.getElementById('binSpecForm').onsubmit = async (e) => {
    e.preventDefault();
    const specs = [...document.getElementById('bsBoxSpecs').selectedOptions].map(o => parseInt(o.value));
    await api('/api/bin-specs', { method: 'POST', body: JSON.stringify({
        name: document.getElementById('bsName').value,
        note: document.getElementById('bsNote').value,
        box_spec_ids: specs
    })});
    document.getElementById('binSpecForm').reset(); notify('已添加'); loadBinSpecs();
};

// ============ 编辑：盒规格 ============
window.editBinSpec = (id) => {
    const s = binSpecCache[id];
    if (!s) return;
    if (!Object.keys(specCache).length) {
        alert('数据正在加载，请稍候再试');
        return;
    }

    const bsOptions = Object.values(specCache).map(sp =>
        `<option value="${sp.id}" ${(s.box_spec_ids || []).includes(sp.id) ? 'selected' : ''}>${escapeHtml(sp.name)} (${sp.capacity}格)</option>`
    ).join('');

    openModal('编辑盒规格', `
        <div class="form-grid">
            <label class="full">名称 *<input type="text" id="m_bsname" value="${escapeHtml(s.name)}"></label>
            <label class="full">兼容的箱规格（可多选）
                <select id="m_bsboxspecs" multiple size="4">${bsOptions}</select>
            </label>
            <label class="full">备注<input type="text" id="m_bsnote" value="${escapeHtml(s.note || '')}"></label>
        </div>
    `, async () => {
        const name = document.getElementById('m_bsname').value.trim();
        if (!name) throw new Error('名称不能为空');
        const bsids = [...document.getElementById('m_bsboxspecs').selectedOptions].map(o => parseInt(o.value, 10));
        await api('/api/bin-specs/' + id, { method: 'PUT', body: JSON.stringify({
            name,
            note: document.getElementById('m_bsnote').value.trim() || null,
            box_spec_ids: bsids
        })});
        notify('已保存'); loadBinSpecs();
    }, async () => {
        await api('/api/bin-specs/' + id, { method: 'DELETE' });
        notify('已删除'); loadBinSpecs();
    });
};