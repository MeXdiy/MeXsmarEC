/********************************** (C) COPYRIGHT *******************************
 * File Name          : multiCentral.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2022/03/18
 * Description        : 主机多连接例程，主动扫描周围设备，连接至给定的三个从机设备地址
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "CONFIG.h"
#include "gattprofile.h"
#include "ble_central.h"
#include "ble_peripheral.h"
#include "app_mesh.h"

/*********************************************************************
 * MACROS
 */

// Length of bd addr as a string
#define B_ADDR_STR_LEN                      15

/*********************************************************************
 * CONSTANTS
 */
// Maximum number of scan responses
#define DEFAULT_MAX_SCAN_RES                10

// Scan duration in 0.625ms
#define DEFAULT_SCAN_DURATION               2400

// Connection min interval in 1.25ms
#define DEFAULT_MIN_CONNECTION_INTERVAL     20

// Connection max interval in 1.25ms
#define DEFAULT_MAX_CONNECTION_INTERVAL     100

// Connection supervision timeout in 10ms
#define DEFAULT_CONNECTION_TIMEOUT          100

// Discovery mode (limited, general, all)
#define DEFAULT_DISCOVERY_MODE              DEVDISC_MODE_ALL

// Passive scan: the mesh boards carry the service UUID 0xFFE0 in their
// advertising data, so there is no need to send SCAN_REQ to every advertiser.
// Active scanning by every board floods the 2.4G band and starves the links.
#define DEFAULT_DISCOVERY_ACTIVE_SCAN       FALSE

// Scan window in 0.625ms (100ms)
#define DEFAULT_SCAN_WINDOW                 160

// Scan interval in 0.625ms (500ms).  Together with the window above this is a
// ~20% scan duty cycle instead of the default 100%, which leaves the radio
// free for the BLE links.
#define DEFAULT_SCAN_INTERVAL               800

// TRUE to use white list during discovery
#define DEFAULT_DISCOVERY_WHITE_LIST        FALSE

// TRUE to use high scan duty cycle when creating link
#define DEFAULT_LINK_HIGH_DUTY_CYCLE        FALSE

// TRUE to use white list when creating link
#define DEFAULT_LINK_WHITE_LIST             FALSE

// Default read RSSI period in 0.625ms
#define DEFAULT_RSSI_PERIOD                 2400

// Minimum connection interval (units of 1.25ms)
#define DEFAULT_UPDATE_MIN_CONN_INTERVAL    20

// Maximum connection interval (units of 1.25ms)
#define DEFAULT_UPDATE_MAX_CONN_INTERVAL    100

// Slave latency to use parameter update
#define DEFAULT_UPDATE_SLAVE_LATENCY        0

// Supervision timeout value (units of 10ms)
#define DEFAULT_UPDATE_CONN_TIMEOUT         600

// Default passcode
#define DEFAULT_PASSCODE                    0

// Default GAP pairing mode
#define DEFAULT_PAIRING_MODE                GAPBOND_PAIRING_MODE_WAIT_FOR_REQ

// Default MITM mode (TRUE to require passcode or OOB when pairing)
#define DEFAULT_MITM_MODE                   TRUE

// Default bonding mode, TRUE to bond, max bonding 6 devices
#define DEFAULT_BONDING_MODE                TRUE

// Default GAP bonding I/O capabilities
#define DEFAULT_IO_CAPABILITIES             GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT

// Default service discovery timer delay in 0.625ms
#define DEFAULT_SVC_DISCOVERY_DELAY         1600

// Default parameter update delay in 0.625ms
#define DEFAULT_PARAM_UPDATE_DELAY          3200

// Default read or write timer delay in 0.625ms
#define DEFAULT_READ_OR_WRITE_DELAY         1600

// Default write CCCD delay in 0.625ms
#define DEFAULT_WRITE_CCCD_DELAY            1600

// Establish link timeout in 0.625ms
#define ESTABLISH_LINK_TIMEOUT              3200

// Period of the service discovery watchdog in 0.625ms (1 s)
#define DISC_TIMEOUT_TICKS                  1600

// Delay before the next scan round when a round found nothing to connect, in
// 0.625ms.  It doubles up to SCAN_BACKOFF_MAX while the mesh is already up, so
// a board with all its neighbours connected scans only now and then instead of
// keeping the radio busy all the time.
#define SCAN_BACKOFF_MIN                    2400
#define SCAN_BACKOFF_MAX                    9600

// Application states
enum
{
    BLE_STATE_IDLE,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTING
};

// Discovery states
enum
{
    BLE_DISC_STATE_IDLE, // Idle
    BLE_DISC_STATE_SVC,  // Service discovery
    BLE_DISC_STATE_CHAR, // Characteristic discovery
    BLE_DISC_STATE_CCCD  // client characteristic configuration discovery
};
/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

// Task ID for internal task/event processing
static uint8_t centralTaskId;

// Number of scan results
static uint8_t centralScanRes;

// Scan result list
static gapDevRec_t centralDevList[DEFAULT_MAX_SCAN_RES];

// Address of the peer that is being connected to
static uint8_t centralPendingAddr[B_ADDR_LEN];
static uint8_t centralPendingAddrType;

// Own BLE address
static uint8_t centralOwnAddr[B_ADDR_LEN];

// Connection item list
static centralConnItem_t centralConnList[CENTRAL_MAX_CONNECTION];

// Value to write
static uint8_t centralCharVal = 0x5A;

// Value read/write toggle
static uint8_t centralDoWrite = TRUE;

// Current delay before the next scan round
static uint16_t centralScanBackoff = SCAN_BACKOFF_MIN;

