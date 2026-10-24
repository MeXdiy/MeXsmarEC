# MeXsmarEC

基于 **CH584F**（CH585 系列，RISC-V + BLE 5.x + NFC）的多板自组织无线网络节点固件。

每块板同时扮演 **BLE 从机 + BLE 主机 + 网状转发节点** 三种角色：板子之间自动互连组网，电脑可以用蓝牙连接**任意一块**板，并把数据/指令通达全网任意一块板。

---

## 1. 功能特性

| 功能 | 说明 |
| --- | --- |
| 板间自动组网 | 上电后自动扫描同网板（按广播名 `MeXsmarEC` / 服务 UUID `0xFFE0` 识别）并建立 BLE 连接，无需人工干预 |
| 网状转发 | 收到的消息按序号去重后向除来源外的所有链路洪泛转发，电脑可通过任一节点通达全网 |
| 电脑蓝牙接入 | 电脑可连接任意一块板，通过 GATT 收发数据 |
| NFC 读卡上报 | 板子识别到 NFC 标签后，把 **卡 UID + 标签文本 + 本板蓝牙地址** 上报到电脑 |
| NFC 写文本 | 电脑可让**任意一块**板把文本写成 NFC 文本标签（NDEF 文本记录），文本超长按标签容量截断 |
| 远程 LED 控制 | 电脑可控制**任意一块**板的 WS2812 灯带（按蓝牙地址寻址，多跳转发可达） |
| 上报节奏 | 卡片一直在场时每 5 秒重复上报一次（`NFC_REPORT_REPEAT_MS`），卡片移开再放会立即上报 |
| 两个实体按键 | `PA15` / `PB23` 低电平触发，10 ms 扫描 + 30 ms 消抖，按下 / 松开各报一次，预留 `APP_KEY_OnEvent()` 回调挂功能 |
| 状态指示灯 | 灯带第 0 颗（`IND_LED`）作状态灯：读卡闪青、写卡闪橙（各 2 秒），蓝牙状态变化提示 2 秒（广播蓝 / 连接绿），上位机断开持续红闪；提示结束把上位机的灯带内容放回 |
| 上位机在线心跳 | 电脑每秒发一帧 `0xFF` 心跳；被连的板每 2 秒把在线状态广播到全网，6 秒收不到即判离线（全网红闪） |
| 广播门控 | 上位机连着某块板时，其它板停止 BLE 广播；上位机断开后跟随红灯节奏"亮时广播、灭时停" |
| 故障现场打印 | 硬件异常时先打印 `mcause / mepc / mtval / sysLastMarker` 再软复位，用于定位"跑一会儿就死" |

---

## 2. 硬件要求

- CH584F（CH585 系列）自制板，板载：
  - 2.4G / BLE 天线（`ANT` 引脚）
  - NFC 天线（`PB17`=`NFC+`、`PB16`=`NFC-`、`PB8`=`NFCM`、`PB9`=`NFCI`）
  - **32.768 kHz 外部晶振**（含 BLE 主机角色时必须使用外部 32K）
- 下载器：WCH-Link
- 调试串口：UART1，`PB13`(`TXD1_`) / `PB12`(`RXD1_`)，**115200-8-N-1**

> 注意：CH584F 的 `PA9` 与 `PB9` 内部短接，固件已把 `PA9` 设为模拟输入并关闭数字输入。
> NFC 未使用 `NFC_CTR` 引脚。

---

## 3. 目录结构

