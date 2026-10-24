#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MeXsmarEC 上位机（本地 Web 界面）

    浏览器  <── HTTP / WebSocket ──>  server.py (aiohttp)  <── BLE ──>  CH584 板子

Linux 侧依赖：
    sudo apt install bluez            # BlueZ >= 5.43，bleak 依赖它
    pip install -r requirements.txt   # bleak + aiohttp

运行：
    python3 server.py                 # http://127.0.0.1:8765/
    python3 server.py --host 0.0.0.0 --port 8765

BLE 权限（访问 BLE 需要 cap_net_raw / cap_net_admin，二选一）：
    sudo setcap 'cap_net_raw,cap_net_admin+eip' "$(readlink -f "$(command -v python3)")"
    sudo python3 server.py

WebSocket 消息（浏览器 -> 后端）：
    {"cmd":"scan","timeout":6}
    {"cmd":"connect","address":"AA:BB:CC:DD:EE:FF"}
    {"cmd":"disconnect","address":"AA:BB:CC:DD:EE:FF"}
    {"cmd":"led","via":"AA:..","address":"AA:..|FF:..","on":true}
    {"cmd":"nfc_write","via":"AA:..","address":"AA:..|FF:..","text":"Hello"}
    {"cmd":"raw","via":"AA:..","hex":"A5 00 00 00 01 02 ..."}
    {"cmd":"dedup","on":true}
    {"cmd":"state"}

WebSocket 事件（后端 -> 浏览器）：
    {"ev":"hello",...} / {"ev":"state",...} / {"ev":"devices",...}
    {"ev":"link",...} / {"ev":"frame",...} / {"ev":"raw",...} / {"ev":"log",...}

帧格式与固件 src/app_mesh.h 一致，详见工程 readme.md。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import re
import struct
import time
from collections import deque
from pathlib import Path

from aiohttp import WSMsgType, web

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    raise SystemExit("缺少 bleak，请先执行： pip install -r requirements.txt")

LOG = logging.getLogger("metoo")

VERSION = "1.0.0"
STATIC_DIR = Path(__file__).resolve().parent / "static"

# ------------------------------------------------------------------ GATT / 帧协议
SERVICE_UUID     = "0000ffe0-0000-1000-8000-00805f9b34fb"
CHAR_WRITE_UUID  = "0000ffe1-0000-1000-8000-00805f9b34fb"   # PC -> 板
CHAR_NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"   # 板 -> PC

DEVICE_NAME_PREFIX = "MeXsmarEC"

MESH_HEAD      = 0xA5
MESH_HEAD_LEN  = 13          # 0xA5 + msgId(4) + type(1) + addr(6)
CHUNK_MAGIC    = 0x5A
CHUNK_HEAD_LEN = 3           # 0x5A + 序号 + 分片总数

TYPE_NFC       = 0x01
TYPE_LED_SET   = 0x02
TYPE_LED_ACK   = 0x03
TYPE_NFC_WRITE = 0x04
TYPE_PC_STATE  = 0xFF        # 上位机在线心跳（本机 -> 全网）

BROADCAST_MAC  = "FF:FF:FF:FF:FF:FF"

TYPE_NAME = {
    TYPE_NFC: "NFC 事件",
    TYPE_LED_SET: "LED 指令",
    TYPE_LED_ACK: "LED 回执",
    TYPE_NFC_WRITE: "NFC 写卡",
    TYPE_PC_STATE: "上位机心跳",
}

BROADCAST     = "FF:FF:FF:FF:FF:FF"
MESH_TEXT_MAX = 200
UID_LEN       = 7

_MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}$")


# ------------------------------------------------------------------ 通用工具
def mac_to_bytes(mac: str) -> bytes:
    parts = re.split(r"[:-]", (mac or "").strip())
    if len(parts) != 6:
        raise ValueError("MAC 地址格式不对：%s" % mac)
    return bytes(int(p, 16) for p in parts)


