(async function init() {
    loadSettings(); loadInfo(); loadBackupList();
})();

async function loadSettings() {
    const s = await api('/api/settings');
    document.getElementById('set_default_quantity').value = s.default_quantity || '0';
    document.getElementById('set_page_size').value = s.page_size || '100';
    document.getElementById('set_auto_backup_enabled').value = s.auto_backup_enabled || '0';
    document.getElementById('set_auto_backup_interval').value = s.auto_backup_interval || '60';
    document.getElementById('set_auto_backup_keep').value = s.auto_backup_keep || '10';

    const whs = await api('/api/warehouses');
    document.getElementById('set_default_warehouse').innerHTML = '<option value="">(不指定)</option>' + whs.map(w => `<option value="${w.id}">${w.name}</option>`).join('');
    document.getElementById('set_default_warehouse').value = s.default_warehouse_id || '';

    const specs = await api('/api/box-specs');
    document.getElementById('set_default_spec').innerHTML = '<option value="">(不指定)</option>' + specs.map(sp => `<option value="${sp.id}">${sp.name}</option>`).join('');
    document.getElementById('set_default_spec').value = s.default_spec_id || '';

    // 加载颜色和亮度
    const r = parseInt(s.led_r || 0);
    const g = parseInt(s.led_g || 255);
    const b = parseInt(s.led_b || 0);
    const bright = s.led_brightness || '255';

    // 将 RGB 转为 HEX 颜色代码
    const hex = '#' + [r, g, b].map(x => x.toString(16).padStart(2, '0')).join('');
    document.getElementById('set_led_color').value = hex;
    document.getElementById('set_led_brightness').value = bright;
    document.getElementById('ledBrightnessVal').innerText = bright;

}

document.getElementById('saveBasic').onclick = async () => {
    // 读取颜色 HEX 并转回 R G B
    const hex = document.getElementById('set_led_color').value;
    const r = parseInt(hex.substring(1, 3), 16);
    const g = parseInt(hex.substring(3, 5), 16);
    const b = parseInt(hex.substring(5, 7), 16);

    await api('/api/settings', { method: 'PUT', body: JSON.stringify({
        default_warehouse_id: document.getElementById('set_default_warehouse').value,
        default_spec_id: document.getElementById('set_default_spec').value,
        default_quantity: document.getElementById('set_default_quantity').value,
        page_size: document.getElementById('set_page_size').value,
        led_r: r,
        led_g: g,
        led_b: b,
        led_brightness: document.getElementById('set_led_brightness').value
    })});
    notify('设置已保存');
};

document.getElementById('saveBackup').onclick = async () => {
    await api('/api/settings', {
        method: 'PUT', body: JSON.stringify({
            auto_backup_enabled: document.getElementById('set_auto_backup_enabled').value,
            auto_backup_interval: document.getElementById('set_auto_backup_interval').value,
            auto_backup_keep: document.getElementById('set_auto_backup_keep').value
        })
    });
    notify('备份设置已保存');
};

document.getElementById('btnBackupNow').onclick = async () => {
    await api('/api/system/backup-now', { method: 'POST' });
    notify('已生成备份');
};

async function loadInfo() {
    const s = await api('/api/storage/stats');
    document.getElementById('systemInfo').innerHTML = `
        <div class="mini-item"><span>存储库</span><strong>${s.warehouses}</strong></div>
        <div class="mini-item"><span>存储箱</span><strong>${s.boxes} / ${s.total_capacity} 格</strong></div>
        <div class="mini-item"><span>存储盒</span><strong>${s.bins}</strong></div>
    `;
}

async function loadBackupList() {
    const list = await api('/api/system/backups').catch(() => []);
    document.getElementById('backupList').innerHTML = list.map(b => `<div class="mini-item"><div>${b.name}</div><div class="dim">${b.mtime}</div></div>`).join('') || '<div class="dim">暂无备份</div>';
}



document.getElementById('btnClearAll').onclick = async () => {
    const code = prompt('危险操作！输入 DELETE_ALL 确认清空所有数据：');
    if (code === 'DELETE_ALL') {
        await api('/api/system/clear', { method: 'POST', body: JSON.stringify({ confirm: 'DELETE_ALL', keep_settings: true }) });
        notify('系统已清空'); loadInfo();
    }
};

