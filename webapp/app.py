import os
import re
import sqlite3
import json
import shutil
import time as _time
import threading
from datetime import datetime
from flask import Flask, jsonify, request, render_template, g, send_file , Response
import queue
from ble_manager import BLEManager

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATABASE = os.path.join(BASE_DIR, 'component.db')
BACKUP_DIR = os.path.join(BASE_DIR, 'backups')

app = Flask(__name__, static_folder='static', static_url_path='', template_folder='templates')

@app.after_request
def _no_cache_static(resp):
    if request.path.startswith(('/js/', '/css/')):
        resp.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
        resp.headers['Pragma'] = 'no-cache'
        resp.headers['Expires'] = '0'
    return resp

from collections import deque
from time import time as _now

# ============================================================
# 事件总线（广播模式，多个 SSE 客户端互不干扰）
# ============================================================
class EventBus:
    def __init__(self):
        self._clients = []
        self._lock = threading.Lock()
        self._recent = deque(maxlen=100)

    def put(self, data):
        with self._lock:
            if data.get('type') == 'nfc_scan':
                self._recent.append((_now(), data))
            clients = list(self._clients)
        for q in clients:
            try:
                q.put_nowait(data)
            except queue.Full:
                pass

    def subscribe(self):
        q = queue.Queue(maxsize=200)
        with self._lock:
            self._clients.append(q)
            recent = [d for ts, d in self._recent if _now() - ts < 5.0]
        return q, recent

    def unsubscribe(self, q):
        with self._lock:
            if q in self._clients:
                self._clients.remove(q)


event_bus = EventBus()
ble_mgr = BLEManager(event_bus)
ble_mgr.start()