```
CH584F/
├── src/
│   ├── Main.c            主流程：时钟 / 串口 / BLE / NFC / 按键 / 状态灯初始化 + 硬件异常现场打印 + TMOS 主循环
│   ├── ble_peripheral.c  从机角色：广播、GATT 服务、Notify 上行、广播门控
│   ├── ble_peripheral.h
│   ├── ble_central.c     主机角色：扫描同网板、建连、服务发现、Write 下行
│   ├── ble_central.h
│   ├── app_mesh.c        网状层：组帧 / 解帧、去重、洪泛转发、LED 处理、上位机在线状态机
│   ├── app_mesh.h
│   ├── app_nfc.c         NFC 轮询读卡（LPCD 检卡 + 防冲突取 UID）+ NDEF 文本读写 + 在场去重
│   ├── app_nfc.h
│   ├── app_key.c         两个按键：PA15 / PB23 低电平触发 + 消抖 + 边沿上报
│   ├── app_key.h
│   ├── app_ind.c         状态指示灯：提示 / 离线报警、闪烁相位同步、广播门控钩子
│   ├── app_ind.h
│   ├── app_led.c         把 0x02 位图指令写到 WS2812 灯带
│   ├── app_led.h
│   ├── ws2812.c          WS2812 驱动（SPI0 + DMA，101 颗灯珠）
│   └── ws2812.h
├── tools/
│   ├── metoo_ble.py      Linux 侧 BLE 客户端示例（bleak）
│   ├── ble_probe.py      Linux 侧连接诊断脚本（服务 / CCCD / Notify 逐项检查）
│   └── metoo_web/        自研上位机（aiohttp + bleak + Web 界面）
├── BLE/
│   ├── LIB/              BLE 协议栈库（libCH58xBLE.a、CH58xBLE_LIB.h、ble_task_scheduler.S）
│   ├── HAL/              HAL 层（MCU.c、RTC.c、SLEEP.c、KEY.c、LED.c、CONFIG.h）
│   └── Profile/          GATT 服务（gattprofile.c/h、devinfoservice.c/h）
├── NFCA/                 NFC-A 读卡器库（libCH58x_NFCA.a、PCD BSP、NTAG21x 驱动）
├── StdPeriphDriver/      外设驱动 + libISP585.a
├── Startup/              startup_CH585.S
├── RVMSIS/               core_riscv.h
├── Ld/Link.ld            链接脚本（FLASH 448K / RAM 96K）
└── obj/                  编译输出
```

---

## 4. 编译与烧录

1. 用 MounRiver Studio 打开工程，直接编译（`obj` 配置）。
2. 用 WCH-Link 下载：BOOT 脚 `PB22` 拉低上电进入下载模式（或免按键下载），连接 WCH-Link 后执行下载。
3. 复位后串口（115200-8-N-1）应输出：

```
Start @ChipID=92
MeXsmarEC
CH585_BLE_LIB_V1.5.09
mesh local mac: 00 00 00 00 00 00        <- 初始化早期地址尚未生效，可忽略
gs_lpcd_adc_base_value:xxxx              <- NFC 检卡基准值校准
nfc reader ready
key init: PA15/PB23 active-low           <- 按键就绪
task id peri f / peri conn 0 10 ...      <- 各 TMOS 任务号（唯一，用于排查任务占用）
ind: ble adv 2s                          <- 状态灯：广播中提示（蓝色闪 2 秒）
Initialized...
Discovering...                           <- 主机角色：开始扫描
mesh local mac: F0 EA E1 EE 32 AD        <- 真实蓝牙地址（与电脑所见一致）
Advertising...                           <- 从机角色：开始广播
ind: ble conn 2s                         <- 状态灯：连接建立提示（绿色亮 2 秒）
```

当前资源占用：**FLASH 约 195 KB / 448 KB（42.5%），RAM 约 42 KB / 96 KB（42.7%）**。

---

## 5. 组网说明

### 5.1 角色模型

每块板上电后同时做两件事：

- **从机**：以名称 `MeXsmarEC` 广播，等待被电脑或其他板连接；
- **主机**：持续扫描广播，发现同网板就主动建连。

两个方向的数据通道都接入同一个网状层，收到消息后除来源链路外向其余链路转发，形成多点对多点（mesh）结构。

为避免两块板同时向对方发起连接造成冲突，固件按**地址大小裁决**：只有**本机地址较大**的一方主动发起连接，地址较小的一方只被动接受。因此任意一对板之间只会发生一次连接动作，同一对设备不会互相重复建连。**地址最小的那块板不会主动连任何人**，它靠其他板连上来，这是正常现象。

### 5.2 连接数约束

| 项目 | 数值 | 说明 |
| --- | --- | --- |
| 从机链路 | **3** | `PERIPHERAL_MAX_CONNECTION=3`，本板最多同时被 3 个设备（电脑或其他板）连接 |
| 主机链路 | **4** | `CENTRAL_MAX_CONNECTION=4`，本板最多主动连接 4 块其他板 |
| 单板连接总数 | **7** | 3 + 4 |

> 电脑占用的也是**从机**名额：电脑连上某块板后，该板只剩 2 个名额给其他板。
> 从机上限 3 由协议栈位域固定（`ConnectNumber = (PERIPHERAL_MAX_CONNECTION & 3) | (CENTRAL_MAX_CONNECTION << 2)`），**不可再调高**；
> 主机数可调到最大 **15**（修改工程预处理宏 `CENTRAL_MAX_CONNECTION` 即可）。

### 5.3 规模建议

