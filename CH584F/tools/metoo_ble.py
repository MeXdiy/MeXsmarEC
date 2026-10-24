#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MeXsmarEC 蓝牙组网节点 PC 端客户端（Linux / BlueZ / bleak）

依赖（Linux 上先装好 BlueZ 和 bleak）：
    sudo apt install bluez
    pip install bleak

用法：

    # 扫描并联上名称以 MeXsmarEC 开头的板子，持续打印 NFC 上报
    python3 metoo_ble.py listen

    # 也可以直接指定板子蓝牙地址
    python3 metoo_ble.py listen --device F0:EA:E1:EE:32:AD

    # 控制某一块板的 LED（--target 为目标板 MAC）
    python3 metoo_ble.py led on  --target F0:EA:E1:EE:32:AD
    python3 metoo_ble.py led off --target F0:EA:E1:EE:32:AD

    # 全网所有板一起亮 / 灭（广播地址）
    python3 metoo_ble.py led on  --target FF:FF:FF:FF:FF:FF

    # 让某块板把文本写成 NFC 文本标签（下发后把标签放到该板天线上即可写入）
    python3 metoo_ble.py write "Hello" --target F0:EA:E1:EE:32:AD

说明：
    - 上行（板 -> PC）走 FFE4 的 Notify，脚本已自动订阅并按需重组分片。
    - 下行（PC -> 板）走 FFE1 的 Write。
    - 帧格式与固件 src/app_mesh.h 一致，详见工程 readme.md。
