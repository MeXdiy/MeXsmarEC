/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_ind.h
 * Description        : Status indicator (IND_LED) for the BLE/NFC mesh node.
 *
 *                      The indicator is the first pixel of the WS2812 string
 *                      (IND_LED in ws2812.h), so it shares the strip and only
 *                      touches that one pixel; the layout area is index 1..N.
 *******************************************************************************/

#ifndef __APP_IND_H
#define __APP_IND_H

#include "CONFIG.h"
#include "CH58x_common.h"
#include "ws2812.h"

/*================== 颜色配置 (R, G, B) ==================*/
/* 颜色宏是三元组，用法:  ws2812_color_t c = { IND_COLOR_NFC_READ };
 * 直接改这里的数值即可换色。 */
#define IND_COLOR_OFF             0,   0,   0     /* 熄灭              */
#define IND_COLOR_NFC_READ        0,   180, 180   /* 青色: 读到卡      */
#define IND_COLOR_NFC_WRITE       255, 120, 0     /* 橙色: 写卡成功    */
#define IND_COLOR_BLE_ADV         0,   0,   180   /* 蓝色: 广播中      */
#define IND_COLOR_BLE_CONN        0,   180, 0     /* 绿色: 已连接      */
#define IND_COLOR_PC_LOST         255, 0,   0     /* 红色: 上位机断开  */

/*================== 时序 / 亮度配置 ==================*/
#define IND_BRIGHTNESS            30      /* 状态灯亮度 (仅 IND_OWN_BRIGHTNESS=1 时用) */
#define IND_TICK_MS               50      /* 刷新周期 (ms)                  */
#define IND_PC_BLINK_MS           250     /* 上位机断开红闪半周期 (ms)      */
#define IND_HINT_MS               500     /* 提示(读卡/写卡/蓝牙状态变化)持续 (ms)    */
#define IND_NFC_BLINK_MS          100     /* NFC 提示闪烁半周期 (ms)        */
#define IND_BLE_ADV_BLINK_MS      500     /* 蓝牙广播提示慢闪半周期 (ms)    */

/* 1 = 提示的那一帧临时用 IND_BRIGHTNESS（提示看得清，但该帧整条灯带按此亮度显示）；
 * 0 = 严格跟随上位机指令的亮度（默认：指令优先）。 */
#define IND_OWN_BRIGHTNESS        0

/*================== 蓝牙状态 ==================*/
#define IND_BLE_OFF               0       /* 未启动 / 熄灭                  */
#define IND_BLE_ADV               1       /* 广播中（未连接）               */
#define IND_BLE_CONN              2       /* 已连接                         */

/*================== NFC 事件 ==================*/
#define IND_NFC_READ              0       /* 读到卡                         */
#define IND_NFC_WRITE             1       /* 写卡成功                       */

/*********************************************************************
 * @fn      IND_Init
 *
 * @brief   Clear the indicator pixel and start the 50 ms refresh task.
 *
 * @return  none
 */
void IND_Init(void);

/*********************************************************************
 * @fn      IND_SetBleState
 *
 * @brief   Report the peripheral role state to the indicator.
 *
 * @param   state - IND_BLE_OFF / IND_BLE_ADV / IND_BLE_CONN
 *
 * @return  none
 */
void IND_SetBleState(uint8_t state);

/*********************************************************************
 * @fn      IND_SetPcOnline
 *
 * @brief   Report whether the upper computer is reachable.  While it is
 *          not, pixel 0 blinks red until it comes back; then the content
 *          that was there before is put back.
 *
 * @param   online - 1: upper computer reached, 0: lost
 *
 * @return  none
 */
void IND_SetPcOnline(uint8_t online);

/*********************************************************************
 * @fn      IND_SyncBlink
 *
 * @brief   Align the blink phase with the mesh heartbeat, so all boards
 *          flash in step.
 *
 * @return  none
 */
void IND_SyncBlink(void);

/*********************************************************************
 * @fn      IND_AlarmPhaseCB
 *
 * @brief   Weak hook, called on every refresh tick with the current phase of
 *          the "upper computer lost" alarm, so an upper layer can gate
 *          something else (for example BLE advertising) on the same rhythm.
 *
 * @param   alarmOn - 1 while the red phase is on, 0 while it is off
 *
 * @return  none
 */
void IND_AlarmPhaseCB(uint8_t alarmOn);

/*********************************************************************
 * @fn      IND_NfcEvent
 *
 * @brief   Flash the indicator for IND_NFC_FLASH_MS in the colour of the
 *          given NFC event, then fall back to the BLE state colour.
 *
 * @param   kind - IND_NFC_READ / IND_NFC_WRITE
 *
 * @return  none
 */
void IND_NfcEvent(uint8_t kind);

#endif /* __APP_IND_H */