- **最少 2 块**即可组网；
- **≤ 8 块**：若所有板都在彼此射频范围内，每块板恰好需要 7 条连接，网络可全互通；
- **> 8 块**：全互通不再可能，但**没有硬性板数上限**——只要每块板至少连上 1 个邻居即可成网，消息靠多跳洪泛可达；
- **建议单网 ≤ 20 块**：主要受洪泛去重表（24 项）与 8 位消息序号限制，NFC 刷卡这类低频事件足够；如需更大规模或更高消息频率，请扩大 `MESH_DUP_TBL_SIZE` 并考虑改用更长的消息序号。

以 **3 块板**全互通为例，按上面的裁决规则，各板串口应打印出如下状态（可据此判断组网是否成功）：

| 板 | `peripheral` | `central` |
| --- | --- | --- |
| 地址最大 | 0 | 2 |
| 地址居中 | 1 | 1 |
| 地址最小 | 2 | 0 |

只要每块板的 `peripheral + central` 总数 ≥ 1，且全网能互相转发数据，就说明网络已连通。

### 5.4 射频资源与扫描策略

每块板只有**一套 2.4G 射频**，广播、扫描和所有 BLE 连接都要分时共用它。板子数量增加时，如果每块板都长时间扫描，会把射频时间全部吃掉，导致已有连接（尤其是电脑那条）得不到服务而超时断开。固件为此做了三层约束：

1. **被动扫描**：板的广播包里已经带了服务 UUID `0xFFE0`，因此扫描时不发 `SCAN_REQ`，省掉了每个广播者一问一答的往返流量。
2. **低占空比扫描**：扫描窗口 100ms / 间隔 500ms（约 20% 占空比），而不是协议栈默认的 10ms/10ms（100%）。
3. **扫描退避**：某一轮扫描没有发现可连的新板时，下一次扫描的间隔按 1.5s → 3s → 6s 逐级拉长并稳定在 6s；一旦发现新板开始连接，立即恢复到 1.5s。

因此**网络稳定后串口刷屏会明显减少**（`mesh links:` 大约每 6 秒一行），这是正常现象。新板加入的最长等待时间也就是这个退避上限，通常几秒内即可入网。

---

## 6. 通信协议

### 6.1 GATT 服务

| 项目 | 值 |
| --- | --- |
| 广播名 | `MeXsmarEC` |
| 服务 UUID | `0000FFE0-0000-1000-8000-00805F9B34FB` |
| 上行特征 | `0000FFE2-...`（只读）、`0000FFE4-...`（**Notify**，板 → 电脑） |
| 下行特征 | `0000FFE1-...`（读/写）、`0000FFE3-...`（只写） |

电脑侧：**订阅 `FFE4` 的 Notify 收数据**，**向 `FFE1` 写指令**。电脑链路协商后 MTU 为 247；板间链路在连接建立后也会主动交换 MTU（247），以保证带文本的帧能跨板转发。

### 6.2 应用层帧格式

单帧最长 220 字节（其中文本 200 字节）；电脑链路与板间链路都会协商 247 的 MTU，因此不需要分包。

```
偏移    长度   字段        说明
0       1     head        固定 0xA5
1       4     msgId       消息序号（uint32_t，**高位在前**），用于洪泛去重
5       1     type        消息类型
6       6     addr        0x01/0x03 = 源板 MAC，0x02/0x04 = 目标板 MAC
12      N     data        负载
```

`addr` 为 **6 字节蓝牙地址，高位在前**，与电脑扫描/连接时显示的地址完全一致（例如 `F0:EA:E1:EE:32:AD` → `F0 EA E1 EE 32 AD`）。

### 6.3 消息类型

| type | 名称 | 长度 | 方向 | data 含义 |
| --- | --- | --- | --- | --- |
| `0x01` | NFC 事件上报 | 20 ~ 220 | 板 → 电脑 | `[12..18]` = 卡 UID（7 字节，4 字节 UID 的卡后 3 字节补 0）；`[19]` = 文本长度 n；`[20..]` = 标签文本（n 字节；标签无文本记录时 n = 0） |
| `0x02` | LED 控制 (位图) | 48 | 电脑 → 板 | `[12]` = 亮度(0~255)；`[13..15]` = R, G, B (0~255)；`[16..47]` = 256个LED的32字节位图（`bit=1` 亮，`bit=0` 灭）；`addr` = 目标板 MAC |
| `0x03` | LED 回执 | 13 | 板 → 电脑 | `[12]` = 实际执行的开关值；`addr` = 执行板 MAC |
| `0x04` | NFC 写文本 | 12 ~ 212 | 电脑 → 板 | `[12..]` = 待写入的文本（UTF-8）；`addr` = 目标板 MAC |
| `0xFF` | 上位机在线心跳 | 13 | 双向 | `[12]` = 1 在线 / 0 离线。**电脑 → 板**：`addr` 填广播地址，每秒一帧；**板 → 全网**：`addr` = 发送板 MAC，被上位机连着的那块板每 2 秒广播一次 |