# ============================================================
# 数据库初始化
# ============================================================
def _ensure_schema():
    is_new = not os.path.exists(DATABASE)
    conn = sqlite3.connect(DATABASE)
    try:
        # 👇 关键修复：将 WAL 模式放在最前面，避免隐式事务报错
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA foreign_keys = ON")
        
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS warehouse (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL, nfc_uid TEXT UNIQUE, ble_mac TEXT, led_index INTEGER, note TEXT
            );
            CREATE TABLE IF NOT EXISTS box_spec (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE, capacity INTEGER NOT NULL, note TEXT
            );
            CREATE TABLE IF NOT EXISTS box (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                warehouse_id INTEGER NOT NULL, spec_id INTEGER NOT NULL,
                name TEXT NOT NULL, nfc_uid TEXT UNIQUE, ble_mac TEXT, led_index INTEGER,
                pref_category TEXT, pref_package TEXT, note TEXT,
                FOREIGN KEY(warehouse_id) REFERENCES warehouse(id) ON DELETE CASCADE,
                FOREIGN KEY(spec_id) REFERENCES box_spec(id)
            );
            CREATE TABLE IF NOT EXISTS bin (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                box_id INTEGER NOT NULL, slot_index INTEGER NOT NULL,
                name TEXT NOT NULL, category TEXT, package TEXT, value TEXT, quantity INTEGER DEFAULT 0,
                nfc_uid TEXT UNIQUE, led_index INTEGER, note TEXT,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY(box_id) REFERENCES box(id) ON DELETE CASCADE,
                UNIQUE(box_id, slot_index)
            );
            CREATE TABLE IF NOT EXISTS bin_spec (
                id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE, note TEXT
            );
            CREATE TABLE IF NOT EXISTS bin_spec_box_spec (
                bin_spec_id INTEGER NOT NULL, box_spec_id INTEGER NOT NULL,
                PRIMARY KEY (bin_spec_id, box_spec_id),
                FOREIGN KEY(bin_spec_id) REFERENCES bin_spec(id) ON DELETE CASCADE,
                FOREIGN KEY(box_spec_id) REFERENCES box_spec(id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS box_spec_warehouse (
                box_spec_id INTEGER NOT NULL, warehouse_id INTEGER NOT NULL,
                PRIMARY KEY (box_spec_id, warehouse_id),
                FOREIGN KEY(box_spec_id) REFERENCES box_spec(id) ON DELETE CASCADE,
                FOREIGN KEY(warehouse_id) REFERENCES warehouse(id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);

            CREATE INDEX IF NOT EXISTS idx_bin_name ON bin(name);
            CREATE INDEX IF NOT EXISTS idx_bin_category ON bin(category);
            CREATE INDEX IF NOT EXISTS idx_bin_package ON bin(package);
            CREATE INDEX IF NOT EXISTS idx_bin_value ON bin(value);
            CREATE INDEX IF NOT EXISTS idx_bin_box ON bin(box_id);
            CREATE INDEX IF NOT EXISTS idx_bin_nfc ON bin(nfc_uid);
        """)
        conn.executescript("""
            INSERT OR IGNORE INTO box_spec(name, capacity, note) VALUES
                ('100格标准箱', 100, '适合一般元件'), ('50格小箱', 50, '适合常用元件');
            INSERT OR IGNORE INTO settings(key, value) VALUES
                ('default_warehouse_id', ''), ('default_spec_id', ''),
                ('default_quantity', '0'), ('recent_limit', '10'),
                ('page_size', '100'), ('auto_backup_enabled', '0'),
                ('auto_backup_interval', '60'), ('auto_backup_keep', '10'),
                ('led_r', '0'), ('led_g', '255'), ('led_b', '0'), ('led_brightness', '255');
        """)
        if is_new:
            row = conn.execute("SELECT COUNT(*) FROM warehouse").fetchone()
            if row[0] == 0:
                conn.execute("INSERT INTO warehouse(name, ble_mac) VALUES (?, ?)",
                             ('一号元件库', 'AA:BB:CC:DD:EE:01'))
        conn.commit()
        
        # 清理脏数据，并在执行完后显式提交事务
        conn.execute("UPDATE warehouse SET nfc_uid = NULL WHERE nfc_uid = ''")
        conn.execute("UPDATE box       SET nfc_uid = NULL WHERE nfc_uid = ''")
        conn.execute("UPDATE bin       SET nfc_uid = NULL WHERE nfc_uid = ''")
        conn.commit()
        
        print(f"✅ 数据库初始化完成: {DATABASE}")
        
    except Exception as e:
        print(f"❌ 数据库初始化失败: {e}")
        raise
    finally:
        conn.close()


_ensure_schema()

def get_db():
    if 'db' not in g:
        conn = sqlite3.connect(DATABASE, timeout=10)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        conn.execute("PRAGMA busy_timeout = 10000")
        g.db = conn
    return g.db

@app.teardown_appcontext
def close_db(exc):
    db = g.pop('db', None)
    if db is not None: db.close()

def _list(rows): return [dict(r) for r in rows]
def _int_or_none(v):
    if v in (None, ''): return None
    try: return int(v)
    except (TypeError, ValueError): raise ValueError('必须是整数')

def _unique_check(db, table, field, value, exclude_id=None):
    if not value: return True
    if exclude_id is None:
        row = db.execute(f"SELECT id FROM {table} WHERE {field}=?", (value,)).fetchone()
    else:
        row = db.execute(f"SELECT id FROM {table} WHERE {field}=? AND id!=?", (value, exclude_id)).fetchone()
    return row is None

# ============================================================
# 页面路由
# ============================================================
@app.route('/')
def index_page(): return render_template('index.html', active_page='index')
@app.route('/components')
def components_page(): return render_template('components.html', active_page='components')
@app.route('/components/add')
def add_component_page(): return render_template('add_component.html', active_page='add')
@app.route('/storage')
def storage_page(): return render_template('storage.html', active_page='storage')
@app.route('/settings')
def settings_page(): return render_template('settings.html', active_page='settings')

# ============================================================
# BLE 接口
# ============================================================
@app.route('/api/ble/status', methods=['GET'])
def api_ble_status():
    return jsonify({
        'ready': ble_mgr.is_ready,
        'scanning': ble_mgr.scanning,
        'connected': ble_mgr.connected_device
    })

@app.route('/api/ble/scan', methods=['POST'])
def api_ble_scan():
    ble_mgr.trigger_scan()
    return jsonify({'status': 'scanning'})

@app.route('/api/ble/results', methods=['GET'])
def api_ble_results():
    return jsonify(ble_mgr.get_results())

@app.route('/api/ble/connect', methods=['POST'])
def api_ble_connect():
    data = request.get_json(silent=True) or {}
    address = data.get('address')
    if not address:
        return jsonify({'error': '缺少设备地址'}), 400
    ble_mgr.connect(address)
    return jsonify({'status': 'connecting'})

@app.route('/api/ble/disconnect', methods=['POST'])
def api_ble_disconnect():
    ble_mgr.disconnect()
    return jsonify({'status': 'disconnected'})

# ============================================================
# 存储库 (Warehouse) API
# ============================================================
@app.route('/api/warehouses', methods=['GET'])
def warehouses_list():
    db = get_db()
    rows = db.execute("""
        SELECT w.*, (SELECT COUNT(*) FROM box WHERE warehouse_id = w.id) AS box_count
        FROM warehouse w ORDER BY w.id
    """).fetchall()
    return jsonify(_list(rows))

@app.route('/api/warehouses', methods=['POST'])
def warehouses_add():
    data = request.get_json(silent=True) or {}
    name = (data.get('name') or '').strip()
    if not name: return jsonify({'error': 'name 不能为空'}), 400
    try: led_index = _int_or_none(data.get('led_index'))
    except ValueError: return jsonify({'error': 'LED索引错误'}), 400

    def _s(v):
        v = (v or '').strip()
        return v or None
    nfc_uid = _s(data.get('nfc_uid'))
    ble_mac = _s(data.get('ble_mac'))

    db = get_db()
    if not _unique_check(db, 'warehouse', 'nfc_uid', nfc_uid):
        return jsonify({'error': 'NFC已被占用'}), 409
    try:
        cur = db.execute("INSERT INTO warehouse(name, nfc_uid, ble_mac, led_index) VALUES (?,?,?,?)",
            (name, nfc_uid, ble_mac, led_index))
        db.commit()
        return jsonify({'id': cur.lastrowid, 'status': 'created'}), 201
    except sqlite3.IntegrityError as e:
        return jsonify({'error': str(e)}), 409

@app.route('/api/warehouses/<int:wid>', methods=['PUT', 'DELETE'])
def warehouses_modify(wid):
    db = get_db()
    if request.method == 'DELETE':
        db.execute("DELETE FROM warehouse WHERE id=?", (wid,))
        db.commit()
        return jsonify({'status': 'deleted'})
    
    data = request.get_json(silent=True) or {}
    fields, params = [], []
    for k in ('name', 'nfc_uid', 'ble_mac', 'note'):
        if k in data:
            fields.append(f'{k}=?'); params.append((data[k] or '').strip() or None)
    if 'led_index' in data:
        fields.append('led_index=?'); params.append(_int_or_none(data['led_index']))
    
    if fields:
        params.append(wid)
        db.execute(f"UPDATE warehouse SET {', '.join(fields)} WHERE id=?", params)
        db.commit()
    return jsonify({'status': 'updated'})

# ============================================================
# 箱规格 (Box Spec) API
# ============================================================
@app.route('/api/box-specs', methods=['GET'])
def box_specs_list():
    db = get_db()
    rows = _list(db.execute("SELECT s.*, (SELECT COUNT(*) FROM box WHERE spec_id=s.id) AS box_count FROM box_spec s").fetchall())
    for s in rows:
        whs = db.execute("SELECT warehouse_id FROM box_spec_warehouse WHERE box_spec_id=?", (s['id'],)).fetchall()
        s['warehouse_ids'] = [r['warehouse_id'] for r in whs]
    return jsonify(rows)

@app.route('/api/box-specs', methods=['POST'])
@app.route('/api/box-specs/<int:sid>', methods=['PUT', 'DELETE'])
def box_specs_modify(sid=None):
    db = get_db()
    if request.method == 'DELETE':
        cnt = db.execute("SELECT COUNT(*) FROM box WHERE spec_id=?", (sid,)).fetchone()[0]
        if cnt: return jsonify({'error': '有存储箱正在使用该规格'}), 409
        db.execute("DELETE FROM box_spec WHERE id=?", (sid,))
        db.commit()
        return jsonify({'status': 'deleted'})

    data = request.get_json(silent=True) or {}
    name = (data.get('name') or '').strip()
    capacity = data.get('capacity')
    
    if request.method == 'POST':
        if not name: return jsonify({'error': '名称必填'}), 400
        try:
            cur = db.execute("INSERT INTO box_spec(name, capacity, note) VALUES (?,?,?)", (name, int(capacity), data.get('note')))
            sid = cur.lastrowid
        except sqlite3.IntegrityError: return jsonify({'error': '规格名重复'}), 409
    else:
        fields, params = [], []
        if name: fields.append('name=?'); params.append(name)
        if capacity: fields.append('capacity=?'); params.append(int(capacity))
        if 'note' in data: fields.append('note=?'); params.append((data.get('note') or '').strip() or None)
        if fields:
            params.append(sid)
            db.execute(f"UPDATE box_spec SET {', '.join(fields)} WHERE id=?", params)
    
    if 'warehouse_ids' in data:
        db.execute("DELETE FROM box_spec_warehouse WHERE box_spec_id=?", (sid,))
        for wid in data['warehouse_ids']:
            db.execute("INSERT INTO box_spec_warehouse(box_spec_id, warehouse_id) VALUES (?,?)", (sid, int(wid)))
            
    db.commit()
    return jsonify({'status': 'ok', 'id': sid}), (201 if request.method == 'POST' else 200)

# ============================================================
# 盒规格 (Bin Spec) API
# ============================================================
@app.route('/api/bin-specs', methods=['GET'])
def bin_specs_list():
    db = get_db()
    rows = _list(db.execute("SELECT * FROM bin_spec ORDER BY id").fetchall())
    for s in rows:
        bss = db.execute("SELECT box_spec_id FROM bin_spec_box_spec WHERE bin_spec_id=?", (s['id'],)).fetchall()
        s['box_spec_ids'] = [r['box_spec_id'] for r in bss]
    return jsonify(rows)

@app.route('/api/bin-specs', methods=['POST'])
@app.route('/api/bin-specs/<int:sid>', methods=['PUT', 'DELETE'])
def bin_specs_modify(sid=None):
    db = get_db()

    if request.method == 'DELETE':
        db.execute("DELETE FROM bin_spec WHERE id=?", (sid,))
        db.commit()
        return jsonify({'status': 'deleted'})

    data = request.get_json(silent=True) or {}
    name = (data.get('name') or '').strip()
    note = (data.get('note') or '').strip() or None

    if request.method == 'POST':
        if not name: return jsonify({'error': '名称必填'}), 400
        try:
            cur = db.execute("INSERT INTO bin_spec(name, note) VALUES (?,?)", (name, note))
            sid = cur.lastrowid
        except sqlite3.IntegrityError: return jsonify({'error': '盒规格名重复'}), 409
    else:
        fields, params = [], []
        if name: fields.append('name=?'); params.append(name)
        if 'note' in data: fields.append('note=?'); params.append(note)
        if fields:
            params.append(sid)
            db.execute(f"UPDATE bin_spec SET {', '.join(fields)} WHERE id=?", params)

    if 'box_spec_ids' in data:
        db.execute("DELETE FROM bin_spec_box_spec WHERE bin_spec_id=?", (sid,))
        for bsid in (data.get('box_spec_ids') or []):
            db.execute("INSERT OR IGNORE INTO bin_spec_box_spec(bin_spec_id, box_spec_id) VALUES (?,?)", (sid, int(bsid)))

    db.commit()
    return jsonify({'status': 'ok', 'id': sid}), (201 if request.method == 'POST' else 200)

# ============================================================
# 存储箱 (Box) API
# ============================================================
@app.route('/api/boxes', methods=['GET'])
def boxes_list():
    db = get_db()
    rows = db.execute("""
        SELECT b.*, s.name AS spec_name, s.capacity, w.name AS warehouse_name,
               (SELECT COUNT(*) FROM bin WHERE box_id = b.id) AS used_count
        FROM box b JOIN box_spec s ON b.spec_id = s.id JOIN warehouse w ON b.warehouse_id = w.id
    """).fetchall()
    return jsonify(_list(rows))

@app.route('/api/boxes', methods=['POST'])
@app.route('/api/boxes/<int:bid>', methods=['PUT', 'DELETE'])
def boxes_modify(bid=None):
    db = get_db()
    
    if request.method == 'GET': return jsonify({'error': '不支持的方法'}), 405
    
    if request.method == 'DELETE':
        mode = request.args.get('mode')
        target_box_id = request.args.get('target_box_id')
        row = db.execute("SELECT b.*, s.capacity FROM box b JOIN box_spec s ON b.spec_id = s.id WHERE b.id=?", (bid,)).fetchone()
        if not row: return jsonify({'error': '箱不存在'}), 404
        
        bins = db.execute("SELECT * FROM bin WHERE box_id=? ORDER BY slot_index", (bid,)).fetchall()
        if not bins:
            db.execute("DELETE FROM box WHERE id=?", (bid,)); db.commit()
            return jsonify({'status': 'deleted', 'removed_bins': 0, 'moved': 0})
        
        if not mode: return jsonify({'error': '有元件存在', 'bin_count': len(bins), 'options': ['delete_all', 'auto', 'manual']}), 409
        
        if mode == 'delete_all':
            db.execute("DELETE FROM box WHERE id=?", (bid,)); db.commit()
            return jsonify({'status': 'deleted', 'removed_bins': len(bins), 'moved': 0})
            
        if mode == 'auto':
            candidates = db.execute("""
                SELECT b.id, b.name, s.capacity, (SELECT COUNT(*) FROM bin WHERE box_id = b.id) AS used
                FROM box b JOIN box_spec s ON b.spec_id = s.id WHERE b.spec_id = ? AND b.id != ?
            """, (row['spec_id'], bid)).fetchall()
            available = []
            for c in candidates:
                used = set(r[0] for r in db.execute("SELECT slot_index FROM bin WHERE box_id=?", (c['id'],)).fetchall())
                free = [i for i in range(c['capacity']) if i not in used]
                if free: available.append({'id': c['id'], 'free': free})
            
            if sum(len(a['free']) for a in available) < len(bins):
                return jsonify({'error': '空位不足', 'need_manual': True}), 409
            
            for b in bins:
                for a in available:
                    if a['free']:
                        slot = a['free'].pop(0)
                        db.execute("UPDATE bin SET box_id=?, slot_index=? WHERE id=?", (a['id'], slot, b['id']))
                        break
            db.execute("DELETE FROM box WHERE id=?", (bid,)); db.commit()
            return jsonify({'status': 'deleted', 'moved': len(bins)})
            
        if mode == 'manual':
            t_id = int(target_box_id or 0)
            target = db.execute("SELECT b.*, s.capacity FROM box b JOIN box_spec s ON b.spec_id=s.id WHERE b.id=?", (t_id,)).fetchone()
            if not target: return jsonify({'error': '目标箱不存在'}), 400
            used = set(r[0] for r in db.execute("SELECT slot_index FROM bin WHERE box_id=?", (t_id,)).fetchall())
            free = [i for i in range(target['capacity']) if i not in used]
            if len(free) < len(bins): return jsonify({'error': '目标箱空位不足'}), 409
            
            for i, b in enumerate(bins):
                db.execute("UPDATE bin SET box_id=?, slot_index=? WHERE id=?", (t_id, free[i], b['id']))
            db.execute("DELETE FROM box WHERE id=?", (bid,)); db.commit()
            return jsonify({'status': 'deleted', 'moved': len(bins)})

    data = request.get_json(silent=True) or {}
    fields, params = [], []
    
    if request.method == 'POST':
        def _s(v):
            v = (v or '').strip()
            return v or None
        try:
            cur = db.execute(
                "INSERT INTO box(warehouse_id, spec_id, name, nfc_uid, ble_mac, led_index, pref_category, pref_package) "
                "VALUES (?,?,?,?,?,?,?,?)",
                (data.get('warehouse_id'), data.get('spec_id'), data.get('name'),
                _s(data.get('nfc_uid')), _s(data.get('ble_mac')),
                _int_or_none(data.get('led_index')),
                _s(data.get('pref_category')), _s(data.get('pref_package')))
            )
            db.commit()
            return jsonify({'id': cur.lastrowid}), 201
        except sqlite3.IntegrityError as e:
            return jsonify({'error': str(e)}), 409

    for k in ('name', 'nfc_uid', 'ble_mac', 'pref_category', 'pref_package', 'note'):
        if k in data: fields.append(f'{k}=?'); params.append((data[k] or '').strip() or None)
    for k in ('warehouse_id', 'spec_id', 'led_index'):
        if k in data: fields.append(f'{k}=?'); params.append(_int_or_none(data[k]))
    
    if fields:
        params.append(bid)
        db.execute(f"UPDATE box SET {', '.join(fields)} WHERE id=?", params)
        db.commit()
    return jsonify({'status': 'updated'})

# ============================================================
# 存储盒 (Bin) & 元件 API
# ============================================================
def _first_free_slot(db, box_id, capacity):
    used = set(r[0] for r in db.execute("SELECT slot_index FROM bin WHERE box_id=?", (box_id,)).fetchall())
    for i in range(capacity):
        if i not in used: return i
    return None

@app.route('/api/components', methods=['GET'])
def components_search():
    q = (request.args.get('q') or '').strip()
    category = (request.args.get('category') or '').strip()
    box_id = request.args.get('box_id')
    page = max(1, int(request.args.get('page') or 1))
    page_size = max(1, min(int(request.args.get('page_size') or 100), 500))
    
    where, params = [], []
    if q:
        tokens = [t for t in re.split(r'[\s,,、]+', q) if t][:8]
        for tok in tokens:
            pat = f'%{tok}%'
            where.append("(b.name LIKE ? OR b.value LIKE ? OR b.package LIKE ? OR b.category LIKE ?)")
            params.extend([pat, pat, pat, pat])
    if category:
        if category == '未分类': where.append("(b.category IS NULL OR b.category = '')")
        else: where.append("b.category=?"); params.append(category)
    if box_id: where.append("b.box_id=?"); params.append(int(box_id))
    
    where_sql = (' WHERE ' + ' AND '.join(where)) if where else ''
    db = get_db()
    
    total = db.execute("SELECT COUNT(*) FROM bin b" + where_sql, params).fetchone()[0]
    total_pages = (total + page_size - 1) // page_size if total > 0 else 1
    page = min(page, total_pages)
    
    sql = f"""
        SELECT b.*, bx.name AS box_name, bx.led_index AS box_led, bx.ble_mac AS box_mac,
               w.name AS warehouse_name, w.led_index AS wh_led, w.ble_mac AS wh_mac
        FROM bin b JOIN box bx ON b.box_id = bx.id JOIN warehouse w ON bx.warehouse_id = w.id
        {where_sql} ORDER BY b.id DESC LIMIT ? OFFSET ?
    """
    rows = db.execute(sql, params + [page_size, (page-1)*page_size]).fetchall()
    return jsonify({'items': _list(rows), 'total': total, 'page': page, 'page_size': page_size, 'total_pages': total_pages})

@app.route('/api/components', methods=['POST'])
def components_add():
    data = request.get_json(silent=True) or {}
    name = (data.get('name') or '').strip()
    if not name:
        return jsonify({'error': '名称不能为空'}), 400
    try:
        qty = int(data.get('quantity') or 0)
    except Exception:
        return jsonify({'error': '数量错误'}), 400

    nfc = (data.get('nfc_uid') or '').strip() or None
    db = get_db()
    prefer_box_id = data.get('box_id')

    if prefer_box_id:
        box = db.execute(
            "SELECT b.*, s.capacity FROM box b JOIN box_spec s ON b.spec_id = s.id WHERE b.id=?",
            (prefer_box_id,)
        ).fetchone()
        if not box: return jsonify({'error': '指定箱不存在'}), 400
        used = db.execute("SELECT COUNT(*) FROM bin WHERE box_id=?", (prefer_box_id,)).fetchone()[0]
        if used >= box['capacity']: return jsonify({'error': '指定箱已满'}), 409
        box_id = prefer_box_id
        capacity = box['capacity']
    else:
        cat, pkg = data.get('category'), data.get('package')
        candidates = db.execute("""
            SELECT b.id, b.name, s.capacity, b.pref_category, b.pref_package,
                   (SELECT COUNT(*) FROM bin WHERE box_id = b.id) AS used
            FROM box b JOIN box_spec s ON b.spec_id = s.id
            WHERE (SELECT COUNT(*) FROM bin WHERE box_id = b.id) < s.capacity
        """).fetchall()
        if not candidates: return jsonify({'error': '没有可用的存储箱'}), 409

        def _score(c):
            s = 0
            if cat and c['pref_category'] == cat: s += 2
            if pkg and c['pref_package'] == pkg: s += 1
            return -s

        candidates = sorted(candidates, key=lambda c: (_score(c), c['used']))
        box_id = candidates[0]['id']
        capacity = candidates[0]['capacity']

    if not _unique_check(db, 'bin', 'nfc_uid', nfc):
        return jsonify({'error': 'NFC占用'}), 409

    import time as _t
    last_err = None
    for attempt in range(3):
        slot = _first_free_slot(db, box_id, capacity)
        if slot is None:
            return jsonify({'error': '该箱已无空闲格位'}), 409
        try:
            cur = db.execute("""
                INSERT INTO bin(box_id, slot_index, name, category, package,
                                value, quantity, nfc_uid, note)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (box_id, slot, name, data.get('category'), data.get('package'),
                  data.get('value'), qty, nfc, data.get('note')))
            db.commit()
            return jsonify({'id': cur.lastrowid, 'box_id': box_id,
                            'slot_index': slot, 'status': 'created'}), 201
        except sqlite3.IntegrityError as e:
            db.rollback()
            last_err = str(e)
            if 'box_id, slot_index' in last_err and attempt < 2:
                _t.sleep(0.05)
                continue
            if 'nfc_uid' in last_err:
                return jsonify({'error': 'NFC占用'}), 409
            return jsonify({'error': last_err}), 409
    return jsonify({'error': last_err or '分配失败'}), 409

@app.route('/api/bins/<int:bid>', methods=['PUT', 'DELETE'])
def bins_modify(bid):
    db = get_db()
    if request.method == 'DELETE':
        db.execute("DELETE FROM bin WHERE id=?", (bid,)); db.commit()
        return jsonify({'status': 'deleted'})
        
    data = request.get_json(silent=True) or {}
    fields, params = [], []
    for k in ('name', 'category', 'package', 'value', 'note', 'nfc_uid'):
        if k in data: fields.append(f'{k}=?'); params.append((data[k] or '').strip() or None)
    for k in ('quantity', 'led_index'):
        if k in data: fields.append(f'{k}=?'); params.append(_int_or_none(data[k]))
        
    moved = False
    if 'box_id' in data:
        nid = int(data['box_id'])
        row = db.execute("SELECT box_id FROM bin WHERE id=?", (bid,)).fetchone()
        if nid != row['box_id']:
            box = db.execute("SELECT capacity FROM box_spec WHERE id=(SELECT spec_id FROM box WHERE id=?)", (nid,)).fetchone()
            slot = _first_free_slot(db, nid, box['capacity'])
            if slot is None: return jsonify({'error': '目标箱已满'}), 409
            fields.extend(['box_id=?', 'slot_index=?']); params.extend([nid, slot])
            moved = True
            
    if fields:
        params.append(bid)
        db.execute(f"UPDATE bin SET {', '.join(fields)} WHERE id=?", params)
        db.commit()
    return jsonify({'status': 'updated', 'moved': moved})

# ============================================================
# NFC 查询 & 元数据 & 统计
# ============================================================
@app.route('/api/nfc/<path:uid>', methods=['GET'])
def api_nfc_lookup(uid):
    db = get_db()
    row = db.execute("""
        SELECT b.id, b.name, b.category, b.package, b.value, b.quantity, b.slot_index, b.nfc_uid,
               bx.name AS box_name, w.name AS wh_name
        FROM bin b JOIN box bx ON b.box_id=bx.id JOIN warehouse w ON bx.warehouse_id=w.id
        WHERE b.nfc_uid=?
    """, (uid,)).fetchone()
    if row: return jsonify({'type': 'bin', 'data': dict(row), 'lights': []})
    row = db.execute("SELECT b.*, s.name AS spec_name, s.capacity, w.name AS wh_name FROM box b JOIN box_spec s ON b.spec_id=s.id JOIN warehouse w ON b.warehouse_id=w.id WHERE b.nfc_uid=?", (uid,)).fetchone()
    if row: return jsonify({'type': 'box', 'data': dict(row), 'lights': []})
    row = db.execute("SELECT * FROM warehouse WHERE nfc_uid=?", (uid,)).fetchone()
    if row: return jsonify({'type': 'warehouse', 'data': dict(row), 'lights': []})
    return jsonify({'error': '未找到设备'}), 404

def _merge_preset_and_used(preset, used):
    seen, res = set(), []
    for v in preset + used:
        if v and v not in seen: res.append(v); seen.add(v)
    return res

@app.route('/api/meta/categories')
def meta_categories():
    cats = ["电阻", "电容", "电感", "二极管", "三极管", "MOSFET", "MCU", "连接器", "传感器"]
    db = get_db()
    used = [r['v'] for r in db.execute("SELECT DISTINCT category AS v FROM bin WHERE category IS NOT NULL AND category != ''").fetchall()]
    return jsonify(_merge_preset_and_used(cats, used))

@app.route('/api/meta/packages')
def meta_packages():
    pkgs = ["0201", "0402", "0603", "0805", "1206", "SOT-23", "SOP-8", "QFN", "直插"]
    db = get_db()
    used = [r['v'] for r in db.execute("SELECT DISTINCT package AS v FROM bin WHERE package IS NOT NULL AND package != ''").fetchall()]
    return jsonify(_merge_preset_and_used(pkgs, used))

@app.route('/api/storage/stats')
def api_storage_stats():
    db = get_db()
    return jsonify({
        'warehouses': db.execute("SELECT COUNT(*) FROM warehouse").fetchone()[0],
        'boxes': db.execute("SELECT COUNT(*) FROM box").fetchone()[0],
        'bins': db.execute("SELECT COUNT(*) FROM bin").fetchone()[0],
        'specs': db.execute("SELECT COUNT(*) FROM box_spec").fetchone()[0],
        'total_capacity': db.execute("SELECT COALESCE(SUM(s.capacity),0) FROM box b JOIN box_spec s ON b.spec_id=s.id").fetchone()[0],
    })

# ============================================================
# 系统与设置
# ============================================================
def _get_setting_int(key, default=0):
    try:
        conn = sqlite3.connect(DATABASE)
        row = conn.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        conn.close()
        if row and row[0]: return int(row[0])
    except: pass
    return default

def _backup_once():
    os.makedirs(BACKUP_DIR, exist_ok=True)
    ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    dst = os.path.join(BACKUP_DIR, f'component_{ts}.db')
    try:
        src_conn = sqlite3.connect(DATABASE)
        dst_conn = sqlite3.connect(dst)
        with dst_conn: src_conn.backup(dst_conn)
        dst_conn.close(); src_conn.close()
        
        keep = _get_setting_int('auto_backup_keep', 10)
        files = sorted([f for f in os.listdir(BACKUP_DIR) if f.startswith('component_')], reverse=True)
        for old in files[keep:]: os.remove(os.path.join(BACKUP_DIR, old))
        return True
    except Exception as e: return False

@app.route('/api/settings', methods=['GET', 'PUT'])
def api_settings():
    db = get_db()
    if request.method == 'PUT':
        for k, v in (request.json or {}).items():
            db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES (?,?)", (k, str(v)))
        db.commit()
        return jsonify({'status': 'ok'})
    return jsonify({r['key']: r['value'] for r in db.execute("SELECT key,value FROM settings").fetchall()})

@app.route('/api/system/backup-now', methods=['POST'])
def api_backup_now():
    return jsonify({'status': 'ok'}) if _backup_once() else (jsonify({'error': '备份失败'}), 500)

@app.route('/api/system/backups', methods=['GET'])
def api_system_backups():
    if not os.path.exists(BACKUP_DIR):
        return jsonify([])
    result = []
    for f in sorted(os.listdir(BACKUP_DIR), reverse=True):
        if f.startswith('component_') and f.endswith('.db'):
            full_path = os.path.join(BACKUP_DIR, f)
            try:
                stat = os.stat(full_path)
                result.append({
                    'name': f,
                    'size': stat.st_size,
                    'mtime': datetime.fromtimestamp(stat.st_mtime).strftime('%Y-%m-%d %H:%M:%S')
                })
            except Exception:
                pass
    return jsonify(result)

@app.route('/api/system/clear', methods=['POST'])
def api_system_clear():
    if request.json.get('confirm') != 'DELETE_ALL': return jsonify({'error': '确认码错误'}), 400
    db = get_db()
    db.execute("DELETE FROM bin"); db.execute("DELETE FROM box"); db.execute("DELETE FROM warehouse")
    if not request.json.get('keep_settings'): db.execute("DELETE FROM settings")
    db.execute("DELETE FROM sqlite_sequence")
    db.commit()
    return jsonify({'status': 'ok'})

# ============================================================
# 后台自动备份线程
# ============================================================
def _auto_backup_worker():
    last_backup = 0
    while True:
        try:
            enabled = str(_get_setting_int('auto_backup_enabled', 0)) == '1'
            interval = _get_setting_int('auto_backup_interval', 60)
            if enabled and interval > 0:
                if _time.time() - last_backup >= interval * 60:
                    if _backup_once(): last_backup = _time.time()
        except: pass
        _time.sleep(30)

if not app.debug or os.environ.get('WERKZEUG_RUN_MAIN') == 'true':
    threading.Thread(target=_auto_backup_worker, daemon=True).start()


# ============================================================
# 实时事件推送 (SSE)
# ============================================================
@app.route('/api/events')
def sse_events_stream():
    q, recent = event_bus.subscribe()

    def event_stream():
        try:
            yield f"data: {json.dumps({'type': 'connected'})}\n\n"
            for ev in recent:
                yield f"data: {json.dumps(ev)}\n\n"
            while True:
                try:
                    data = q.get(timeout=20)
                    yield f"data: {json.dumps(data)}\n\n"
                except queue.Empty:
                    yield ": keep-alive\n\n"
        finally:
            event_bus.unsubscribe(q)

    return Response(
        event_stream(),
        mimetype="text/event-stream",
        headers={'Cache-Control': 'no-cache, no-transform', 'X-Accel-Buffering': 'no'}
    )

# ============================================================
# LED控制与定位 API
# ============================================================
def _get_led_color(db):
    def _get_val(k, default):
        row = db.execute("SELECT value FROM settings WHERE key=?", (k,)).fetchone()
        return int(row[0]) if row and row[0] else default
    
    r = _get_val('led_r', 0)
    g = _get_val('led_g', 255)
    b = _get_val('led_b', 0)
    bright = _get_val('led_brightness', 255)
    return r, g, b, bright

@app.route('/api/warehouses/<int:wid>/locate', methods=['POST'])
def api_warehouse_locate(wid):
    db = get_db()
    row = db.execute("SELECT * FROM warehouse WHERE id=?", (wid,)).fetchone()
    if not row: return jsonify({'error': '不存在'}), 404
    
    r, g, b, bright = _get_led_color(db)
    lights = []
    
    # 直接遍历箱下所有的存储箱，点亮它们的 0 号灯
    boxes = db.execute("SELECT * FROM box WHERE warehouse_id=?", (wid,)).fetchall()
    for box in boxes:
        if box['ble_mac']:
            ble_mgr.send_led_bitmap(box['ble_mac'], [0], r, g, b, bright)
            lights.append({'ble_mac': box['ble_mac'], 'led_index': 0, 'name': box['name']})
            
    return jsonify({'lights': lights})

@app.route('/api/boxes/<int:bid>/locate', methods=['POST'])
def api_box_locate(bid):
    db = get_db()
    row = db.execute("SELECT * FROM box WHERE id=?", (bid,)).fetchone()
    if not row: return jsonify({'error': '箱不存在'}), 404
    
    r, g, b, bright = _get_led_color(db)
    lights = []
    
    if row['ble_mac']:
        ble_mgr.send_led_bitmap(row['ble_mac'], [0], r, g, b, bright)
        lights.append({'ble_mac': row['ble_mac'], 'led_index': 0, 'name': row['name']})
        
    return jsonify({'lights': lights})

@app.route('/api/components/<int:cid>/locate', methods=['POST'])
def components_locate(cid):
    db = get_db()
    # 注意这里的 SQL 增加了 b.slot_index
    row = db.execute("""
        SELECT b.id AS bin_id, b.name AS bin_name, b.led_index AS bin_led, b.slot_index,
               bx.name AS box_name, bx.ble_mac AS box_mac,
               w.name AS wh_name, w.led_index AS wh_led, w.ble_mac AS wh_mac
        FROM bin b JOIN box bx ON b.box_id = bx.id JOIN warehouse w ON bx.warehouse_id = w.id WHERE b.id=?
    """, (cid,)).fetchone()
    if not row: return jsonify({'error': '不存在'}), 404
    
    r, g, b, bright = _get_led_color(db)
    lights = []
    
    if row['box_mac']:
        # 1. 默认点亮箱子的 0 号指示
        leds_to_light = [0] 
        
        # 2. 计算元件的 LED 序号
        bin_led = row['bin_led']
        # 如果数据库里 led_index 没填 (None) 或者填了 0，自动推导为 slot_index + 1
        if not bin_led:
            bin_led = row['slot_index'] + 1 
            
        leds_to_light.append(bin_led)
            
        # 3. 一帧下发（同时点亮 0 和 bin_led）
        ble_mgr.send_led_bitmap(row['box_mac'], leds_to_light, r, g, b, bright)
        
        lights.append({'ble_mac': row['box_mac'], 'led_index': 0, 'name': row['box_name']})
        lights.append({'ble_mac': row['box_mac'], 'led_index': bin_led, 'name': row['bin_name']})
            
    if row['wh_mac'] and row['wh_led'] is not None:
        ble_mgr.send_led_bitmap(row['wh_mac'], [row['wh_led']], r, g, b, bright)
        lights.append({'ble_mac': row['wh_mac'], 'led_index': row['wh_led'], 'name': row['wh_name']})
        
    return jsonify({'lights': lights})

# ============================================================
# 写卡
# ============================================================
@app.route('/api/nfc/write', methods=['POST'])
def api_nfc_write():
    data = request.get_json(silent=True) or {}
    text = data.get('text', '')
    # 默认使用广播地址，全网板子都会准备写入
    target_mac = data.get('target_mac', 'FF:FF:FF:FF:FF:FF')
    
    if not text:
        return jsonify({'error': '写入文本不能为空'}), 400
        
    ble_mgr.send_nfc_write(target_mac, text)
    return jsonify({'status': 'ok', 'message': '写卡指令已下发，请将标签靠近板子'})

@app.errorhandler(405)
def _405(e):
    if request.path.startswith('/api/'):
        return jsonify({'error': f'API 未实现: {request.method} {request.path}', 'hint': '可能忘了在 app.py 里定义对应路由'}), 405
    return e

# ============================================================
# 本地调试启动入口
# ============================================================
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)