// Service discovery watchdog: retry counter and "GATT procedure pending" tick
// counter, one slot per connection item.  A lost ATT response must never leave
// a mesh link with charHdl == 0 (nothing is forwarded) or procedureInProgress
// stuck at TRUE (every later frame is dropped) for good.
static uint8_t centralDiscRetry[CENTRAL_MAX_CONNECTION];
static uint8_t centralProcBusyTicks[CENTRAL_MAX_CONNECTION];

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void     centralProcessGATTMsg(gattMsgEvent_t *pMsg);
static void     centralRssiCB(uint16_t connHandle, int8_t rssi);
static void     centralEventCB(gapRoleEvent_t *pEvent);
static void     centralHciMTUChangeCB(uint16_t connHandle, uint16_t maxTxOctets, uint16_t maxRxOctets);
static void     centralPasscodeCB(uint8_t *deviceAddr, uint16_t connectionHandle,
                                  uint8_t uiInputs, uint8_t uiOutputs);
static void     centralPairStateCB(uint16_t connHandle, uint8_t state, uint8_t status);
static uint16_t connect_ProcessEvent(uint8_t task_id, uint16_t events);
static void     central_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
static void     centralGATTDiscoveryEvent(uint8_t connItem, gattMsgEvent_t *pMsg);
static void     centralConnListStartDiscovery(uint8_t connItem);
static void     centralAddDeviceInfo(uint8_t *pAddr, uint8_t addrType);
static void     centralInitConnItem(uint8_t task_id, centralConnItem_t *centralConnList);
static uint8_t  centralIsMeshAdvert(uint8_t *pData, uint8_t len);
static uint8_t  centralAddrConnected(uint8_t *addr);
static uint8_t  centralAddrGreater(const uint8_t *a, const uint8_t *b);

/*********************************************************************
 * PROFILE CALLBACKS
 */

// GAP Role Callbacks
static gapCentralRoleCB_t centralRoleCB = {
    centralRssiCB,        // RSSI callback
    centralEventCB,       // Event callback
    centralHciMTUChangeCB // MTU change callback
};

// Bond Manager Callbacks
static gapBondCBs_t centralBondCB = {

    centralPasscodeCB,
    centralPairStateCB
};

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      Central_Init
 *
 * @brief   Initialization function for the Central App Task.
 *          This is called during initialization and should contain
 *          any application specific initialization (ie. hardware
 *          initialization/setup, table initialization, power up
 *          notification).
 *
 * @param   task_id - the ID assigned by TMOS.  This ID should be
 *                    used to send messages and set timers.
 *
 * @return  none
 */
void Central_Init(void)
{
    centralTaskId = TMOS_ProcessEventRegister(Central_ProcessEvent);
    PRINT("task id central %x\n", centralTaskId);

    // Setup GAP
    GAP_SetParamValue(TGAP_DISC_SCAN, DEFAULT_SCAN_DURATION);
    GAP_SetParamValue(TGAP_DISC_SCAN_WIND, DEFAULT_SCAN_WINDOW);
    GAP_SetParamValue(TGAP_DISC_SCAN_INT, DEFAULT_SCAN_INTERVAL);
    GAP_SetParamValue(TGAP_CONN_EST_INT_MIN, DEFAULT_MIN_CONNECTION_INTERVAL);
    GAP_SetParamValue(TGAP_CONN_EST_INT_MAX, DEFAULT_MAX_CONNECTION_INTERVAL);
    GAP_SetParamValue(TGAP_CONN_EST_SUPERV_TIMEOUT, DEFAULT_CONNECTION_TIMEOUT);

    // Setup the GAP Bond Manager
    {
        uint32_t passkey = DEFAULT_PASSCODE;
        uint8_t  pairMode = DEFAULT_PAIRING_MODE;
        uint8_t  mitm = DEFAULT_MITM_MODE;
        uint8_t  ioCap = DEFAULT_IO_CAPABILITIES;
        uint8_t  bonding = DEFAULT_BONDING_MODE;

        GAPBondMgr_SetParameter(GAPBOND_CENT_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_CENT_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_CENT_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_CENT_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_CENT_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    // Init Connection Item
    centralInitConnItem(centralTaskId, centralConnList);
    // Initialize GATT Client
    GATT_InitClient();
    // Register to receive incoming ATT Indications/Notifications
    GATT_RegisterForInd(centralTaskId);
    // Setup a delayed profile startup
    tmos_set_event(centralTaskId, START_DEVICE_EVT);
}

/*********************************************************************
 * @fn      centralInitConnItem
 *
 * @brief   Init Connection Item
 *
 * @param   task_id -
 *          centralConnList -
 *
 * @return  NULL
 */
static void centralInitConnItem(uint8_t task_id, centralConnItem_t *centralConnList)
{
    uint8_t connItem;
    for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
    {
        // 每个连接的任务通过taskID区分
        centralConnList[connItem].taskID = TMOS_ProcessEventRegister(Central_ProcessEvent);
        PRINT("task id central conn %d %x\n", connItem, centralConnList[connItem].taskID);
        centralConnList[connItem].connHandle = GAP_CONNHANDLE_INIT;
        centralConnList[connItem].state = BLE_STATE_IDLE;
        centralConnList[connItem].discState = BLE_DISC_STATE_IDLE;
        centralConnList[connItem].procedureInProgress = FALSE;
        centralConnList[connItem].charHdl = 0;
        centralConnList[connItem].cccdHdl = 0;
        centralConnList[connItem].svcStartHdl = 0;
        centralConnList[connItem].svcEndHdl = 0;
    }
}

/*********************************************************************
 * @fn      Central_ProcessEvent
 *
 * @brief   Central Application Task event processor.  This function
 *          is called to process all events for the task.  Events
 *          include timers, messages and any other user defined events.
 *
 * @param   task_id  - The TMOS assigned task ID.
 * @param   events - events to process.  This is a bit map and can
 *                   contain more than one event.
 *
 * @return  events not processed
 */
uint16_t Central_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  PRINT("Task id : %#x \n",task_id);
    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(task_id)) != NULL)
        {
            central_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);
            // Release the TMOS message
            tmos_msg_deallocate(pMsg);
        }
        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & START_DEVICE_EVT)
    {
        // Start the Device
        GAPRole_CentralStartDevice(centralTaskId, &centralBondCB, &centralRoleCB);
        return (events ^ START_DEVICE_EVT);
    }

    if(events & START_SCAN_EVT)
    {
        GAPRole_CentralStartDiscovery(DEFAULT_DISCOVERY_MODE,
                                      DEFAULT_DISCOVERY_ACTIVE_SCAN,
                                      DEFAULT_DISCOVERY_WHITE_LIST);
        return (events ^ START_SCAN_EVT);
    }

    if(events & ESTABLISH_LINK_TIMEOUT_EVT)
    {
        GAPRole_TerminateLink(INVALID_CONNHANDLE);
        return (events ^ ESTABLISH_LINK_TIMEOUT_EVT);
    }

    // 连接0的任务处理
    if(task_id == centralConnList[CONNECT0_ITEM].taskID)
    {
        return connect_ProcessEvent(task_id, events);
    }
    // 连接1的任务处理
    else if(task_id == centralConnList[CONNECT1_ITEM].taskID)
    {
        return connect_ProcessEvent(task_id, events);
    }
    // 连接2的任务处理
    else if(task_id == centralConnList[CONNECT2_ITEM].taskID)
    {
        return connect_ProcessEvent(task_id, events);
    }
    else if(task_id == centralConnList[CONNECT3_ITEM].taskID)
    {
        return connect_ProcessEvent(task_id, events);
    }
    // Discard unknown events
    return 0;
}

