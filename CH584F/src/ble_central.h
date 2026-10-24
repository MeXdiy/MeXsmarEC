/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_central.h
 * Description        : Central (GATT client) role of the mesh node.
 *******************************************************************************/

#ifndef __BLE_CENTRAL_H
#define __BLE_CENTRAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CONFIG.h"

/* Central Task Events */
#define START_DEVICE_EVT              0x0001
#define START_DISCOVERY_EVT           0x0002
#define START_SCAN_EVT                0x0004
#define START_SVC_DISCOVERY_EVT       0x0008
#define START_PARAM_UPDATE_EVT        0x0010
#define START_READ_OR_WRITE_EVT       0x0020
#define START_READ_RSSI_EVT           0x0040
#define ESTABLISH_LINK_TIMEOUT_EVT    0x0080
#define START_WRITE_CCCD_EVT          0x0100
#define START_DISC_TIMEOUT_EVT        0x0200

/* How often the service discovery watchdog retries before giving up */
#define CENTRAL_DISC_RETRY_MAX        3

#define CONNECT0_ITEM                 0
#define CONNECT1_ITEM                 1
#define CONNECT2_ITEM                 2
#define CONNECT3_ITEM                 3

typedef struct
{
    uint8_t  taskID;
    uint16_t connHandle;
    uint8_t  state;
    uint8_t  peerAddr[B_ADDR_LEN];
    uint8_t  discState;
    uint8_t  procedureInProgress;
    uint16_t svcStartHdl;
    uint16_t svcEndHdl;
    uint16_t charHdl;
    uint16_t cccdHdl;
} centralConnItem_t;

typedef struct
{
    uint8_t peerAddr[B_ADDR_LEN];
} peerAddrDefItem_t;

/*
 * @brief  Init the central task (scanning + connecting other mesh boards).
 */
extern void Central_Init(void);

/*
 * @brief  Central task event processor.
 */
extern uint16_t Central_ProcessEvent(uint8_t task_id, uint16_t events);

/*
 * @brief  Send a raw frame to every connected peer, skipping one handle.
 * @param  skipConn - connection handle to skip, 0xFFFF to send to all
 */
extern void Central_SendExcept(uint16_t skipConn, const uint8_t *data, uint16_t len);

/*
 * @brief  Number of currently connected peers.
 */
extern uint8_t Central_LinkCount(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_CENTRAL_H */
