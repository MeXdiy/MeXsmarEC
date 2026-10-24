import asyncio
import logging
import threading
import time
import struct
from collections import deque

from bleak import BleakClient, BleakScanner

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

CHAR_NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"
CHAR_WRITE_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

class FrameAssembler:
    def __init__(self, timeout=5.0):
        self._timeout = timeout
        self._parts = {}
        self._total = 0
        self._stamp = 0.0

    def feed(self, data: bytes):
        now = time.monotonic()
        if len(data) >= 3 and data[0] == 0x5A:
            seq, total = data[1], data[2]
            if total == 0 or seq >= total:
                self._parts.clear()
                return None
            if self._parts and (total != self._total or now - self._stamp > self._timeout):
                self._parts.clear()
                return None
            self._total = total
            self._stamp = now
            self._parts[seq] = bytes(data[3:])
            
            if len(self._parts) == total:
                frame = b"".join(self._parts[i] for i in range(total))
                self._parts.clear()
                return frame
            return None
        self._parts.clear()
        return bytes(data)

class FrameDedup:
    def __init__(self, window=10.0, capacity=4096):
        self._window = window
        self._capacity = capacity
        self._seen = {}
        self._order = deque()

    def is_duplicate(self, node: str, msg_id: int) -> bool:
        now = time.monotonic()
        while self._order and now - self._order[0][1] > self._window:
            key, _ = self._order.popleft()
            self._seen.pop(key, None)
        key = (node, msg_id)
        if key in self._seen:
            return True
        self._seen[key] = now
        self._order.append((key, now))
        return False