/*********************************************************************
 * @fn      connect_ProcessEvent
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static uint16_t connect_ProcessEvent(uint8_t task_id, uint16_t events)
{
    uint8_t connItem;
    for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
    {
        if(centralConnList[connItem].taskID == task_id)
            break;
    }

    /* An event that maps to no connection item means the task table handed this
     * handler an id it does not own (a failed or colliding registration).
     * Indexing the list would then run out of bounds, so bail out instead. */
    if(connItem == CENTRAL_MAX_CONNECTION)
    {
        PRINT("central event for unknown task %x\n", task_id);
        return 0;
    }

    if(events & START_DISC_TIMEOUT_EVT)
    {
        /* Watchdog, ticks once per second while the link is up.  A GATT answer
         * that never came back used to leave the link silent for good: either
         * charHdl stayed 0 (nothing is ever forwarded) or procedureInProgress
         * stayed TRUE (every frame of the flood is skipped). */
        if(centralConnList[connItem].procedureInProgress)
        {
            if(++centralProcBusyTicks[connItem] >= 2)
            {
                centralProcBusyTicks[connItem] = 0;
                centralConnList[connItem].procedureInProgress = FALSE;
                PRINT("gatt procedure stuck %x, released\n",
                      centralConnList[connItem].connHandle);
            }
        }
        else
        {
            centralProcBusyTicks[connItem] = 0;
        }

        if(centralConnList[connItem].discState != BLE_DISC_STATE_IDLE)
        {
            if(centralDiscRetry[connItem] < CENTRAL_DISC_RETRY_MAX)
            {
                centralDiscRetry[connItem]++;
                PRINT("svc discovery retry %d for %x\n",
                      centralDiscRetry[connItem], centralConnList[connItem].connHandle);
                centralConnListStartDiscovery(connItem);
            }
        }

        tmos_start_task(centralConnList[connItem].taskID, START_DISC_TIMEOUT_EVT, DISC_TIMEOUT_TICKS);
        return (events ^ START_DISC_TIMEOUT_EVT);
    }

    if(events & START_SVC_DISCOVERY_EVT)
    {
        // start service discovery
        centralConnListStartDiscovery(connItem);
        return (events ^ START_SVC_DISCOVERY_EVT);
    }

    if(events & START_READ_OR_WRITE_EVT)
    {
        if(centralConnList[connItem].procedureInProgress == FALSE)
        {
            if(centralDoWrite)
            {
                // Do a write
                attWriteReq_t req;

                req.cmd = FALSE;
                req.sig = FALSE;
                req.handle = centralConnList[connItem].charHdl;
                req.len = 1;
                req.pValue = GATT_bm_alloc(centralConnList[connItem].connHandle, ATT_WRITE_REQ, req.len, NULL, 0);
                if(req.pValue != NULL)
                {
                    *req.pValue = centralCharVal;

                    if(GATT_WriteCharValue(centralConnList[connItem].connHandle, &req, centralTaskId) == SUCCESS)
                    {
                        centralConnList[connItem].procedureInProgress = TRUE;
                        centralDoWrite = !centralDoWrite;
                        tmos_start_task(centralConnList[connItem].taskID, START_READ_OR_WRITE_EVT, DEFAULT_READ_OR_WRITE_DELAY);
                    }
                    else
                    {
                        GATT_bm_free((gattMsg_t *)&req, ATT_WRITE_REQ);
                    }
                }
            }
            else
            {
                // Do a read
                attReadReq_t req;

                req.handle = centralConnList[connItem].charHdl;
                if(GATT_ReadCharValue(centralConnList[connItem].connHandle, &req, centralTaskId) == SUCCESS)
                {
                    centralConnList[connItem].procedureInProgress = TRUE;
                    centralDoWrite = !centralDoWrite;
                }
            }
        }
        return (events ^ START_READ_OR_WRITE_EVT);
    }

    if(events & START_WRITE_CCCD_EVT)
    {
        if(centralConnList[connItem].procedureInProgress == FALSE)
        {
            // Do a write
            attWriteReq_t req;

            req.cmd = FALSE;
            req.sig = FALSE;
            req.handle = centralConnList[connItem].cccdHdl;
            req.len = 2;
            req.pValue = GATT_bm_alloc(centralConnList[connItem].connHandle, ATT_WRITE_REQ, req.len, NULL, 0);
            if(req.pValue != NULL)
            {
                req.pValue[0] = 1;
                req.pValue[1] = 0;

                if(GATT_WriteCharValue(centralConnList[connItem].connHandle, &req, centralTaskId) == SUCCESS)
                {
                    centralConnList[connItem].procedureInProgress = TRUE;
                }
                else
                {
                    GATT_bm_free((gattMsg_t *)&req, ATT_WRITE_REQ);
                }
            }
        }
        return (events ^ START_WRITE_CCCD_EVT);
    }

    if(events & START_PARAM_UPDATE_EVT)
    {
        // start connect parameter update
        GAPRole_UpdateLink(centralConnList[connItem].connHandle,
                           DEFAULT_UPDATE_MIN_CONN_INTERVAL,
                           DEFAULT_UPDATE_MAX_CONN_INTERVAL,
                           DEFAULT_UPDATE_SLAVE_LATENCY,
                           DEFAULT_UPDATE_CONN_TIMEOUT);

        return (events ^ START_PARAM_UPDATE_EVT);
    }

    if(events & START_READ_RSSI_EVT)
    {
        GAPRole_ReadRssiCmd(centralConnList[connItem].connHandle);
        tmos_start_task(centralConnList[connItem].taskID, START_READ_RSSI_EVT, DEFAULT_RSSI_PERIOD);
        return (events ^ START_READ_RSSI_EVT);
    }
    // Discard unknown events
    return 0;
}
/*********************************************************************
 * @fn      central_ProcessTMOSMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void central_ProcessTMOSMsg(tmos_event_hdr_t *pMsg)
{
    switch(pMsg->event)
    {
        case GATT_MSG_EVENT:
            centralProcessGATTMsg((gattMsgEvent_t *)pMsg);
            break;
    }
}

/*********************************************************************
 * @fn      centralProcessGATTMsg
 *
 * @brief   Process GATT messages
 *
 * @return  none
 */