地址填 `FF FF FF FF FF FF` 表示广播：`0x02` 时所有板都执行 LED 动作（不回执），`0x04` 时所有板都会待写入同一段文本。

`0xFF` 的两种来源靠 `addr` 区分：**广播地址 = 电脑自己的 ping**（收到它的板就是被连着的那块板），**普通 MAC = 某块板广播的在线状态**。被连的板收到 ping 后会用自己的 MAC 重新广播状态、而不把 ping 洪泛出去——否则每块转发它的板都会以为电脑连在自己身上，各自超时时报的离线会把全网状态打掉。

`0x04` 在**收到之后放到天线上的第一张卡**上生效（写入成功后自动清除，需要再写下一张时重新下发）；文本超出标签容量会被截断；data 长度为 0 表示取消待写入的文本。

### 6.4 使用示例

假设电脑扫描到板子地址为 `F0:EA:E1:EE:32:AD`。

**控制该板点亮第 0 号和第 1 号 LED（例如点亮箱指示和第一个元件位）** —— 向 `FFE1` 写入：

```
A5 00 00 00 01 02 F0 EA E1 EE 32 AD FF 00 FF 00 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
> 解析：`FF`=亮度，`00 FF 00`=绿色，`03 00 ...`=位图（二进制 `0000 0011`，即点亮 0 和 1）。

**控制全网所有板熄灭所有 LED** —— 向 `FFE1` 写入：


**控制全网所有板熄灭 LED** —— 向 `FFE1` 写入：

```
A5 00 00 00 02 02 FF FF FF FF FF FF FF 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
> 解析：所有位图字节全为 0，即全灭。


**让该板把 `Hello` 写成文本标签** —— 向 `FFE1` 写入：

```
A5 00 00 00 03 04 F0 EA E1 EE 32 AD 48 65 6C 6C 6F
```

**接收 NFC 上报** —— 订阅 `FFE4` 后，把写着 `Hello` 的标签放到天线上，会收到：

```
A5 00 00 00 04 01 F0 EA E1 EE 32 AD 84 67 62 A8 05 48 65 6C 6C 6F
│  └────┬───┘  │  └───────┬───────┘ └────┬────┘ │  └──────┬──────┘
│    msgId(4B) │     源板 MAC(6B)    卡 UID(7B) │    文本(5B)
│              └ type=0x01 NFC 事件            └ 文本长度 = 5
└ head
```

表示：板 `F0:EA:E1:EE:32:AD` 读到了 UID 为 `84 67 62 A8` 的卡，标签文本是 `Hello`（无文本记录时该处长度为 `00`）。

> 电脑端如使用通用 BLE 调试工具（nRF Connect / LightBlue 等），直接按上表手工收发即可；
> 如需自动化，使用任一 PC 侧 BLE 库按同一帧格式收发，固件无需改动。

### 6.5 Linux 上用 Python 连接（BlueZ + bleak）

工程内已带一份可直接使用的客户端 `tools/metoo_ble.py`：

```bash
pip install bleak

# 扫描并连上名称以 MeXsmarEC 开头的板子，持续打印 NFC 上报
python3 tools/metoo_ble.py listen

# 指定板子蓝牙地址
python3 tools/metoo_ble.py listen --device F0:EA:E1:EE:32:AD

# 控制某块板 / 全网的 LED
python3 tools/metoo_ble.py led on  --target F0:EA:E1:EE:32:AD
python3 tools/metoo_ble.py led off --target FF:FF:FF:FF:FF:FF

# 让某块板把文本写成 NFC 标签（下发后把标签放到该板天线上）
python3 tools/metoo_ble.py write "Hello" --target F0:EA:E1:EE:32:AD
```

脚本会自动订阅 `FFE4`、向 `FFE1` 写指令，并打印协商到的 ATT MTU 与全部特征列表。

**关于 MTU 与分包**：链路 MTU 由主机（PC）侧决定，Windows 调试工具和 BlueZ 通常都会协商到 247，此时 220 字节的帧一次就能发完；若主机没有抬高 MTU（仍是 23），固件会把放不下的帧自动拆成多片发出，每片格式为：