class BLEManager(threading.Thread):
    def __init__(self, sse_queue):
        super().__init__(daemon=True)
        self.sse_events = sse_queue
        
        self.loop = None
        self.client = None
        self.connected_device = None
        self.is_ready = False
        self.scanning = False
        self.scan_results = []
        
        self.assembler = FrameAssembler()
        self.dedup = FrameDedup()
        
        self._keep_connected = False
        self._reconnect_task = None
        self._lost_event = None

        self._recent_uids = {}
        self._bg_tasks = set() 

    def run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self._lost_event = asyncio.Event()
        self.is_ready = True
        logger.info("✅ BLE 异步事件循环已启动")
        self.loop.run_forever()

    def _run_async(self, coro):
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(coro, self.loop)

    def trigger_scan(self):
        self._run_async(self._async_scan())

    def get_results(self):
        return self.scan_results

    def connect(self, address):
        self._run_async(self._async_connect(address))

    def disconnect(self):
        self._run_async(self._async_disconnect())

    # ================= 供 Flask 调用的同步接口 =================
    def send_led_bitmap(self, target_mac, led_indices, r, g, b, brightness):
        self._run_async(self._async_send_led_bitmap(target_mac, led_indices, r, g, b, brightness))

    # ================= 底层异步发送逻辑 =================
    async def _async_send_led_bitmap(self, target_mac, led_indices, r, g, b, brightness):
        if not self.client or not self.client.is_connected:
            logger.error("📤 [BLE TX] ❌ 发送失败，未连接设备")
            return
            
        # 1. 构建 32 字节的位图 (初始化全 0)
        bitmap = bytearray(32)
        for idx in led_indices:
            if 0 <= idx < 256:
                # 将对应位置 1
                bitmap[idx // 8] |= (1 << (idx % 8))
                
        msg_id = int(time.time() * 1000) & 0xFFFFFFFF
        mac_bytes = bytes(int(p, 16) for p in target_mac.split(":"))
        
        # 2. 组装 Payload: 亮度(1) + R(1) + G(1) + B(1) + Bitmap(32)
        payload = struct.pack(">BBBB", brightness, r, g, b) + bytes(bitmap)
        
        # 3. 组装完整帧
        frame = (struct.pack(">B", 0xA5) + 
                 struct.pack(">I", msg_id) + 
                 struct.pack(">B", 0x02) + 
                 mac_bytes + 
                 payload)
                 
        # logger.info(f"📤 [BLE TX] 发送LED位图 -> {target_mac} 目标LED:{led_indices}")
        
        try:
            await self.client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
            logger.info(f"📤 [BLE TX] ✅ 发送成功 -> {target_mac}, 点亮数量:{len(led_indices)}")
        except Exception as e:
            logger.error(f"📤 [BLE TX] ❌ 发送异常: {e}")

    # ================= 底层异步发送逻辑 =================
    async def _async_send_led(self, target_mac, led_index, on, r, g, b, brightness):
        if not self.client or not self.client.is_connected:
            logger.error("📤 [BLE TX] ❌ 发送失败，未连接设备")
            return
        
        msg_id = int(time.time() * 1000) & 0xFFFFFFFF
        mac_bytes = bytes(int(p, 16) for p in target_mac.split(":"))
        
        # 组装 0x02 帧：head + msgId(4) + type(1) + mac(6) + data(6: [led_index, state, r, g, b, brightness])
        frame = (struct.pack(">B", 0xA5) + 
                 struct.pack(">I", msg_id) + 
                 struct.pack(">B", 0x02) + 
                 mac_bytes + 
                 struct.pack(">BBBBBB", led_index, 1 if on else 0, r, g, b, brightness))
                 
        hex_str = frame.hex(' ').upper()
        logger.info(f"📤 [BLE TX] 发送LED指令 -> {target_mac} 序号:{led_index} 状态:{'开' if on else '关'} RGB:({r},{g},{b}) 亮度:{brightness}")
        
        try:
            await self.client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
            logger.info(f"📤 [BLE TX] ✅ 发送成功 -> {target_mac}")
        except Exception as e:
            logger.error(f"📤 [BLE TX] ❌ 发送异常: {e}")


    # ================= NFC 写入接口 =================
    def send_nfc_write(self, target_mac, text):
        self._run_async(self._async_send_nfc_write(target_mac, text))

    async def _async_send_nfc_write(self, target_mac, text):
        if not self.client or not self.client.is_connected:
            logger.error("📤 [BLE TX] ❌ 写卡失败，未连接设备")
            return
        
        msg_id = int(time.time() * 1000) & 0xFFFFFFFF
        mac_bytes = bytes(int(p, 16) for p in target_mac.split(":"))
        
        # 文本转 UTF-8 并限制最大 200 字节
        text_bytes = text.encode('utf-8')[:200]
        if len(text_bytes) == 0:
            return

        # 组装 0x04 帧：head + msgId(4) + type(0x04) + mac(6) + text_bytes
        frame = (struct.pack(">B", 0xA5) + 
                 struct.pack(">I", msg_id) + 
                 struct.pack(">B", 0x04) + 
                 mac_bytes + 
                 text_bytes)
                 
        logger.info(f"📤 [BLE TX] 发送 NFC 写入指令 -> {target_mac} 文本: {text}")
        try:
            await self.client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
            logger.info(f"📤 [BLE TX] ✅ 写卡指令已下发 -> {target_mac}")
        except Exception as e:
            logger.error(f"📤 [BLE TX] ❌ 写卡异常: {e}")







    async def _async_scan(self):
        if self.scanning: return
        self.scanning = True
        self.scan_results = []
        try:
            devices = await BleakScanner.discover(timeout=5.0)
            for d in devices:
                self.scan_results.append({"name": d.name or "未知设备", "address": d.address})
        except Exception as e:
            logger.error(f"❌ 扫描失败: {e}")
        finally:
            self.scanning = False

    async def _async_connect(self, address):
        await self._async_disconnect()
        self._keep_connected = True
        self._reconnect_task = asyncio.create_task(self._supervise_connection(address))

    async def _async_disconnect(self):
        self._keep_connected = False
        if self._lost_event:
            self._lost_event.set()
            
        if self._reconnect_task:
            self._reconnect_task.cancel()
            try:
                await self._reconnect_task
            except asyncio.CancelledError:
                pass
            self._reconnect_task = None
            
        if self.client:
            try:
                await self.client.disconnect()
            except:
                pass
            self.client = None
        self.connected_device = None

    async def _supervise_connection(self, address):
        delay = 2.0
        
        while self._keep_connected:
            self._lost_event.clear()
            client = None
            heartbeat_task = None
            
            try:
                client = BleakClient(address, disconnected_callback=self._on_disconnected)
                await client.connect()
                if not client.is_connected:
                    raise RuntimeError("底层报告已连接，但实际无响应")

                self.client = client
                self.connected_device = {"name": address, "address": address}
                
                await self._subscribe(client)
                logger.info(f"[{address}] ✅ 订阅成功！")
                
                # 👇 启动心跳任务（每秒发送一次 0xFF 广播）
                heartbeat_task = asyncio.create_task(self._heartbeat_loop(client))
                
                delay = 2.0 
                await self._lost_event.wait()

            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"[{address}] ❌ 连接异常: {e}")
            
            # 断开时取消心跳
            if heartbeat_task:
                heartbeat_task.cancel()
            
            self.client = None
            self.connected_device = None
            if client:
                try: await client.disconnect()
                except: pass
            
            if self._keep_connected:
                logger.info(f"[{address}] {delay} 秒后尝试重连...")
                await asyncio.sleep(delay)
                delay = min(delay + 1.5, 10.0)

    # 👇 新增：心跳发送循环
    async def _heartbeat_loop(self, client):
        """每秒向广播地址发送 0xFF 心跳包"""
        msg_seq = 0
        while self._keep_connected and client.is_connected:
            try:
                msg_seq += 1
                # 组装心跳帧： A5 + MsgId(4) + 0xFF + 广播MAC(FF*6) + 在线状态(01)
                frame = (struct.pack(">B", 0xA5) + 
                         struct.pack(">I", msg_seq & 0xFFFFFFFF) + 
                         struct.pack(">B", 0xFF) + 
                         b'\xFF\xFF\xFF\xFF\xFF\xFF' + 
                         struct.pack(">B", 0x01))
                await client.write_gatt_char(CHAR_WRITE_UUID, frame, response=False)
            except Exception as e:
                logger.warning(f"📤 [BLE TX] 心跳发送失败: {e}")
                break
            await asyncio.sleep(1.0)

    async def _subscribe(self, client):
        last_err = None
        for attempt in range(3):
            try:
                await client.start_notify(CHAR_NOTIFY_UUID, self._on_notify)
                return
            except Exception as e:
                last_err = e
                logger.warning(f"⚠️ 订阅失败 ({attempt+1}/3): {e}")
                await asyncio.sleep(0.5 * (attempt + 1))
        raise last_err

    def _on_disconnected(self, client):
        logger.warning(f"[{client.address}] ⚠️ BLE 连接断开！")
        if self._lost_event:
            self._lost_event.set()

    def _on_notify(self, sender, data: bytes):
        task = asyncio.create_task(self._process_notify(data))
        self._bg_tasks.add(task)
        task.add_done_callback(self._bg_tasks.discard)

    async def _process_notify(self, data: bytes):
        try:
            raw_hex = data.hex(' ').upper()
            logger.info(f"📥 [BLE RX] 收到数据 ({len(data)} 字节): {raw_hex}")
            
            frame = self.assembler.feed(data)
            if not frame:
                return
            if len(frame) < 13 or frame[0] != 0xA5:
                logger.warning(f"📥 [BLE RX] ❌ 帧格式错误")
                return
            
            msg_id = int.from_bytes(frame[1:5], 'big')
            msg_type = frame[5]
            node_mac = ":".join(f"{b:02X}" for b in frame[6:12])
            payload = frame[12:]

            if self.dedup.is_duplicate(node_mac, msg_id):
                return
                
            if msg_type == 0x01:
                uid = payload[:7].hex().upper()
                if uid.endswith("000000") and len(uid) > 8:
                    uid = uid[:8]
                
                t_len = payload[7] if len(payload) > 7 else 0
                text = payload[8:8+t_len].decode('utf-8', errors='ignore')
                
                now = time.time()
                if uid in self._recent_uids and now - self._recent_uids[uid] < 2.0:
                    return
                self._recent_uids[uid] = now
                self._recent_uids = {k: v for k, v in self._recent_uids.items() if now - v < 5.0}
                
                logger.info(f"🎉 [解析成功] 类型: NFC, MAC={node_mac}, UID={uid}, 文本={text}")
                self.sse_events.put({"type": "nfc_scan", "uid": uid, "text": text, "node": node_mac})
           # 👇 新增心跳状态解析
            elif msg_type == 0xFF:
                is_online = (len(payload) > 0 and payload[0] == 1)
                status_str = "在线" if is_online else "离线"
                logger.info(f"💓 [心跳] 板子 {node_mac} 报告 PC 状态: {status_str}")
                
                # 推送到 SSE，前端可展示
                self.sse_events.put({
                    "type": "heartbeat_status",
                    "node": node_mac,
                    "online": is_online
                })
                
        except Exception as e:
            logger.error(f"处理通知数据出错: {e}")

    async def _async_send_frame(self, target_mac, on):
        if not self.client or not self.client.is_connected:
            return
        
        msg_id = int(time.time() * 1000) & 0xFFFFFFFF
        mac_bytes = bytes(int(p, 16) for p in target_mac.split(":"))
        frame = struct.pack(">B", 0xA5) + struct.pack(">I", msg_id) + struct.pack(">B", 0x02) + mac_bytes + struct.pack(">B", 1 if on else 0)
        
        try:
            await self.client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
        except Exception as e:
            logger.error(f"📤 [BLE TX] ❌ 发送异常: {e}")