static void centralProcessGATTMsg(gattMsgEvent_t *pMsg)
{
    uint8_t connItem;
    for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
    {
        if(centralConnList[connItem].connHandle == pMsg->connHandle)
            break;
    }
    if(connItem == CENTRAL_MAX_CONNECTION)
    {
        return;
        // Should not go there
    }

    if(centralConnList[connItem].state != BLE_STATE_CONNECTED)
    {
        // In case a GATT message came after a connection has dropped,
        // ignore the message
        GATT_bm_free(&pMsg->msg, pMsg->method);
        return;
    }

    /* Every error response is worth a line: a rejected service discovery or a
     * rejected write shows up here and nowhere else. */
    if(pMsg->method == ATT_ERROR_RSP)
    {
        PRINT("att err %x req %x\n", pMsg->msg.errorRsp.errCode, pMsg->msg.errorRsp.reqOpcode);
    }

    if((pMsg->method == ATT_EXCHANGE_MTU_RSP) ||
       ((pMsg->method == ATT_ERROR_RSP) &&
        (pMsg->msg.errorRsp.reqOpcode == ATT_EXCHANGE_MTU_REQ)))
    {
        if(pMsg->method == ATT_ERROR_RSP)
        {
            uint8_t status = pMsg->msg.errorRsp.errCode;

            PRINT("Exchange MTU Error: %x\n", status);
        }
        centralConnList[connItem].procedureInProgress = FALSE;
    }

    if(pMsg->method == ATT_MTU_UPDATED_EVENT)
    {
        PRINT("MTU: %x\n", pMsg->msg.mtuEvt.MTU);
    }

    if((pMsg->method == ATT_READ_RSP) ||
       ((pMsg->method == ATT_ERROR_RSP) &&
        (pMsg->msg.errorRsp.reqOpcode == ATT_READ_REQ)))
    {
        if(pMsg->method == ATT_ERROR_RSP)
        {
            uint8_t status = pMsg->msg.errorRsp.errCode;

            PRINT("Read Error: %x\n", status);
        }
        else
        {
            // After a successful read, display the read value
            if(pMsg->msg.readRsp.len > 0)
            {
                PRINT("Read rsp: %x\n", *pMsg->msg.readRsp.pValue);
            }
        }
        centralConnList[connItem].procedureInProgress = FALSE;
    }
    else if((pMsg->method == ATT_WRITE_RSP) ||
            ((pMsg->method == ATT_ERROR_RSP) &&
             (pMsg->msg.errorRsp.reqOpcode == ATT_WRITE_REQ)))
    {
        if(pMsg->method == ATT_ERROR_RSP)
        {
            uint8_t status = pMsg->msg.errorRsp.errCode;

            PRINT("Write Error: %x\n", status);
        }
        else
        {
            // After a successful write, display the value that was written and increment value
            PRINT("Write sent: %x\n", centralCharVal);
        }

        centralConnList[connItem].procedureInProgress = FALSE;
    }
    else if(pMsg->method == ATT_HANDLE_VALUE_NOTI)
    {
        if(pMsg->msg.handleValueNoti.len > 0)
        {
            /* Frame from a peer board: hand it over to the mesh layer. */
            MESH_OnFrame(pMsg->msg.handleValueNoti.pValue,
                         pMsg->msg.handleValueNoti.len, 1, pMsg->connHandle);
        }
    }
    else if(centralConnList[connItem].discState != BLE_DISC_STATE_IDLE)
    {
        centralGATTDiscoveryEvent(connItem, pMsg);
    }
    GATT_bm_free(&pMsg->msg, pMsg->method);
}

/*********************************************************************
 * @fn      centralRssiCB
 *
 * @brief   RSSI callback.
 *
 * @param   connHandle - connection handle
 * @param   rssi - RSSI
 *
 * @return  none
 */
static void centralRssiCB(uint16_t connHandle, int8_t rssi)
{
    PRINT("RSSI -%d dB Conn - %x \n", -rssi, connHandle);
}

/*********************************************************************
 * @fn      centralHciMTUChangeCB
 *
 * @brief   MTU changed callback.
 *
 * @param   maxTxOctets - Max tx octets
 * @param   maxRxOctets - Max rx octets
 *
 * @return  none
 */
static void centralHciMTUChangeCB(uint16_t connHandle, uint16_t maxTxOctets, uint16_t maxRxOctets)
{
}