```
[0] 0x5A      分片魔数（mesh 帧头固定是 0xA5，两者不会混淆）
[1] 序号       从 0 开始
[2] 分片总数
[3..]         数据
```

PC 侧按序号收齐后拼回完整 mesh 帧即可，`tools/metoo_ble.py` 已经实现了这一步。

> 在 `bluetoothctl` 里手工 `notify on` 时，请先用 `list-attributes` 确认选中的是 `0000ffe4` 对应的那一行；选到别的特征会直接报 `org.bluez.Error.Failed`。
> 另外，从机不再在连接建立时抢先把 CCCD 置为使能——BlueZ 会读取 CCCD，读到"已使能"就拒绝 `StartNotify`，这就是之前在 Linux 上 `notify on` 失败、而 Windows 调试工具正常的原因。

### 6.6 上位机（本地 Web 界面）

`tools/metoo_web/` 是一套自研上位机：Python 后端（`aiohttp` + `bleak`）负责 BLE 收发，界面在浏览器里，PC 端不需要装 GUI 依赖。

```bash
cd tools/metoo_web
pip install -r requirements.txt        # bleak + aiohttp
sudo apt install bluez                 # BlueZ >= 5.43，bleak 依赖它

# 访问 BLE 需要 cap_net_raw / cap_net_admin，下面二选一：
sudo setcap 'cap_net_raw,cap_net_admin+eip' "$(readlink -f "$(command -v python3)")"
sudo python3 server.py                 # 或 sudo 直接运行
```

启动后浏览器打开 `http://127.0.0.1:8765/`（默认只监听本机，改 `--host 0.0.0.0` 可放开）。

| 页面 | 功能 |
| --- | --- |
| NFC 上报 | 实时表格：时间 / 上报节点 / UID / 标签文本，可按 `msgId` 去重 |
| LED 控制 | 选发送通道与目标板（或广播），点亮 / 熄灭 |
| 写卡 | 下发 `NFC_WRITE` 文本（上限 200 字节），板子随后写入标签 |
| 调试 | 手工发十六进制帧，并显示收发原始字节流 |

两个容易混的概念：「发送通道」是 PC 实际连着的板子；「目标板」是帧里的 `addr` 字段，填 `FF:FF:FF:FF:FF:FF` 表示全网广播。连多块板时它们都会出现在左侧设备列表，各自独立订阅 `FFE4` 并做分片重组。

### 6.7 上位机在线判定（心跳 / 超时 / 全网红闪）

- **电脑侧**：连上后每秒发一帧 `0xFF` 心跳（`tools/metoo_web/server.py` 的 `_ping_loop`，直接写 `FFE1`，不刷界面日志）。
- **被连的板（uplink）**：收到心跳就刷新 6 秒计时，并**每 2 秒**用自己的 MAC 广播一次 `online 1`；**只有它广播**，其它板只转发不重播。
- **其它板**：收到 `online 1` 就刷新同一个 6 秒计时，超时即判"上位机离线"。
- **判离线**：立即广播一次 `online 0` 并开始**红闪**。`online 0` 只被"最近报过在线的那块板"接受，避免一块自己链路断掉的板把全网状态打掉（级联误报）。
- **相位同步**：所有板以"发出/收到那帧 `online`"为相位基准，且 2 秒心跳恰好是红闪半周期 250 ms 的整数倍，漏掉一帧也不会错相。
- **恢复**：再收到 `online 1` 即停红闪，并把断开前索引 0 的内容原样放回（不会留下"常亮红"）。
- **广播门控**：上位机连着某块板时，其它板停止 BLE 广播（等价于"红灯灭"）；上位机断开后跟随红闪节奏脉冲广播（红灯亮广播、灯灭停广播）。

---

## 7. 二次开发

### 7.1 灯带与状态灯

WS2812 驱动在 `src/ws2812.c`（SPI0 + DMA，101 颗灯珠），`LED_Ctrl()` 把 `0x02` 位图指令写到灯带。灯带第 0 颗是**状态灯**（`IND_LED`，实现在 `src/app_ind.c/h`），由一个 50 ms 的 TMOS 任务驱动：

- **只在提示或报警期间**才写索引 0，其余时间完全不碰灯带，上位机位图的内容得以保留；
- **提示**：读卡闪青、写卡闪橙、蓝牙状态变化闪蓝（广播中）/绿（已连接），各 2 秒，结束后把提示前的内容放回；
- **报警**：上位机离线时红闪（半周期 250 ms），恢复后同样把内容放回，不会停在报警色上；
- 提示 / 报警帧亮度**不低于 `IND_BRIGHTNESS`(30)**，上位机把亮度压到 2 时提示依然看得见；想严格跟随上位机亮度，把 `IND_OWN_BRIGHTNESS` 置 0；
- 颜色 / 时长 / 亮度都在 `src/app_ind.h` 的宏里改。

