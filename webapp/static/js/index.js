async function loadStats() {
    try {
        const s = await api('/api/storage/stats');
        document.getElementById('overviewStats').innerHTML = `
            <div class="stat-card"><div class="num">${s.warehouses}</div><div>存储库</div></div>
            <div class="stat-card"><div class="num">${s.boxes}</div><div>存储箱</div></div>
            <div class="stat-card"><div class="num">${s.bins}</div><div>存储盒</div></div>
            <div class="stat-card"><div class="num">${s.specs}</div><div>箱规格</div></div>
        `;
    } catch (e) { document.getElementById('overviewStats').innerHTML = `<div style="color:red">加载失败</div>`; }
}

async function loadRecent() {
    try {
        const s = await api('/api/settings');
        const limit = s.recent_limit || 10;
        const data = await api(`/api/components?page=1&page_size=${limit}`);
        if (!data.items.length) {
            document.getElementById('recentList').innerHTML = '<div class="dim">暂无数据</div>';
            return;
        }
        document.getElementById('recentList').innerHTML = data.items.map(c => `
            <div class="mini-item">
                <div>
                    <strong>${c.name}</strong> 
                    <span class="dim">${c.category || ''} ${c.package || ''} ${c.value || ''}</span>
                </div>
                <div class="dim">${c.warehouse_name} / ${c.box_name}</div>
            </div>
        `).join('');
    } catch (e) { document.getElementById('recentList').innerHTML = `<div style="color:red">加载失败</div>`; }
}

loadStats();
loadRecent();