// ================= 蓝牙功能 =================
let blePollingTimer = null;

async function checkBleStatus() {
    try {
        const status = await api('/api/ble/status');
        const statusEl = document.getElementById('bleStatus');
        const btnDisconnect = document.getElementById('btnBleDisconnect');
        const btnScan = document.getElementById('btnBleScan');

        if (!status.ready) {
            statusEl.innerHTML = `<span>当前状态</span><strong style="color:#e74c3c;">蓝牙未就绪</strong>`;
            btnScan.disabled = true;
            btnDisconnect.style.display = 'none';
            return;
        }

        if (status.connected) {
            statusEl.innerHTML = `<span>当前状态</span><strong style="color:#27ae60;">已连接: ${status.connected.name} (${status.connected.address})</strong>`;
            btnDisconnect.style.display = 'inline-flex';
            btnScan.disabled = true;
        } else {
            statusEl.innerHTML = `<span>当前状态</span><strong style="color:#e74c3c;">未连接</strong>`;
            btnDisconnect.style.display = 'none';
            btnScan.disabled = false;
        }
    } catch (e) {
        console.warn('获取蓝牙状态失败', e);
    }
}

document.getElementById('btnBleScan').onclick = async () => {
    const listEl = document.getElementById('bleScanList');
    listEl.innerHTML = '<div class="dim">正在扫描周边设备，请稍候(约5-8秒)...</div>';
    document.getElementById('btnBleScan').disabled = true;

    try {
        await api('/api/ble/scan', { method: 'POST' });

        // 轮询扫描状态和结果
        let pollCount = 0;
        const pollScan = setInterval(async () => {
            try {
                // 获取状态，看看后端扫完了没
                const status = await api('/api/ble/status');
                const results = await api('/api/ble/results');
                pollCount++;

                // 如果后端 scanning 变成 false，说明扫完了
                if (!status.scanning) {
                    clearInterval(pollScan);
                    document.getElementById('btnBleScan').disabled = false;

                    if (results.length === 0) {
                        listEl.innerHTML = '<div class="dim">未发现周边设备</div>';
                        return;
                    }

                    // 渲染结果列表
                    listEl.innerHTML = results.map(dev => `
                        <div class="mini-item">
                            <div>
                                <strong>${dev.name || '未知设备'}</strong>
                                <div class="dim">${dev.address}</div>
                            </div>
                            <button class="btn btn-primary btn-sm" onclick="connectBle('${dev.address}')">连接</button>
                        </div>
                    `).join('');
                }
                // 超时保护：如果等了 15 秒还没结束，强制停止
                else if (pollCount > 15) {
                    clearInterval(pollScan);
                    document.getElementById('btnBleScan').disabled = false;
                    listEl.innerHTML = '<div class="dim">扫描超时，请重试</div>';
                }
            } catch (e) {
                console.error("轮询出错", e);
            }
        }, 1000);

    } catch (e) {
        listEl.innerHTML = `<div style="color:red">扫描失败: ${e.message}</div>`;
        document.getElementById('btnBleScan').disabled = false;
    }
};

window.connectBle = async (address) => {
    const listEl = document.getElementById('bleScanList');
    listEl.innerHTML = '<div class="dim">正在连接设备...</div>';
    try {
        await api('/api/ble/connect', { method: 'POST', body: JSON.stringify({ address }) });
        notify('连接成功');
        listEl.innerHTML = '';
        checkBleStatus();
    } catch (e) {
        alert('连接失败: ' + e.message);
        document.getElementById('btnBleScan').disabled = false;
        listEl.innerHTML = '';
    }
};

document.getElementById('btnBleDisconnect').onclick = async () => {
    if (!confirm('确定断开当前蓝牙连接吗？')) return;
    try {
        await api('/api/ble/disconnect', { method: 'POST' });
        notify('已断开连接');
        checkBleStatus();
    } catch (e) { alert(e.message); }
};

// 初始化时检查状态，并每 5 秒轮询一次
checkBleStatus();
blePollingTimer = setInterval(checkBleStatus, 5000);