def bytes_to_mac(raw: bytes) -> str:
    return ":".join("%02X" % b for b in raw)


def norm_mac(mac: str) -> str:
    """把 00-11-22-... / 00:11:... 统一成大写冒号分隔形式。"""
    if not _MAC_RE.match((mac or "").strip()):
        raise ValueError("MAC 地址格式不对：%s" % mac)
    return bytes_to_mac(mac_to_bytes(mac))


def parse_hex(text: str) -> bytes:
    cleaned = re.sub(r"[\s,:;\-]", "", text or "")
    if not cleaned:
        raise ValueError("十六进制内容为空")
    if len(cleaned) % 2:
        raise ValueError("十六进制位数必须是偶数")
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        raise ValueError("十六进制内容非法")


# ------------------------------------------------------------------ 帧构造 / 解析
_msg_seq = 0


def next_msg_id() -> int:
    """洪泛去重用的消息序号，本地自增即可。"""
    global _msg_seq
    _msg_seq = (_msg_seq + 1) & 0xFFFFFFFF
    return _msg_seq


def build_frame(ftype: int, target: str, payload: bytes = b"") -> bytes:
    """[0xA5][msgId:4][type:1][addr:6][data:N]"""
    return (bytes([MESH_HEAD]) +
            struct.pack(">I", next_msg_id()) +
            bytes([ftype]) +
            mac_to_bytes(target) +
            payload)


def describe_frame(frame: bytes) -> dict:
    """把一帧解析成给前端展示用的字典。"""
    info = {"raw": frame.hex(" "), "ok": False, "summary": ""}

    if len(frame) < MESH_HEAD_LEN:
        info["summary"] = "长度不足 %d 字节" % MESH_HEAD_LEN
        return info
    if frame[0] != MESH_HEAD:
        info["summary"] = "帧头不是 0xA5，按原始数据处理"
        return info

    msg_id = struct.unpack(">I", frame[1:5])[0]
    ftype  = frame[5]
    node   = bytes_to_mac(frame[6:12])
    data   = frame[12:]

    info.update({
        "ok": True,
        "msgId": msg_id,
        "type": ftype,
        "typeName": TYPE_NAME.get(ftype, "类型 0x%02X" % ftype),
        "node": node,
    })

    if ftype == TYPE_NFC:
        uid  = data[:UID_LEN]
        tlen = data[UID_LEN] if len(data) > UID_LEN else 0
        text = data[UID_LEN + 1: UID_LEN + 1 + tlen]
        info["uid"]  = uid.hex(" ").upper()
        info["text"] = text.decode("utf-8", "replace")
        info["summary"] = "节点 %s UID=%s 文本=%s" % (node, info["uid"], info["text"])
    elif ftype in (TYPE_LED_SET, TYPE_LED_ACK):
        on = bool(data and data[0])
        info["on"] = on
        info["summary"] = "%s %s" % (node, "开" if on else "关")
    elif ftype == TYPE_NFC_WRITE:
        info["text"] = data.decode("utf-8", "replace")
        info["summary"] = "目标 %s 文本=%s" % (node, info["text"])
    else:
        info["summary"] = "节点 %s 负载=%s" % (node, data.hex(" "))

    return info