/*********************************************************************
 * @fn      centralEventCB
 *
 * @brief   Central event callback function.
 *
 * @param   pEvent - pointer to event structure
 *
 * @return  none
 */
static void centralEventCB(gapRoleEvent_t *pEvent)
{
    switch(pEvent->gap.opcode)
    {
        case GAP_DEVICE_INIT_DONE_EVENT:
        {
            uint8_t ownAddr[6];
            PRINT("Discovering...\n");
            GAPRole_GetParameter(GAPROLE_BD_ADDR, ownAddr);
            tmos_memcpy(centralOwnAddr, ownAddr, B_ADDR_LEN);
            MESH_RefreshLocalMac();
            GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, ownAddr);
            GAPRole_CentralStartDiscovery(DEFAULT_DISCOVERY_MODE,
                                          DEFAULT_DISCOVERY_ACTIVE_SCAN,
                                          DEFAULT_DISCOVERY_WHITE_LIST);
        }
        break;

        case GAP_DEVICE_INFO_EVENT:
        {
            /* Only mesh boards are interesting, and never ourselves or an
             * already connected peer. */
            if(centralIsMeshAdvert(pEvent->deviceInfo.pEvtData, pEvent->deviceInfo.dataLen) &&
               !tmos_memcmp(pEvent->deviceInfo.addr, centralOwnAddr, B_ADDR_LEN) &&
               !centralAddrConnected(pEvent->deviceInfo.addr))
            {
                centralAddDeviceInfo(pEvent->deviceInfo.addr, pEvent->deviceInfo.addrType);
            }
        }
        break;

        case GAP_DEVICE_DISCOVERY_EVENT:
        {
            uint8_t i;
            uint8_t connecting = FALSE;

            /* Connect to the first mesh board of this scan round.  Only the
             * board with the larger address initiates the link, so that two
             * boards never try to connect to each other at the same time. */
            for(i = 0; i < centralScanRes; i++)
            {
                if(centralAddrConnected(centralDevList[i].addr))
                {
                    continue;
                }
                if(!centralAddrGreater(centralOwnAddr, centralDevList[i].addr))
                {
                    continue;
                }

                tmos_memcpy(centralPendingAddr, centralDevList[i].addr, B_ADDR_LEN);
                centralPendingAddrType = centralDevList[i].addrType;

                PRINT("mesh board %02x%02x%02x%02x%02x%02x found, connecting...\n",
                      centralPendingAddr[0], centralPendingAddr[1], centralPendingAddr[2],
                      centralPendingAddr[3], centralPendingAddr[4], centralPendingAddr[5]);

                GAPRole_CentralEstablishLink(DEFAULT_LINK_HIGH_DUTY_CYCLE,
                                             DEFAULT_LINK_WHITE_LIST,
                                             centralPendingAddrType,
                                             centralPendingAddr);

                /* Start establish link timeout event */
                tmos_start_task(centralTaskId, ESTABLISH_LINK_TIMEOUT_EVT, ESTABLISH_LINK_TIMEOUT);
                centralScanBackoff = SCAN_BACKOFF_MIN;
                connecting = TRUE;
                break;
            }

            centralScanRes = 0;

            /* One status line per scan round, so the mesh state stays visible
             * instead of repeating a "not found" message. */
            PRINT("mesh links: peripheral %d / central %d\n",
                  Peripheral_LinkCount(), Central_LinkCount());

            if(!connecting)
            {
                /* Nothing to connect in this round: back off so the boards
                 * that already have their neighbours stop scanning all the
                 * time and leave the air to the established links. */
                centralScanBackoff = (centralScanBackoff >= (SCAN_BACKOFF_MAX / 2)) ?
                                     SCAN_BACKOFF_MAX : (uint16_t)(centralScanBackoff * 2);
                tmos_start_task(centralTaskId, START_SCAN_EVT, centralScanBackoff);
            }
        }
        break;

        case GAP_LINK_ESTABLISHED_EVENT:
        {
            tmos_stop_task(centralTaskId, ESTABLISH_LINK_TIMEOUT_EVT);
            if(pEvent->gap.hdr.status == SUCCESS)
            {
                uint8_t connItem;
                // 查询是否有空余连接条目
                for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
                {
                    if(centralConnList[connItem].connHandle == GAP_CONNHANDLE_INIT)
                        break;
                }
                if(connItem == CENTRAL_MAX_CONNECTION)
                {
                    GAPRole_TerminateLink(pEvent->linkCmpl.connectionHandle);
                    PRINT("Connection max...\n");
                }
                else
                {
                    centralConnList[connItem].state = BLE_STATE_CONNECTED;
                    centralConnList[connItem].connHandle = pEvent->linkCmpl.connectionHandle;
                    tmos_memcpy(centralConnList[connItem].peerAddr, centralPendingAddr, B_ADDR_LEN);

                    PRINT("Conn %x - Int %x \n", pEvent->linkCmpl.connectionHandle, pEvent->linkCmpl.connInterval);

                    centralDiscRetry[connItem] = 0;
                    centralProcBusyTicks[connItem] = 0;
                    centralConnList[connItem].procedureInProgress = TRUE;

                    /* Raise the MTU of the mesh link: the NFC frames carry up
                     * to 200 bytes of text, far more than the 23 byte default
                     * MTU allows.  The response clears procedureInProgress. */
                    {
                        attExchangeMTUReq_t mtuReq;

                        mtuReq.clientRxMTU = BLE_BUFF_MAX_LEN - 4;
                        if(GATT_ExchangeMTU(centralConnList[connItem].connHandle, &mtuReq, centralTaskId) != SUCCESS)
                        {
                            centralConnList[connItem].procedureInProgress = FALSE;
                        }
                    }

                    /* Initiate service discovery */
                    tmos_start_task(centralConnList[connItem].taskID, START_SVC_DISCOVERY_EVT, DEFAULT_SVC_DISCOVERY_DELAY);

                    /* Watchdog: retry a discovery that never answered and
                     * release a GATT procedure that got no response. */
                    tmos_start_task(centralConnList[connItem].taskID, START_DISC_TIMEOUT_EVT, DISC_TIMEOUT_TICKS);

                    /* Initiate connect parameter update */
                    tmos_start_task(centralConnList[connItem].taskID, START_PARAM_UPDATE_EVT, DEFAULT_PARAM_UPDATE_DELAY);

                    /* RSSI polling is left off: it only adds traffic and log
                     * noise while the mesh links are already established. */

                    PRINT("Connected...\n");

                    // See if need discover again
                    for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
                    {
                        if(centralConnList[connItem].connHandle == GAP_CONNHANDLE_INIT)
                            break;
                    }
                    if(connItem < CENTRAL_MAX_CONNECTION)
                    {
                        PRINT("Discovering...\n");
                        centralScanRes = 0;
                        GAPRole_CentralStartDiscovery(DEFAULT_DISCOVERY_MODE,
                                                      DEFAULT_DISCOVERY_ACTIVE_SCAN,
                                                      DEFAULT_DISCOVERY_WHITE_LIST);
                    }
                }
            }
            else
            {
                PRINT("Connect Failed...Reason:%X\n", pEvent->gap.hdr.status);
                PRINT("Discovering...\n");
                centralScanRes = 0;
                GAPRole_CentralStartDiscovery(DEFAULT_DISCOVERY_MODE,
                                              DEFAULT_DISCOVERY_ACTIVE_SCAN,
                                              DEFAULT_DISCOVERY_WHITE_LIST);
            }
        }
        break;

        case GAP_LINK_TERMINATED_EVENT:
        {
            uint8_t connItem;
            for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
            {
                if(centralConnList[connItem].connHandle == pEvent->linkTerminate.connectionHandle)
                    break;
            }
            if(connItem == CENTRAL_MAX_CONNECTION)
            {
                // Should not go there
                break;
            }
            PRINT("  %x  Disconnected...Reason:%x\n", centralConnList[connItem].connHandle, pEvent->linkTerminate.reason);
            centralConnList[connItem].state = BLE_STATE_IDLE;
            centralConnList[connItem].connHandle = GAP_CONNHANDLE_INIT;
            centralConnList[connItem].discState = BLE_DISC_STATE_IDLE;
            centralConnList[connItem].charHdl = 0;
            centralConnList[connItem].procedureInProgress = FALSE;
            centralScanRes = 0;

            tmos_stop_task(centralConnList[connItem].taskID, START_READ_RSSI_EVT);
            tmos_stop_task(centralConnList[connItem].taskID, START_DISC_TIMEOUT_EVT);
            tmos_stop_task(centralConnList[connItem].taskID, START_SVC_DISCOVERY_EVT);

            PRINT("Discovering...\n");
            GAPRole_CentralStartDiscovery(DEFAULT_DISCOVERY_MODE,
                                          DEFAULT_DISCOVERY_ACTIVE_SCAN,
                                          DEFAULT_DISCOVERY_WHITE_LIST);
        }
        break;

        case GAP_LINK_PARAM_UPDATE_EVENT:
        {
            PRINT("Update %x - Int %x \n", pEvent->linkUpdate.connectionHandle, pEvent->linkUpdate.connInterval);
        }
        break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      pairStateCB
 *
 * @brief   Pairing state callback.
 *
 * @return  none
 */
static void centralPairStateCB(uint16_t connHandle, uint8_t state, uint8_t status)
{
    if(state == GAPBOND_PAIRING_STATE_STARTED)
    {
        PRINT("Connection %04x - Pairing started:%d\n", connHandle, status);
    }
    else if(state == GAPBOND_PAIRING_STATE_COMPLETE)
    {
        if(status == SUCCESS)
        {
            PRINT("Connection %04x - Pairing success\n", connHandle);
        }
        else
        {
            PRINT("Connection %04x - Pairing fail\n", connHandle);
        }
    }
    else if(state == GAPBOND_PAIRING_STATE_BONDED)
    {
        if(status == SUCCESS)
        {
            PRINT("Connection %04x - Bonding success\n", connHandle);
        }
    }
    else if(state == GAPBOND_PAIRING_STATE_BOND_SAVED)
    {
        if(status == SUCCESS)
        {
            PRINT("Connection %04x - Bond save success\n", connHandle);
        }
        else
        {
            PRINT("Connection %04x - Bond save failed: %d\n", connHandle, status);
        }
    }
}

/*********************************************************************
 * @fn      centralPasscodeCB
 *
 * @brief   Passcode callback.
 *
 * @return  none
 */
static void centralPasscodeCB(uint8_t *deviceAddr, uint16_t connectionHandle,
                              uint8_t uiInputs, uint8_t uiOutputs)
{
    uint32_t passcode;

    // Create random passcode
    passcode = tmos_rand();
    passcode %= 1000000;
    // Display passcode to user
    if(uiOutputs != 0)
    {
        PRINT("Passcode:%06d\n", (int)passcode);
    }
    // Send passcode response
    GAPBondMgr_PasscodeRsp(connectionHandle, SUCCESS, passcode);
}

/*********************************************************************
 * @fn      centralConnListStartDiscovery
 *
 * @brief   Start connection 0 service discovery.
 *
 * @return  none
 */
static void centralConnListStartDiscovery(uint8_t connItem)
{
    uint8_t   uuid[ATT_BT_UUID_SIZE] = {LO_UINT16(SIMPLEPROFILE_SERV_UUID),
                                      HI_UINT16(SIMPLEPROFILE_SERV_UUID)};
    bStatus_t st;

    /* A discovery event can still be pending when the link drops.  Asking for
     * the service on a dead handle only wastes a GATT request and leaves
     * discState at SVC, which makes the watchdog retry it forever. */
    if(centralConnList[connItem].connHandle == GAP_CONNHANDLE_INIT)
    {
        centralConnList[connItem].discState = BLE_DISC_STATE_IDLE;
        return;
    }

    // Initialize cached handles
    centralConnList[connItem].svcStartHdl = centralConnList[connItem].svcEndHdl = centralConnList[connItem].charHdl = 0;

    centralConnList[connItem].discState = BLE_DISC_STATE_SVC;

    // Discovery simple BLE service
    st = GATT_DiscPrimaryServiceByUUID(centralConnList[connItem].connHandle,
                                       uuid,
                                       ATT_BT_UUID_SIZE,
                                       centralTaskId);
    /* A request that never went out is retried by the watchdog. */
    PRINT("svc discovery start %x st %x\n", centralConnList[connItem].connHandle, st);
}

/*********************************************************************
 * @fn      centralGATTDiscoveryEvent
 *
 * @brief   Process GATT discovery event
 *
 * @return  none
 */
static void centralGATTDiscoveryEvent(uint8_t connItem, gattMsgEvent_t *pMsg)
{
    attReadByTypeReq_t req;

    if(1)
    {
        if(centralConnList[connItem].discState == BLE_DISC_STATE_SVC)
        {
            // Service found, store handles
            if(pMsg->method == ATT_FIND_BY_TYPE_VALUE_RSP &&
               pMsg->msg.findByTypeValueRsp.numInfo > 0)
            {
                centralConnList[connItem].svcStartHdl = ATT_ATTR_HANDLE(pMsg->msg.findByTypeValueRsp.pHandlesInfo, 0);
                centralConnList[connItem].svcEndHdl = ATT_GRP_END_HANDLE(pMsg->msg.findByTypeValueRsp.pHandlesInfo, 0);

                // Display Profile Service handle range
                PRINT("Found Profile Service handle : %x ~ %x \n", centralConnList[connItem].svcStartHdl, centralConnList[connItem].svcEndHdl);
            }
            // If procedure complete
            if((pMsg->method == ATT_FIND_BY_TYPE_VALUE_RSP &&
                pMsg->hdr.status == bleProcedureComplete) ||
               (pMsg->method == ATT_ERROR_RSP))
            {
                if(centralConnList[connItem].svcStartHdl != 0)
                {
                    // Discover characteristic
                    centralConnList[connItem].discState = BLE_DISC_STATE_CHAR;
                    req.startHandle = centralConnList[connItem].svcStartHdl;
                    req.endHandle = centralConnList[connItem].svcEndHdl;
                    req.type.len = ATT_BT_UUID_SIZE;
                    req.type.uuid[0] = LO_UINT16(SIMPLEPROFILE_CHAR1_UUID);
                    req.type.uuid[1] = HI_UINT16(SIMPLEPROFILE_CHAR1_UUID);

                    GATT_ReadUsingCharUUID(centralConnList[connItem].connHandle, &req, centralTaskId);
                }
            }
        }
        else if(centralConnList[connItem].discState == BLE_DISC_STATE_CHAR)
        {
            // Characteristic found, store handle
            if(pMsg->method == ATT_READ_BY_TYPE_RSP &&
               pMsg->msg.readByTypeRsp.numPairs > 0)
            {
                centralConnList[connItem].charHdl = BUILD_UINT16(pMsg->msg.readByTypeRsp.pDataList[0],
                                                                 pMsg->msg.readByTypeRsp.pDataList[1]);
                centralConnList[connItem].procedureInProgress = FALSE;

                // Display Characteristic 1 handle
                PRINT("Found Characteristic 1 handle : %x \n", centralConnList[connItem].charHdl); //23
            }

            if((pMsg->method == ATT_READ_BY_TYPE_RSP &&
                pMsg->hdr.status == bleProcedureComplete) ||
               (pMsg->method == ATT_ERROR_RSP))
            {
                // Discover characteristic
                centralConnList[connItem].discState = BLE_DISC_STATE_CCCD;
                req.startHandle = centralConnList[connItem].svcStartHdl;
                req.endHandle = centralConnList[connItem].svcEndHdl;
                req.type.len = ATT_BT_UUID_SIZE;
                req.type.uuid[0] = LO_UINT16(GATT_CLIENT_CHAR_CFG_UUID);
                req.type.uuid[1] = HI_UINT16(GATT_CLIENT_CHAR_CFG_UUID);

                GATT_ReadUsingCharUUID(centralConnList[connItem].connHandle, &req, centralTaskId);
            }
        }
        else if(centralConnList[connItem].discState == BLE_DISC_STATE_CCCD)
        {
            // Characteristic found, store handle
            if(pMsg->method == ATT_READ_BY_TYPE_RSP &&
               pMsg->msg.readByTypeRsp.numPairs > 0)
            {
                centralConnList[connItem].cccdHdl = BUILD_UINT16(pMsg->msg.readByTypeRsp.pDataList[0],
                                                                 pMsg->msg.readByTypeRsp.pDataList[1]);
                centralConnList[connItem].procedureInProgress = FALSE;

                // Start do write CCCD
                tmos_start_task(centralConnList[connItem].taskID, START_WRITE_CCCD_EVT, DEFAULT_WRITE_CCCD_DELAY);

                // Display Characteristic 1 handle
                PRINT("Found client characteristic configuration handle : %x \n", centralConnList[connItem].cccdHdl); //2d
            }
            centralConnList[connItem].discState = BLE_DISC_STATE_IDLE;
        }
    }
}

/*********************************************************************
 * @fn      centralAddDeviceInfo
 *
 * @brief   Add a device to the device discovery result list
 *
 * @return  none
 */
static void centralAddDeviceInfo(uint8_t *pAddr, uint8_t addrType)
{
    uint8_t i;

    // If result count not at max
    if(centralScanRes < DEFAULT_MAX_SCAN_RES)
    {
        // Check if device is already in scan results
        for(i = 0; i < centralScanRes; i++)
        {
            if(tmos_memcmp(pAddr, centralDevList[i].addr, B_ADDR_LEN))
            {
                return;
            }
        }
        // Add addr to scan result list
        tmos_memcpy(centralDevList[centralScanRes].addr, pAddr, B_ADDR_LEN);
        centralDevList[centralScanRes].addrType = addrType;
        // Increment scan result count
        centralScanRes++;
        // Display device addr
        PRINT("Device %d - Addr %x %x %x %x %x %x \n", centralScanRes,
              centralDevList[centralScanRes - 1].addr[0],
              centralDevList[centralScanRes - 1].addr[1],
              centralDevList[centralScanRes - 1].addr[2],
              centralDevList[centralScanRes - 1].addr[3],
              centralDevList[centralScanRes - 1].addr[4],
              centralDevList[centralScanRes - 1].addr[5]);
    }
}

/*********************************************************************
 * @fn      centralAddrCmp
 *
 * @brief   none
 *
 * @return  none
 */
/*********************************************************************
 * @fn      centralIsMeshAdvert
 *
 * @brief   Check whether the advertising data belongs to one of our mesh
 *          boards: either the local name "MeXsmarEC" or the 16 bit service
 *          UUID 0xFFE0.
 *
 * @return  TRUE if it is a mesh board
 */
static uint8_t centralIsMeshAdvert(uint8_t *pData, uint8_t len)
{
    uint8_t i = 0;
    uint8_t name[] = "MeXsmarEC";

    while(i + 1 < len)
    {
        uint8_t fieldLen = pData[i];
        uint8_t type;

        if(fieldLen == 0)
        {
            break;
        }
        type = pData[i + 1];

        if((type == GAP_ADTYPE_LOCAL_NAME_COMPLETE || type == GAP_ADTYPE_LOCAL_NAME_SHORT) &&
           (fieldLen - 1) == (sizeof(name) - 1))
        {
            if(tmos_memcmp(&pData[i + 2], name, sizeof(name) - 1))
            {
                return TRUE;
            }
        }
        else if((type == GAP_ADTYPE_16BIT_MORE || type == GAP_ADTYPE_16BIT_COMPLETE) && fieldLen >= 3)
        {
            uint8_t k;
            for(k = 0; k + 1 < (fieldLen - 1); k += 2)
            {
                uint16_t uuid = BUILD_UINT16(pData[i + 2 + k], pData[i + 3 + k]);
                if(uuid == SIMPLEPROFILE_SERV_UUID)
                {
                    return TRUE;
                }
            }
        }
        i += fieldLen + 1;
    }
    return FALSE;
}

/*********************************************************************
 * @fn      centralAddrConnected
 *
 * @brief   Check whether the address is already connected.
 *
 * @return  TRUE if already connected
 */
static uint8_t centralAddrConnected(uint8_t *addr)
{
    uint8_t i;
    for(i = 0; i < CENTRAL_MAX_CONNECTION; i++)
    {
        if(centralConnList[i].connHandle != GAP_CONNHANDLE_INIT &&
           tmos_memcmp(centralConnList[i].peerAddr, addr, B_ADDR_LEN))
        {
            return TRUE;
        }
    }
    return FALSE;
}

/*********************************************************************
 * @fn      centralAddrGreater
 *
 * @brief   Compare two BLE addresses byte by byte.
 *
 * @return  TRUE if a is greater than b
 */
static uint8_t centralAddrGreater(const uint8_t *a, const uint8_t *b)
{
    uint8_t i;
    for(i = 0; i < B_ADDR_LEN; i++)
    {
        if(a[i] > b[i])
        {
            return TRUE;
        }
        if(a[i] < b[i])
        {
            return FALSE;
        }
    }
    return FALSE;
}

/*********************************************************************
 * @fn      Central_SendExcept
 *
 * @brief   Write a raw mesh frame to every connected peer except one.
 *
 * @param   skipConn - connection handle to skip, 0xFFFF to write to all
 * @param   data - frame data
 * @param   len - frame length
 *
 * @return  none
 */
void Central_SendExcept(uint16_t skipConn, const uint8_t *data, uint16_t len)
{
    uint8_t connItem;

    for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
    {
        attWriteReq_t req;

        if(centralConnList[connItem].connHandle == GAP_CONNHANDLE_INIT)
        {
            continue;
        }
        if(centralConnList[connItem].connHandle == skipConn)
        {
            continue;
        }
        if(centralConnList[connItem].charHdl == 0)
        {
            PRINT("central skip %x: no char handle\n", centralConnList[connItem].connHandle);
            continue;   /* service discovery not finished yet */
        }
        if(centralConnList[connItem].procedureInProgress)
        {
            continue;   /* only one GATT procedure at a time */
        }

        req.cmd = FALSE;
        req.sig = FALSE;
        req.handle = centralConnList[connItem].charHdl;
        req.len = len;
        req.pValue = GATT_bm_alloc(centralConnList[connItem].connHandle, ATT_WRITE_REQ, req.len, NULL, 0);
        if(req.pValue == NULL)
        {
            continue;
        }
        tmos_memcpy(req.pValue, data, len);

        if(GATT_WriteCharValue(centralConnList[connItem].connHandle, &req, centralTaskId) == SUCCESS)
        {
            centralConnList[connItem].procedureInProgress = TRUE;
            PRINT("central write %x len %d\n", centralConnList[connItem].connHandle, len);
        }
        else
        {
            GATT_bm_free((gattMsg_t *)&req, ATT_WRITE_REQ);
        }
    }
}

/*********************************************************************
 * @fn      Central_LinkCount
 *
 * @brief   Number of currently connected peers.
 *
 * @return  connection count
 */
uint8_t Central_LinkCount(void)
{
    uint8_t connItem;
    uint8_t count = 0;

    for(connItem = 0; connItem < CENTRAL_MAX_CONNECTION; connItem++)
    {
        if(centralConnList[connItem].connHandle != GAP_CONNHANDLE_INIT)
        {
            count++;
        }
    }
    return count;
}

/************************ endfile @ central **************************/