`src/app_led.c` 里实际写入灯带的函数：

```c
void LED_Ctrl(const uint8_t *payload)
{
    uint8_t brightness = payload[0];
    uint8_t r          = payload[1];
    uint8_t g          = payload[2];
    uint8_t b          = payload[3];
    
    // 1. 设置全局亮度
    WS2812_SetBrightness(brightness);

    // 2. 遍历 32 字节的位图，控制 256 个 LED
    for (int byte_idx = 0; byte_idx < 32; byte_idx++) {
        uint8_t mask = payload[4 + byte_idx];
        for (int bit = 0; bit < 8; bit++) {
            int led_index = byte_idx * 8 + bit;
            if (mask & (1 << bit)) {
                WS2812_SetPixel(led_index, r, g, b); // 位为 1，点亮
            } else {
                WS2812_SetPixel(led_index, 0, 0, 0); // 位为 0，熄灭
            }
        }
    }

    // 3. 统一刷新显示（保证所有 LED 同时生效）
    WS2812_Show();
}
```

板子收到针对自己的 LED 指令时会串口打印 `mesh in from central 2 len 48` + `LED_Ctrl bitmap for this node`，便于确认指令已到达。

### 7.2 常用可调参数

| 参数 | 位置 | 当前值 | 说明 |
| --- | --- | --- | --- |
| `NFC_POLL_PERIOD_MS` | `src/app_nfc.c` | 300 | NFC 轮询周期（ms） |
| `NFC_REPORT_REPEAT_MS` | `src/app_nfc.c` | 5000 | 卡片一直在场时的重复上报间隔（ms） |
| `NFC_CARD_GONE_CNT` | `src/app_nfc.c` | 6 | 连续多少次无卡才判定卡片离开 |
| `NFC_WRITE_TEST` | `src/app_nfc.c` | 0 | 1 = 上电只做写卡自测（正常模式保持 0） |
| `MESH_NFC_TEXT_MAX` | `src/app_mesh.h` | 200 | 单帧承载的 NFC 文本上限（字节），超出按标签容量截断 |
| `MESH_DUP_TBL_SIZE` | `src/app_mesh.c` | 24 | 洪泛去重表容量 |
| `MESH_PC_TIMEOUT_MS` | `src/app_mesh.c` | 6000 | 上位机心跳超时（ms），超时即判离线红闪 |
| `MESH_PC_BCAST_MS` | `src/app_mesh.c` | 2000 | 被连板广播在线状态的周期（ms） |
| `KEY_SCAN_PERIOD_MS` / `KEY_DEBOUNCE_CNT` | `src/app_key.c` | 10 / 3 | 按键扫描周期（ms）/ 消抖次数 |
| `IND_TICK_MS` / `IND_HINT_MS` | `src/app_ind.h` | 50 / 2000 | 状态灯刷新周期 / 提示持续时长（ms） |
| `IND_NFC_BLINK_MS` / `IND_PC_BLINK_MS` | `src/app_ind.h` | 250 | 提示闪烁 / 离线红闪半周期（ms） |
| `IND_BRIGHTNESS` / `IND_OWN_BRIGHTNESS` | `src/app_ind.h` | 30 / 1 | 提示帧亮度下限 / 是否启用该下限 |
| `PING_PERIOD` | `tools/metoo_web/server.py` | 1.0 | 上位机心跳周期（秒） |
| `DEFAULT_MAX_SCAN_RES` | `src/ble_central.c` | 10 | 每轮扫描保留的设备数 |
| `DEFAULT_DISCOVERY_ACTIVE_SCAN` | `src/ble_central.c` | `FALSE` | 被动扫描（广播包已含 0xFFE0，无需 SCAN_REQ） |
| `DEFAULT_SCAN_WINDOW` | `src/ble_central.c` | 160 | 扫描窗口，单位 0.625ms（=100ms） |
| `DEFAULT_SCAN_INTERVAL` | `src/ble_central.c` | 800 | 扫描间隔，单位 0.625ms（=500ms，约 20% 占空比） |
| `SCAN_BACKOFF_MIN` / `SCAN_BACKOFF_MAX` | `src/ble_central.c` | 2400 / 9600 | 无新设备时扫描退避的下限/上限（1.5s / 6s） |
| `PERIPHERAL_MAX_CONNECTION` | 工程预处理宏 | 3 | 从机链路数（协议栈上限 3） |
| `CENTRAL_MAX_CONNECTION` | 工程预处理宏 | 4 | 主机链路数（可调，上限 15） |
| `BLE_MEMHEAP_SIZE` | 工程预处理宏 | `10*1024` | BLE 协议栈内存堆 |
| `CLK_OSC32K` | 工程预处理宏 | 0 | 0=外部 32K 晶振（含主机角色必须为 0） |