class FrameAssembler:
    """把固件分片发出的通知重组成一条完整的 mesh 帧。

    固件只在帧长超过 ATT MTU 可用负载时才分片，分片格式为
    [0x5A, 序号, 分片总数, 数据...]；单包直发的帧首字节就是 mesh 帧头 0xA5。
    上一帧没组完就来了新分片、或超时未见齐，则整帧丢弃，避免脏数据串帧。
    """

    def __init__(self, timeout: float = 5.0) -> None:
        self._timeout = timeout
        self._parts: dict[int, bytes] = {}
        self._total = 0
        self._stamp = 0.0

    def _reset(self) -> None:
        self._parts.clear()
        self._total = 0
        self._stamp = 0.0

    def feed(self, data: bytes):
        now = time.monotonic()

        if len(data) >= CHUNK_HEAD_LEN and data[0] == CHUNK_MAGIC:
            seq, total = data[1], data[2]
            if total == 0 or seq >= total:
                self._reset()
                return None
            if self._parts and (total != self._total or now - self._stamp > self._timeout):
                self._reset()
                return None
            self._total = total
            self._stamp = now
            self._parts[seq] = bytes(data[CHUNK_HEAD_LEN:])
            if len(self._parts) == total:
                frame = b"".join(self._parts[i] for i in range(total))
                self._reset()
                return frame
            return None

        self._reset()
        return bytes(data)


class FrameDedup:
    """按 (源节点, msgId) 在时间窗口内去重。

    固件用 msgId 做洪泛去重，同一 NFC 事件仍可能从多条路径重复送达；
    节点重启后 msgId 会从新序列开始，所以只对窗口内的重复生效。
    """

    def __init__(self, window: float = 10.0, capacity: int = 4096) -> None:
        self._window = window
        self._capacity = capacity
        self._seen: dict[tuple, float] = {}
        self._order: deque = deque()

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
        while len(self._order) > self._capacity:
            old, _ = self._order.popleft()
            self._seen.pop(old, None)
        return False


