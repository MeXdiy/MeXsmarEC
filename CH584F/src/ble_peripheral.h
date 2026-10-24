/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_peripheral.h
 * Description        : Peripheral (GATT server) role of the mesh node.
 *******************************************************************************/

#ifndef __BLE_PERIPHERAL_H
#define __BLE_PERIPHERAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CONFIG.h"

/* Peripheral Task Events */
#define SBP_START_DEVICE_EVT    0x0001
#define SBP_PERIODIC_EVT        0x0002
#define SBP_READ_RSSI_EVT       0x0004
#define SBP_PARAM_UPDATE_EVT    0x0008
#define SBP_PHY_UPDATE_EVT      0x0010
#define SBP_NOTIFY_EVT          0x0020   /* chunked notification sender */

#define CONNECT0_ITEM           0
#define CONNECT1_ITEM           1
#define CONNECT2_ITEM           2

typedef struct
{
    uint8_t  taskID;
    uint16_t connHandle;
    uint16_t connInterval;
    uint16_t connSlaveLatency;
    uint16_t connTimeout;
    uint16_t mtu;               /* ATT MTU negotiated on this link */
} peripheralConnItem_t;

/*
 * @brief  Init the peripheral task (advertising + GATT service).
 */
extern void Peripheral_Init(void);

/*
 * @brief  Peripheral task event processor.
 */
extern uint16_t Peripheral_ProcessEvent(uint8_t task_id, uint16_t events);

/*
 * @brief  Send a raw frame to every connected central, skipping one handle.
 * @param  skipConn - connection handle to skip, 0xFFFF to send to all
 */
extern void Peripheral_SendExcept(uint16_t skipConn, const uint8_t *data, uint16_t len);

/*
 * @brief  Number of currently connected centrals.
 */
extern uint8_t Peripheral_LinkCount(void);

/*
 * @brief  Whether a connection handle still belongs to a live peripheral link.
 */
extern uint8_t Peripheral_LinkUp(uint16_t connHandle);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_PERIPHERAL_H */