"""

import argparse
import asyncio
import re
import struct
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("缺少 bleak，请先执行：  pip install bleak")


# ---------------------------------------------------------------- GATT 定义
SERVICE_UUID     = "0000ffe0-0000-1000-8000-00805f9b34fb"
CHAR_WRITE_UUID  = "0000ffe1-0000-1000-8000-00805f9b34fb"   # PC -> 板（Read/Write）
CHAR_NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"   # 板 -> PC（Notify）

DEVICE_NAME_PREFIX = "MeXsmarEC"

# ---------------------------------------------------------------- 帧定义
MESH_HEAD   = 0xA5     # mesh 帧头
CHUNK_MAGIC = 0x5A     # 分片通知头：0x5A, 序号, 分片总数

TYPE_NFC       = 0x01  # 板 -> PC：NFC 事件（UID + 标签文本）
TYPE_LED_SET   = 0x02  # PC -> 板：LED 控制
TYPE_LED_ACK   = 0x03  # 板 -> PC：LED 回执
TYPE_NFC_WRITE = 0x04  # PC -> 板：写入 NDEF 文本

BROADCAST = "FF:FF:FF:FF:FF:FF"

_msg_id = 0


def next_msg_id():
    """洪泛去重用的消息序号，本地自增即可。"""
    global _msg_id
    _msg_id = (_msg_id + 1) & 0xFFFFFFFF
    return _msg_id


# ---------------------------------------------------------------- 工具函数
def mac_to_bytes(mac):
    parts = re.split(r"[:-]", mac.strip())
    if len(parts) != 6:
        raise ValueError("MAC 地址格式不对：%s" % mac)
    return bytes(int(p, 16) for p in parts)


def bytes_to_mac(raw):
    return ":".join("%02X" % b for b in raw)


def build_frame(ftype, target, payload=b""):
    """[0xA5][msgId:4][type:1][addr:6][data:N]"""
    return (bytes([MESH_HEAD]) +
            struct.pack(">I", next_msg_id()) +
            bytes([ftype]) +
            mac_to_bytes(target) +
            payload)


def build_led_frame(on, target):
    return build_frame(TYPE_LED_SET, target, bytes([1 if on else 0]))


def build_nfc_write_frame(text, target):
    """文本长度 0 表示取消该板待写入的文本。"""
    return build_frame(TYPE_NFC_WRITE, target, text.encode("utf-8"))


class FrameAssembler(object):
    """把固件分片发出来的通知重组成一条完整的 mesh 帧。

    固件只有在帧长超过当前 ATT MTU 可用负载时才分片，分片格式为
    [0x5A, 序号, 分片总数, 数据...]；单包直发的帧首字节就是 mesh 帧头 0xA5。
    """

    def __init__(self):
        self._parts = {}

    def feed(self, data):
        data = bytes(data)
        if len(data) >= 3 and data[0] == CHUNK_MAGIC:
            seq = data[1]
            total = data[2]
            self._parts[seq] = data[3:]
            if len(self._parts) == total:
                frame = b"".join(self._parts[i] for i in range(total))
                self._parts.clear()
                return frame
            return None
        return data


def describe_frame(frame):
    if len(frame) < 13 or frame[0] != MESH_HEAD:
        return "未知数据: %s" % frame.hex(" ")

    msg_id = struct.unpack(">I", frame[1:5])[0]
    ftype = frame[5]
    addr = frame[6:12]
    payload = frame[12:]
    mac = bytes_to_mac(addr)

    if ftype == TYPE_NFC:
        uid = payload[:7]
        text_len = payload[7] if len(payload) > 7 else 0
        text = payload[8:8 + text_len]
        return ("NFC 事件    节点=%s  UID=%s  文本(%d)=%s"
                % (mac, uid.hex(" "), text_len, text.decode("utf-8", "replace")))

    if ftype == TYPE_LED_ACK:
        return "LED 回执    节点=%s  %s" % (mac, "开" if payload and payload[0] else "关")

    if ftype == TYPE_LED_SET:
        return "LED 指令    目标=%s  %s" % (mac, "开" if payload and payload[0] else "关")

    if ftype == TYPE_NFC_WRITE:
        return "NFC 写文本  目标=%s  文本=%s" % (mac, payload.decode("utf-8", "replace"))

    return "类型 0x%02X  节点=%s  负载=%s" % (ftype, mac, payload.hex(" "))


# ---------------------------------------------------------------- 连接流程
async def find_device(address):
    if address:
        return address

    print("正在扫描 %s ..." % DEVICE_NAME_PREFIX)
    devices = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for dev, adv in devices.values():
        name = adv.local_name or dev.name or ""
        if name.startswith(DEVICE_NAME_PREFIX):
            print("找到 %s (%s)" % (name, dev.address))
            return dev.address

    raise RuntimeError("没找到 %s 设备，确认板子已上电并在广播" % DEVICE_NAME_PREFIX)


async def run(args):
    address = await find_device(args.device)

    disconnected = asyncio.Event()

    async with BleakClient(address, disconnected_callback=lambda _: disconnected.set()) as client:
        print("已连接 %s" % address)

        # BlueZ 会在连接后协商 ATT MTU，帧长受它限制
        mtu = getattr(client, "mtu_size", None)
        print("ATT MTU = %s" % (mtu if mtu else "未知（未协商时按 23 处理）"))

        for service in client.services:
            print("服务 %s" % service.uuid)
            for ch in service.characteristics:
                print("    特征 %s  [%s]" % (ch.uuid, ",".join(ch.properties)))

        assembler = FrameAssembler()

        def on_notify(_sender, data):
            frame = assembler.feed(data)
            if frame is not None:
                print(describe_frame(frame))

        await client.start_notify(CHAR_NOTIFY_UUID, on_notify)
        print("已订阅 FFE4 通知，等待数据...")

        if args.command == "led":
            frame = build_led_frame(args.value == "on", args.target)
            await client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
            print("已发送: %s" % describe_frame(frame))

        elif args.command == "write":
            frame = build_nfc_write_frame(args.value, args.target)
            await client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
            print("已发送: %s" % describe_frame(frame))
            print("现在把 NFC 标签放到目标板的天线上，串口会打印写入结果")

        seconds = args.listen
        if seconds is None:
            seconds = 0.0 if args.command == "listen" else 5.0

        if seconds > 0:
            try:
                await asyncio.wait_for(disconnected.wait(), timeout=seconds)
            except asyncio.TimeoutError:
                pass
        else:
            print("按 Ctrl-C 退出")
            await disconnected.wait()

        await client.stop_notify(CHAR_NOTIFY_UUID)

    if disconnected.is_set():
        print("连接已断开")


def parse_args():
    parser = argparse.ArgumentParser(
        description="MeXsmarEC 蓝牙组网节点 PC 端客户端（Linux/BlueZ）")
    parser.add_argument("command", choices=["listen", "led", "write"],
                        help="listen 只监听；led 控制 LED；write 写 NFC 文本")
    parser.add_argument("value", nargs="?",
                        help="led 用 on/off；write 用要写入的文本")
    parser.add_argument("--device", help="板子蓝牙地址，省略则按名称扫描")
    parser.add_argument("--target", default=BROADCAST,
                        help="目标板 MAC，默认广播 FF:FF:FF:FF:FF:FF")
    parser.add_argument("--listen", type=float, default=None,
                        help="命令发送后继续监听秒数，0 表示一直监听")
    args = parser.parse_args()

    if args.command == "led" and args.value not in ("on", "off"):
        parser.error("led 的取值只能是 on 或 off")
    if args.command == "write" and args.value is None:
        parser.error("write 需要给出要写入的文本，空文本请用 \"\" 表示取消写入")

    return args


def main():
    args = parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        sys.exit("错误: %s" % exc)


if __name__ == "__main__":
    main()