# ------------------------------------------------------------------ 单块板连接
class NodeLink:
    """一块板子的 BLE 连接：订阅 FFE4，断线后按退避自动重连。"""

    def __init__(self, hub: "Hub", address: str, name: str = "") -> None:
        self.hub = hub
        self.address = address
        self.name = name
        self.state = "idle"
        self.detail = ""
        self.mtu = None
        self.client = None
        self.assembler = FrameAssembler()
        self._keep = False
        self._task = None
        self._lost = asyncio.Event()

    # ------------------------------------------------ 状态
    def snapshot(self) -> dict:
        return {
            "address": self.address,
            "name": self.name,
            "state": self.state,
            "detail": self.detail,
            "mtu": self.mtu,
        }

    @property
    def connected(self) -> bool:
        return self.state == "connected"

    # ------------------------------------------------ 生命周期
    async def start(self) -> None:
        if self._task and not self._task.done():
            return
        self._keep = True
        self._task = asyncio.create_task(self._supervise())

    async def stop(self) -> None:
        self._keep = False
        self._lost.set()
        task, self._task = self._task, None
        if task:
            try:
                await asyncio.wait_for(task, timeout=8.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                task.cancel()
        self.state = "idle"
        self.detail = ""

    async def _supervise(self) -> None:
        delay = 1.0

        while self._keep:
            self._lost.clear()
            await self._set_state("connecting", "")

            try:
                async with BleakClient(self.address,
                                       disconnected_callback=self._on_lost) as client:
                    self.client = client
                    self.mtu = self._read_mtu(client)
                    await self._subscribe(client)
                    await self._set_state("connected", "")
                    delay = 1.0
                    pinger = asyncio.create_task(self._ping_loop(client))
                    try:
                        await self._lost.wait()
                    finally:
                        pinger.cancel()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                await self._set_state("error", "%s: %s" % (type(exc).__name__, exc))
                await self.hub.log("error", "连接 %s 失败：%s" % (self.address, exc))
            finally:
                self.client = None
                if self.state != "error":
                    await self._set_state("disconnected", "")

            if not self._keep:
                break

            await self._set_state("reconnecting", "%.0f 秒后重试" % delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2, 10.0)

        await self._set_state("idle", "")

    @staticmethod
    def _read_mtu(client):
        """Linux 下部分 BlueZ 版本取不到 MTU，取不到就按未协商处理。"""
        try:
            return int(client.mtu_size)
        except Exception:
            return None

    PING_PERIOD = 1.0

    async def _ping_loop(self, client) -> None:
        """每秒发一次"上位机在线"心跳，板子 6 秒收不到就判上位机离线。

        直接写特征值、不走 send()，免得每秒往 UI 日志里塞一条记录。
        """
        while self._keep:
            try:
                await client.write_gatt_char(
                    CHAR_WRITE_UUID,
                    build_frame(TYPE_PC_STATE, BROADCAST_MAC, b"\x01"),
                    response=True,
                )
            except asyncio.CancelledError:
                raise
            except Exception:
                return          # 链路断了，重连后由 _supervise 重新拉起
            await asyncio.sleep(self.PING_PERIOD)

    async def _subscribe(self, client) -> None:
        """Enable notifications on the uplink characteristic.

        A brand new connection can catch the board while the CCCD slot of the
        link that just went down is still busy; the board then answers the
        subscribe with a GATT protocol error.  A retry a moment later works, so
        do not tear the link down over a transient error.
        """
        last = None
        for attempt in range(3):
            try:
                await client.start_notify(CHAR_NOTIFY_UUID, self._on_notify)
                return
            except Exception as exc:
                last = exc
                await asyncio.sleep(0.3 * (attempt + 1))
        raise last

    def _on_lost(self, _client) -> None:
        self._lost.set()
        # 主动 stop() 时 _keep 已置 False，这里不重复记日志
        if not self._keep:
            return
        try:
            asyncio.create_task(self.hub.log(
                "warn",
                "板子 %s（%s）蓝牙连接已断开" % (self.address, self.name or "未知"),
            ))
        except RuntimeError:
            pass

    async def stop(self) -> None:
        was_active = self.state in ("connecting", "connected", "reconnecting", "error")
        self._keep = False
        self._lost.set()
        task, self._task = self._task, None
        if task:
            try:
                await asyncio.wait_for(task, timeout=8.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                task.cancel()
        self.state = "idle"
        self.detail = ""
        if was_active:
            await self.hub.log(
                "info",
                "已主动断开板子 %s（%s）" % (self.address, self.name or "未知"),
            )

    async def _set_state(self, state: str, detail: str) -> None:
        self.state = state
        self.detail = detail
        await self.hub.broadcast({"ev": "link", **self.snapshot()})

    # ------------------------------------------------ 收发
    def _on_notify(self, _sender, data: bytes) -> None:
        asyncio.create_task(self.hub.on_notify(self, bytes(data)))

    async def send(self, data: bytes) -> None:
        client = self.client
        if client is None or not client.is_connected:
            raise RuntimeError("板子 %s 未连接" % self.address)
        await client.write_gatt_char(CHAR_WRITE_UUID, data, response=True)
        await self.hub.broadcast({
            "ev": "raw", "address": self.address, "dir": "tx", "hex": data.hex(" "),
        })


# ------------------------------------------------------------------ 中枢
class Hub:
    """连接、设备缓存、帧分发与 WebSocket 广播。"""

    def __init__(self) -> None:
        self.links: dict[str, NodeLink] = {}
        self.clients: set = set()
        self.devices: dict[str, dict] = {}
        self.dedup = FrameDedup()
        self.dedup_on = True
        self.dedup_dropped = 0
        self.scanning = False

    # ------------------------------------------------ WebSocket
    def add_client(self, ws) -> None:
        self.clients.add(ws)

    def remove_client(self, ws) -> None:
        self.clients.discard(ws)

    async def broadcast(self, message: dict) -> None:
        if not self.clients:
            return
        payload = json.dumps(message, ensure_ascii=False)
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send_str(payload)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.remove_client(ws)

    async def log(self, level: str, text: str) -> None:
        LOG.log(getattr(logging, level.upper(), logging.INFO), text)
        await self.broadcast({"ev": "log", "level": level, "text": text})

    # ------------------------------------------------ 设备
    def device_list(self) -> list:
        out = []
        for address, entry in self.devices.items():
            link = self.links.get(address)
            item = dict(entry)
            item["address"] = address
            item["state"] = link.state if link else "idle"
            item["connected"] = bool(link and link.connected)
            item["matched"] = (entry.get("name") or "").startswith(DEVICE_NAME_PREFIX)
            out.append(item)
        out.sort(key=lambda d: (not d["matched"], d["name"] or "", d["address"]))
        return out

    async def scan(self, timeout: float) -> None:
        if self.scanning:
            return
        self.scanning = True
        await self.log("info", "开始扫描 %.0f 秒 ..." % timeout)
        try:
            found = await self._discover(timeout)
        except Exception as exc:
            await self.log("error", "扫描失败：%s" % exc)
            self.scanning = False
            await self.state_to_all()
            return

        for address, name, rssi in found:
            entry = self.devices.setdefault(address, {"address": address})
            entry["name"] = name
            entry["rssi"] = rssi

        self.scanning = False
        await self.log("info", "扫描结束，共 %d 个设备" % len(self.devices))
        await self.broadcast({"ev": "devices", "devices": self.device_list()})
        await self.state_to_all()

    @staticmethod
    async def _discover(timeout: float) -> list:
        try:
            raw = await BleakScanner.discover(timeout=timeout, return_adv=True)
        except TypeError:
            # 老版本 bleak 没有 return_adv
            devices = await BleakScanner.discover(timeout=timeout)
            return [(d.address, d.name or "", None) for d in devices]

        out = []
        for _address, pair in raw.items():
            device, adv = pair
            name = (adv.local_name or device.name or "")
            out.append((device.address, name, getattr(adv, "rssi", None)))
        return out

    # ------------------------------------------------ 连接
    async def connect(self, address: str) -> None:
        address = norm_mac(address)
        link = self.links.get(address)
        if link and link.state in ("connecting", "connected", "reconnecting"):
            await self.log("info", "%s 已在连接中" % address)
            return
        if link is None:
            name = (self.devices.get(address) or {}).get("name", "")
            link = NodeLink(self, address, name)
            self.links[address] = link
        await link.start()

    async def disconnect(self, address: str) -> None:
        address = norm_mac(address)
        link = self.links.pop(address, None)
        if link is None:
            return
        await link.stop()
        await self.broadcast({"ev": "link", **link.snapshot()})

    # ------------------------------------------------ 收帧
    async def on_notify(self, link: NodeLink, data: bytes) -> None:
        await self.broadcast({
            "ev": "raw", "address": link.address, "dir": "rx", "hex": data.hex(" "),
        })

        frame = link.assembler.feed(data)
        if frame is None:
            return

        info = describe_frame(frame)
        if info.get("ok") and self.dedup_on:
            if self.dedup.is_duplicate(info.get("node", ""), info.get("msgId", 0)):
                self.dedup_dropped += 1
                return
        await self.broadcast({"ev": "frame", "address": link.address, "frame": info})

    # ------------------------------------------------ 发帧
    def _pick_channel(self, preferred) -> str:
        connected = [addr for addr, link in self.links.items() if link.connected]
        if not connected:
            raise RuntimeError("没有已连接的板子，请先连接一块板")
        if preferred:
            if preferred in connected:
                return preferred
            raise RuntimeError("发送通道 %s 未连接" % preferred)
        return connected[0]

    async def send_frame(self, address: str, frame: bytes) -> None:
        link = self.links.get(address)
        if link is None or not link.connected:
            raise RuntimeError("板子 %s 未连接" % address)
        await link.send(frame)

    # ------------------------------------------------ 命令
    async def handle(self, cmd: dict) -> None:
        name = (cmd.get("cmd") or "").strip()
        try:
            if name == "scan":
                if not self.scanning:
                    asyncio.create_task(self.scan(float(cmd.get("timeout") or 6)))
                return

            if name == "connect":
                await self.connect(cmd["address"])
            elif name == "disconnect":
                await self.disconnect(cmd["address"])
            elif name == "led":
                await self._cmd_led(cmd)
            elif name == "nfc_write":
                await self._cmd_nfc_write(cmd)
            elif name == "raw":
                channel = self._pick_channel(cmd.get("via"))
                await self.send_frame(channel, parse_hex(cmd.get("hex") or ""))
            elif name == "dedup":
                self.dedup_on = bool(cmd.get("on"))
                self.dedup_dropped = 0
                await self.log("info", "帧去重已%s" % ("开启" if self.dedup_on else "关闭"))
            elif name == "state":
                pass
            else:
                await self.log("warn", "未知命令：%s" % name)
        except Exception as exc:
            await self.log("error", "命令 %s 失败：%s" % (name, exc))

        await self.state_to_all()

    async def _cmd_led(self, cmd: dict) -> None:
        channel = self._pick_channel(cmd.get("via"))
        target = norm_mac(cmd.get("address") or BROADCAST)
        on = bool(cmd.get("on"))
        frame = build_frame(TYPE_LED_SET, target, bytes([1 if on else 0]))
        await self.send_frame(channel, frame)
        await self.log("info", "LED %s -> %s（经 %s）" % ("开" if on else "关", target, channel))

    async def _cmd_nfc_write(self, cmd: dict) -> None:
        channel = self._pick_channel(cmd.get("via"))
        target = norm_mac(cmd.get("address") or BROADCAST)
        text = (cmd.get("text") or "").encode("utf-8")[:MESH_TEXT_MAX]
        frame = build_frame(TYPE_NFC_WRITE, target, text)
        await self.send_frame(channel, frame)
        await self.log("info", "写卡文本 %d 字节 -> %s（经 %s）"
                       % (len(text), target, channel))

    # ------------------------------------------------ 状态
    async def state_to_all(self) -> None:
        await self.broadcast({
            "ev": "state",
            "devices": self.device_list(),
            "links": [link.snapshot() for link in self.links.values()],
            "dedup": self.dedup_on,
            "dedupDropped": self.dedup_dropped,
            "scanning": self.scanning,
        })

    async def shutdown(self) -> None:
        for address in list(self.links):
            link = self.links.pop(address)
            await link.stop()


# ------------------------------------------------------------------ HTTP / WebSocket
async def index(_request):
    return web.FileResponse(STATIC_DIR / "index.html")


async def ws_handler(request):
    hub: Hub = request.app["hub"]
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)

    hub.add_client(ws)
    await ws.send_json({
        "ev": "hello",
        "version": VERSION,
        "textMax": MESH_TEXT_MAX,
        "prefix": DEVICE_NAME_PREFIX,
        "broadcast": BROADCAST,
    })
    await hub.state_to_all()

    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    await hub.handle(json.loads(msg.data))
                except json.JSONDecodeError:
                    await hub.log("warn", "收到非 JSON 消息")
            elif msg.type == WSMsgType.ERROR:
                break
    finally:
        hub.remove_client(ws)

    return ws


async def on_cleanup(app) -> None:
    await app["hub"].shutdown()


# ------------------------------------------------------------------ 入口
def parse_args():
    parser = argparse.ArgumentParser(description="MeXsmarEC 上位机（本地 Web 界面）")
    parser.add_argument("--host", default="0.0.0.0",
                        help="监听地址，默认 127.0.0.1（只允许本机访问）")
    parser.add_argument("--port", type=int, default=8765, help="监听端口，默认 8765")
    parser.add_argument("--verbose", action="store_true", help="打印调试日志")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    app = web.Application()
    app["hub"] = Hub()
    app.on_cleanup.append(on_cleanup)

    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_static("/static/", STATIC_DIR)

    LOG.info("上位机已启动： http://%s:%d/", args.host, args.port)
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