### 7.3 扩展消息类型

1. 在 `src/app_mesh.h` 增加 `MESH_TYPE_xxx` 与对应帧长宏；
2. 在 `src/app_mesh.c` 的 `MESH_OnFrame()` 中增加该类型的本地处理分支（如需转发则无需改动，非本地处理的帧会自动继续洪泛）；
3. 如需主动上报，参照 `MESH_ReportNfc()` 写一个发送函数（帧头 12 字节，`msgId` 用 `meshSetMsgId()` 按 4 字节高位在前填入）；
4. 帧的最大长度受 ATT MTU 限制（247 → 负载 244 字节），文本类负载上限见 `MESH_NFC_TEXT_MAX`。
5. `MESH_OnFrame()` 入口的长度下限是 `MESH_LEN_PC_STATE`(13)，比它短的帧会被直接丢弃——新类型的帧长不能小于 13。

---

## 8. 串口日志说明

| 日志 | 来源 | 含义 |
| --- | --- | --- |
| `mesh local mac: xx xx xx xx xx xx` | `app_mesh.c` | 本板蓝牙地址，首次为全 0（GAP 未就绪），第二行是正确的 |
| `mesh board xxxxxx found, connecting...` | `ble_central.c` | 发现同网板并发起连接 |
| `mesh links: peripheral X / central Y` | `ble_central.c` | 每轮扫描结束打印一次组网状态：被连接数 / 主动连出数；`X`、`Y` 都长期为 0 才说明没连上 |
| `Connected...` / `Conn 1 - Int xx` | `ble_central.c` | 板间连接建立成功 |
| `Found Profile Service / Characteristic / CCCD handle` | `ble_central.c` | 对端服务发现成功 |
| `adc_value:xxxx` | NFC 库 | LPCD 天线信号采样值 |
| `ATQA:0x0004` + `nfc uid: xx xx xx ...` | `app_nfc.c` | 成功读到卡片 UID（卡片在场期间只打印一次） |
| `nfc text(n): xxxx` | `app_nfc.c` | 读到标签文本（`n` 为长度，标签无文本记录时为 0） |
| `nfc pending text(n): xxxx` / `nfc pending text cleared` | `app_nfc.c` | 收到电脑下发的写文本指令 / 取消指令 |
| `nfc text written(n)` / `nfc text write failed` | `app_nfc.c` | 文本写入标签的结果，`n` 为实际写入字节数（被截断时小于下发长度） |
| `STATUS ERROR: 4` | NFC 库 | 单次通信超时（未拿到卡响应），属正常重试路径 |
| `nfc uid: ...` 后的 `peri notify 2 len 20` | `ble_peripheral.c` | 已把 NFC 帧 Notify 给电脑（长度随文本长度变化） |
| `central write 1 len 20` | `ble_central.c` | 已把 NFC 帧转发给板间链路 |
| `mesh nfc node ... uid ... text len N` | `app_mesh.c` | 转发经过的 NFC 上报帧 |
| `NFC write text(n) for this node` | `app_mesh.c` | 收到针对本机（或广播）的写文本指令 |
| `mesh in from central 2 len 48` + `LED_Ctrl bitmap for this node` | `app_mesh.c` | 收到电脑下发的 48 字节位图控制指令并执行 |
| `key 1/2 pressed` / `key 1/2 released` | `app_key.c` | 按键边沿（30 ms 消抖，长按只报一次按下） |
| `ind: ble adv 2s` / `ind: ble conn 2s` | `app_ind.c` | 状态灯蓝牙状态提示（广播中蓝闪 / 已连接绿亮，各 2 秒）。**广播提示只在开机后第一次开始广播时给一次**：广播开关会被广播门控每 250 ms 切换，若按 GAP 状态反复触发会把提示重启成"常亮" |
| `ind: nfc read 2s` / `ind: nfc write 2s` | `app_ind.c` | NFC 读 / 写触发的 2 秒提示（写卡优先，不会被同一轮的读卡顶掉） |
| `ind: hint done` | `app_ind.c` | 提示结束，索引 0 交还上位机内容 |
| `ind: pc lost` / `ind: pc back` | `app_ind.c` | 上位机离线开始红闪 / 恢复并把内容放回 |
| `nfc poll alive: card X miss Y reports Z` | `app_nfc.c` | NFC 轮询心跳（约 6 秒一条），`reports` 是累计上报次数 |
| `pc ping received` | `app_mesh.c` | 收到电脑心跳（说明本板就是被连着的那块板） |
| `pc state -> 1` / `pc state -> 0` | `app_mesh.c` | 本板判定的上位机在线状态发生变化 |
| `pc heartbeat timeout` | `app_mesh.c` | 6 秒没收到心跳，判离线（随后红闪） |
| `pc link x closed` | `app_mesh.c` | 电脑那条链路断开，立即判离线（不等超时） |
| `notify off h x cccd 0` | `gattprofile.c` | 该链路没订阅，通知被丢弃（只在状态变化时打印一次） |
| `cccd write h x len 2 st 0` | `gattprofile.c` | 电脑订阅 `FFE4` 成功（`st 0` 为成功） |
| `FAULT mcause .. mepc .. mtval .. marker ..` | `Main.c` | 硬件异常现场；`marker` 是异常前最后走到的位置（0x12 选卡前 / 0x13 上报前 / 0x31 发通知中） |

