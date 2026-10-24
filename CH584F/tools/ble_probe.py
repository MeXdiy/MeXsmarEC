#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MeXsmarEC BLE 连接诊断脚本（在 Linux / BlueZ 上运行）

把「上位机连不上板子」的每一步单独打出来，定位到底是连接、服务发现、
CCCD 订阅哪一步失败。

用法：
    python3 ble_probe.py                          # 按广播名 MeXsmarEC 扫描
    python3 ble_probe.py --address F0:EA:E1:EE:32:AD
    python3 ble_probe.py --led on                 # 连上后顺带发一帧 LED 广播
    python3 ble_probe.py --seconds 20             # 订阅后持续接收 20 秒

依赖：bleak（工程 tools/metoo_web/requirements.txt 里已有）
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME_PREFIX = "MeXsmarEC"

SERVICE_UUID     = "0000ffe0-0000-1000-8000-00805f9b34fb"
CHAR_WRITE_UUID  = "0000ffe1-0000-1000-8000-00805f9b34fb"
CHAR_NOTIFY_UUID = "0000ffe4-0000-1000-8000-00805f9b34fb"
CCCD_UUID        = "00002902-0000-1000-8000-00805f9b34fb"

MESH_HEAD = 0xA5
BROADCAST = "FF:FF:FF:FF:FF:FF"
TYPE_LED_SET = 0x02

_msg_seq = 0


def stamp() -> str:
    return time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)


def step(text: str) -> None:
    print("\n=== %s ===" % text)


def note(text: str) -> None:
    print("%s  %s" % (stamp(), text))


def mac_to_bytes(mac: str) -> bytes:
    return bytes(int(x, 16) for x in mac.split(":"))


def build_frame(ftype: int, target: str, payload: bytes = b"") -> bytes:
    global _msg_seq
    _msg_seq = (_msg_seq + 1) & 0xFFFFFFFF
    return (bytes([MESH_HEAD]) + struct.pack(">I", _msg_seq) +
            bytes([ftype]) + mac_to_bytes(target) + payload)


async def pick_device(args):
    step("扫描")
    try:
        found = await BleakScanner.discover(timeout=args.scan, return_adv=True)
    except TypeError:
        devices = await BleakScanner.discover(timeout=args.scan)
        found = {d.address: (d, None) for d in devices}

    for address, pair in found.items():
        device, adv = pair
        name = (getattr(adv, "local_name", None) or device.name or "") if adv else (device.name or "")
        note("%s  %-20s rssi=%s" % (address, name or "(无名)", getattr(adv, "rssi", None) if adv else None))

    if args.address:
        return args.address

    for address, pair in found.items():
        device, adv = pair
        name = (getattr(adv, "local_name", None) or device.name or "") if adv else (device.name or "")
        if name.startswith(DEVICE_NAME_PREFIX):
            note("按广播名选中 %s" % address)
            return address

    raise SystemExit("没找到 %s* 设备，请用 --address 指定" % DEVICE_NAME_PREFIX)


def dump_services(client) -> int:
    step("服务与特征")
    cccd_handle = None

    for service in client.services:
        note("service %s  %s" % (service.uuid, service.description or ""))
        for ch in service.characteristics:
            note("   char %s  props=%s  handle=%d"
                 % (ch.uuid, ",".join(ch.properties), ch.handle))
            for desc in ch.descriptors:
                note("      desc %s  handle=%d" % (desc.uuid, desc.handle))
                if desc.uuid.lower() == CCCD_UUID:
                    cccd_handle = desc.handle

    if cccd_handle is None:
        note("没有找到 CCCD 描述符（0x2902）")
    else:
        note("CCCD handle = %d" % cccd_handle)
    return cccd_handle


async def probe_cccd(client, cccd_handle) -> None:
    """单独读/写 CCCD：这一步失败就是板子回 0x11 的地方。"""
    step("CCCD 读 / 写（单独测试）")
    if cccd_handle is None:
        note("跳过：没有 CCCD 句柄")
        return

    for attempt in range(1, 4):
        try:
            value = await client.read_gatt_descriptor(cccd_handle)
            note("第 %d 次读 CCCD 成功: %s" % (attempt, value.hex(" ")))
            break
        except Exception as exc:
            note("第 %d 次读 CCCD 失败: %r" % (attempt, exc))
            await asyncio.sleep(0.3)

    for attempt in range(1, 4):
        try:
            await client.write_gatt_descriptor(cccd_handle, b"\x01\x00")
            note("第 %d 次写 CCCD 成功" % attempt)
            break
        except Exception as exc:
            note("第 %d 次写 CCCD 失败: %r" % (attempt, exc))
            await asyncio.sleep(0.3)


async def main() -> None:
    parser = argparse.ArgumentParser(description="MeXsmarEC BLE 连接诊断")
    parser.add_argument("--address", help="板子 MAC，不填则按广播名扫描")
    parser.add_argument("--scan", type=float, default=6.0, help="扫描秒数，默认 6")
    parser.add_argument("--seconds", type=float, default=12.0, help="订阅后持续接收秒数")
    parser.add_argument("--led", choices=["on", "off"], help="连上后发一帧 LED 广播")
    parser.add_argument("--no-notify", action="store_true", help="跳过 start_notify")
    args = parser.parse_args()

    address = await pick_device(args)

    step("连接")
    notes = []

    def on_disconnect(_client):
        notes.append("收到断开回调 @ %s" % stamp())
        note("连接断开（回调）")

    t0 = time.monotonic()
    client = BleakClient(address, disconnected_callback=on_disconnect, timeout=15.0)
    try:
        await client.connect()
    except Exception as exc:
        note("connect() 失败: %r" % exc)
        return
    note("connect() 成功，耗时 %.2f s，is_connected=%s" % (time.monotonic() - t0, client.is_connected))

    try:
        cccd_handle = dump_services(client)

        for attempt in range(1, 4):
            try:
                frame = build_frame(TYPE_LED_SET, BROADCAST, bytes([1 if args.led == "on" else 0]))
                break
            except Exception:
                pass

        if args.led:
            step("下发 LED 广播")
            try:
                await client.write_gatt_char(CHAR_WRITE_UUID, frame, response=True)
                note("写 %s 成功: %s" % (CHAR_WRITE_UUID, frame.hex(" ")))
            except Exception as exc:
                note("写失败: %r" % exc)

        await probe_cccd(client, cccd_handle)

        got = []

        def on_notify(_sender, data: bytearray):
            got.append(bytes(data))
            note("通知 %d 字节: %s" % (len(data), bytes(data).hex(" ")))

        if args.no_notify:
            note("按要求跳过 start_notify")
        else:
            step("start_notify（bleak 完整流程）")
            ok = False
            for attempt in range(1, 4):
                try:
                    await client.start_notify(CHAR_NOTIFY_UUID, on_notify)
                    note("第 %d 次 start_notify 成功" % attempt)
                    ok = True
                    break
                except Exception as exc:
                    note("第 %d 次 start_notify 失败: %r" % (attempt, exc))
                    await asyncio.sleep(0.4)
            if not ok:
                note("start_notify 三次都失败——问题就在 CCCD 订阅")

        step("保持连接 %.0f 秒观察" % args.seconds)
        await asyncio.sleep(args.seconds)
        note("期间收到 %d 条通知" % len(got))
    finally:
        step("收尾")
        try:
            await client.disconnect()
            note("已断开")
        except Exception as exc:
            note("断开异常: %r" % exc)

    for line in notes:
        note(line)


if __name__ == "__main__":
    asyncio.run(main())
