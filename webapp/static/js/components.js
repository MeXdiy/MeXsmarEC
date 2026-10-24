let currentView = 'all';
let currentKeyword = '';
let currentCategory = '';
let currentBoxId = '';
let currentPage = 1;
let PAGE_SIZE = 100;
let binCache = {};

// 初始化读取分页设置
(async () => {
    try {
        const s = await api('/api/settings');
        if (s.page_size) PAGE_SIZE = parseInt(s.page_size);
    } catch(e){}
    render();
})();

// 搜索与清空
document.getElementById('searchBtn').onclick = () => { currentKeyword = document.getElementById('listSearch').value.trim(); currentPage = 1; render(); };
document.getElementById('clearBtn').onclick = () => { document.getElementById('listSearch').value = ''; currentKeyword = ''; currentPage = 1; render(); };
document.getElementById('listSearch').addEventListener('keydown', e => {
    if (e.key === 'Enter') { currentKeyword = e.target.value.trim(); currentPage = 1; render(); }
});

// 视图切换
document.querySelectorAll('.view-switch button').forEach(btn => {
    btn.onclick = () => {
        document.querySelectorAll('.view-switch button').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        currentView = btn.dataset.view;
        currentPage = 1;
        document.getElementById('dynamicFilter').style.display = currentView === 'all' ? 'none' : 'block';
        render();
    };
});

async function render() {
    const c = document.getElementById('listContainer');
    c.innerHTML = '<div class="loading">加载中...</div>';
    const filterDiv = document.getElementById('dynamicFilter');
    
    let url = `/api/components?page=${currentPage}&page_size=${PAGE_SIZE}`;
    if (currentKeyword) url += `&q=${encodeURIComponent(currentKeyword)}`;

    // 处理分类/箱子的下拉框
    if (currentView === 'category') {
        if (!filterDiv.innerHTML || !document.getElementById('catSelect')) {
            const cats = await api('/api/meta/categories');
            filterDiv.innerHTML = `<select id="catSelect" style="width:200px">${cats.map(x => `<option value="${x}">${x}</option>`).join('')}</select>`;
            document.getElementById('catSelect').onchange = (e) => { currentCategory = e.target.value; currentPage = 1; render(); };
            if (!currentCategory) currentCategory = cats[0];
        }
        document.getElementById('catSelect').value = currentCategory || '';
        url += `&category=${encodeURIComponent(currentCategory)}`;
    } 
    else if (currentView === 'box') {
        if (!filterDiv.innerHTML || !document.getElementById('boxSelect')) {
            const boxes = await api('/api/boxes');
            filterDiv.innerHTML = `<select id="boxSelect" style="width:300px">${boxes.map(x => `<option value="${x.id}">${x.warehouse_name} / ${x.name}</option>`).join('')}</select>`;
            document.getElementById('boxSelect').onchange = (e) => { currentBoxId = e.target.value; currentPage = 1; render(); };
            if (!currentBoxId) currentBoxId = boxes[0]?.id;
        }
        document.getElementById('boxSelect').value = currentBoxId || '';
        if (currentBoxId) url += `&box_id=${currentBoxId}`;
    }

    try {
        const data = await api(url);
        if (!data.items.length) { c.innerHTML = '<div class="dim" style="padding:20px;text-align:center;">没有匹配的元件</div>'; document.getElementById('pagination').innerHTML = ''; return; }
        data.items.forEach(x => binCache[x.id] = x);
        c.innerHTML = `
            <table class="data-table">
                <thead>
                    <tr>
                        <th>名称</th>
                        <th>数量</th>
                        <th>位置</th>
                        <th class="col-id">ID</th>
                        <th class="col-opt">分类</th>
                        <th class="col-opt">封装</th>
                        <th class="col-opt">值</th>
                    </tr>
                </thead>
                <tbody>
                    ${data.items.map(x => `
                        <tr>
                            <td data-label="名称">
                                <a class="name-link" data-detail="${x.id}">${escapeHtml(x.name)}</a>
                                ${x.note ? `<div class="dim text-truncate">${escapeHtml(x.note)}</div>` : ''}
                            </td>
                            <td data-label="数量">${x.quantity}</td>
                            <td data-label="位置" class="text-truncate" style="max-width: 120px;">${escapeHtml(x.warehouse_name)} / ${escapeHtml(x.box_name)}</td>
                            <td class="col-id">${x.id}</td>
                            <td class="col-opt">${escapeHtml(x.category || '')}</td>
                            <td class="col-opt">${escapeHtml(x.package || '')}</td>
                            <td class="col-opt">${escapeHtml(x.value || '')}</td>
                        </tr>
                    `).join('')}
                </tbody>
            </table>
        `;
        renderPager(data);
    } catch (e) { c.innerHTML = `<div style="color:red">加载失败: ${e.message}</div>`; }
}

function renderPager(data) {
    if (data.total_pages <= 1) { document.getElementById('pagination').innerHTML = ''; return; }
    let btns = '';
    for (let i = 1; i <= data.total_pages; i++) {
        btns += `<button class="page-btn ${i === data.page ? 'active' : ''}" onclick="goPage(${i})">${i}</button>`;
    }
    document.getElementById('pagination').innerHTML = `
        <div class="dim">共 ${data.total} 条</div>
        <div style="display:flex;gap:4px;flex-wrap:wrap">
            <button class="page-btn" ${data.page === 1 ? 'disabled' : ''} onclick="goPage(${data.page - 1})">上一页</button>
            ${btns}
            <button class="page-btn" ${data.page === data.total_pages ? 'disabled' : ''} onclick="goPage(${data.page + 1})">下一页</button>
        </div>
    `;
}

window.goPage = (p) => { currentPage = p; render(); };

// 列表名称点击事件
document.getElementById('listContainer').onclick = (e) => {
    const link = e.target.closest('.name-link');
    if (!link) return;
    const c = binCache[link.dataset.detail];
    if (c) window.openComponentDetailModal(c, 'view');
};