---

## 9. 注意事项

1. **32.768 kHz 外部晶振必需**：含 BLE 主机角色时必须使用外部 32K，否则主机侧扫描/连接会不稳定。
2. **NFC 与 BLE 是两套独立射频**（13.56MHz / 2.4GHz），可共存；但 NFC 天线工作时会对 2.4G 有轻微影响，实测板间连接稳定。
3. **电脑端不要连多块板并同时触发重复指令**：同一块板被电脑和板间链路同时送达同一指令时，网状层的去重机制会保证只执行一次。
4. **地址字节序**：固件上报的板地址是**高位在前**（与电脑所见一致），直接填回 LED 控制帧即可，无需转换。
5. **从机上限 3 不可调**，若需被更多设备同时连接需要改动协议栈，不可行；如需提升组网规模请增加 `CENTRAL_MAX_CONNECTION`。
6. **NFC 文本写入要求标签是已格式化的 NDEF 卡**（NTAG213/215/216 出厂即已格式化）：未格式化的标签会被跳过并打印 `nfc tag has no NDEF capability container, skip write`，不会破坏卡片；文本上限 200 字节，超出标签数据区容量的部分被截断（NTAG213 ≈ 134 字节、NTAG215 ≈ 486 字节、NTAG216 ≈ 862 字节），实际写入长度以 `nfc text written(n)` 为准。
7. **NFC 写入的时序与失败定位**：每写完一页都会先等标签把 EEPROM 编程完（t_WRITE，10ms）再发下一条命令——NTAG21x 在编程期间不响应任何命令，不等就会从第二页开始收到 NAK。写完后还会读回第一块数据做校验，日志会指出失败在哪一层：
   - `nfc write failed at page nn`：写命令当页就被拒——标签不是 NTAG21x 类卡片；
   - `nfc write verify mismatch`：标签回了 ACK 但数据没进 EEPROM——**场强不足**（标签离天线太远或天线调谐偏了）；
   - `nfc write verify read failed`：写完标签就失联——同样是耦合问题。
   也就是说，"读得到、写不进"时先看日志落在哪一条：第一条是卡片类型/软件问题，后两条属于硬件（天线耦合）问题，把标签贴近再试即可确认。
8. **状态灯与上位机灯带的关系**：索引 0 平时不管，只在上位机离线报警（红闪）或提示（读 / 写 / 蓝牙状态，各 2 秒）期间才驱动；提示结束会把断开前的内容放回，不会停在报警色上。上位机位图的 bit0 仍然会点亮索引 0，如果觉得和报警红容易混，可以把 `LED_Ctrl()` 改成跳过索引 0。
9. **广播门控的代价**：上位机连着某块板时其它板停播（等价于"红灯灭"），所以**上位机（重新）连接可能要重试几次**才能抓到广播脉冲；组网也会以"被连着的那块板"为中心，它最多挂 3 条从机链路（`PERIPHERAL_MAX_CONNECTION`），超出后未入网的板会一直红闪。
10. **多块板同时被电脑连接**：在线状态按"最近报过在线的来源"判定，某块板自己链路断掉时报的离线不会把其它板的状态一起打掉。
