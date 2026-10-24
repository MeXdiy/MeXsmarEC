/********************************** (C) COPYRIGHT *******************************
 * File Name          : multiPeripheral.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2022/03/18
 * Description        : 外设从机多连接应用程序，初始化广播连接参数，然后广播，连接主机后，
 *                      请求更新连接参数，通过自定义服务传输数据
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "CONFIG.h"
#include "devinfoservice.h"
#include "gattprofile.h"
#include "ble_peripheral.h"
#include "app_mesh.h"
#include "app_ind.h"

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * CONSTANTS
 */

// How often to perform read rssi event
#define SBP_READ_RSSI_EVT_PERIOD             3200

// How often the chunked notification sender runs, in ms.  A mesh frame (up to
// 220 bytes) does not fit into a small ATT MTU (23 -> 20 bytes of payload),
// which happens when the central never raises the MTU.  Such a frame is split
// into chunks and pushed out one connection event at a time by this event.
#define SBP_NOTIFY_EVT_PERIOD_MS             10

// Chunked notification header: magic, sequence number, chunk count
#define SBP_NOTIFY_CHUNK_MAGIC               0x5A
#define SBP_NOTIFY_CHUNK_HEAD_LEN            3

// How many frames may wait for the chunked sender
#define SBP_NOTIFY_ITEM_MAX                  4

// Largest frame the chunked sender can hold
#define SBP_NOTIFY_BUF_LEN                   224

// Parameter update delay
#define SBP_PARAM_UPDATE_DELAY               6400

// PHY update delay
#define SBP_PHY_UPDATE_DELAY                 3200

// What is the advertising interval when device is discoverable (units of 625us, 80=50ms)
#define DEFAULT_ADVERTISING_INTERVAL         80

// Limited discoverable mode advertises for 30.72s, and then stops
// General discoverable mode advertises indefinitely
#define DEFAULT_DISCOVERABLE_MODE            GAP_ADTYPE_FLAGS_GENERAL

// Minimum connection interval (units of 1.25ms, 20=25ms)
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL    20

// Maximum connection interval (units of 1.25ms, 100=125ms)
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL    100

// Slave latency to use parameter update
#define DEFAULT_DESIRED_SLAVE_LATENCY        0

// Supervision timeout value (units of 10ms, 100=1s)
#define DEFAULT_DESIRED_CONN_TIMEOUT         100

// Company Identifier: WCH
#define WCH_COMPANY_ID                       0x07D7

/*********************************************************************
 * TYPEDEFS
 */

// One frame waiting to be pushed out in chunks
typedef struct
{
    uint16_t connHandle;
    uint16_t len;                        // total frame length
    uint16_t offset;                     // bytes already sent
    uint8_t  seq;                        // index of the next chunk
    uint8_t  total;                      // total number of chunks
    uint8_t  active;                     // 1 while this entry is in flight
    uint8_t  buf[SBP_NOTIFY_BUF_LEN];    // copy of the frame
} peripheralNotifyItem_t;

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
static uint8_t Peripheral_TaskID = INVALID_TASK_ID; // Task ID for internal task/event processing

// Frames that did not fit into one notification, waiting for the chunked sender
static peripheralNotifyItem_t peripheralNotifyList[SBP_NOTIFY_ITEM_MAX];

// GAP - SCAN RSP data (max size = 31 bytes)
static uint8_t scanRspData[] = {

    // complete name
    0x09, // length of this data
    GAP_ADTYPE_LOCAL_NAME_COMPLETE,
    'M',
    'e',
    'X',
    's',
    'm',
    'r',
    'E',
    'C',
    // connection interval range
    0x05, // length of this data
    GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
    LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL), // 100ms
    HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL),
    LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL), // 1s
    HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL),

    // Tx power level
    0x02, // length of this data
    GAP_ADTYPE_POWER_LEVEL,
    0 // 0dBm
};

// GAP - Advertisement data (max size = 31 bytes, though this is
// best kept short to conserve power while advertising)
static uint8_t advertData[] = {

    // Flags; this sets the device to use limited discoverable
    // mode (advertises for 30 seconds at a time) instead of general
    // discoverable mode (advertises indefinitely)
    0x02, // length of this data
    GAP_ADTYPE_FLAGS,
    DEFAULT_DISCOVERABLE_MODE | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

    // service UUID, to notify central devices what services are included
    // in this peripheral
    0x03,                  // length of this data
    GAP_ADTYPE_16BIT_MORE, // some of the UUID's, but not all
    LO_UINT16(SIMPLEPROFILE_SERV_UUID),
    HI_UINT16(SIMPLEPROFILE_SERV_UUID)};

// GAP GATT Attributes
static uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = "MeXsmarEC";

// Connection item list
static peripheralConnItem_t peripheralConnList[PERIPHERAL_MAX_CONNECTION];

/* Largest ATT payload of one link; kept per connection because the PC link and
 * the mesh link can negotiate different MTUs. */
static uint16_t peripheralMtuOf(uint16_t connHandle)
{
    uint8_t connItem;

    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        if(peripheralConnList[connItem].connHandle == connHandle)
        {
            return peripheralConnList[connItem].mtu;
        }
    }
    return ATT_MTU_SIZE;
}

/* Check whether a connection handle still belongs to a live link.  A queued
 * notification whose link is gone must not be retried forever. */
uint8_t Peripheral_LinkUp(uint16_t connHandle)
{
    uint8_t connItem;

    if(connHandle == GAP_CONNHANDLE_INIT)
    {
        return 0;
    }
    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        if(peripheralConnList[connItem].connHandle == connHandle)
        {
            return 1;
        }
    }
    return 0;
}

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void Peripheral_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
/*********************************************************************
 * @fn      Peripheral_SetAdvertise
 *
 * @brief   Turn BLE advertising on or off.  Calls that would not change the
 *          current state are ignored, so it is safe to call at the indicator
 *          tick rate.
 *
 * @param   enable - 1: advertise, 0: stop advertising
 *
 * @return  none
 */
void Peripheral_SetAdvertise(uint8_t enable)
{
    static uint8_t advertiseOn = 1;
    uint8_t        value;

    enable = enable ? 1 : 0;
    if(enable == advertiseOn)
    {
        return;
    }
    advertiseOn = enable;

    value = enable ? TRUE : FALSE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &value);
}

/*********************************************************************
 * @fn      IND_AlarmPhaseCB
 *
 * @brief   Status indicator reports its "upper computer lost" red phase.
 *          This board advertises only while that phase is on, unless the
 *          upper computer is attached to this board, in which case
 *          advertising stays on.
 *
 * @param   alarmOn - 1 while the red phase is on
 *
 * @return  none
 */
void IND_AlarmPhaseCB(uint8_t alarmOn)
{
    Peripheral_SetAdvertise(MESH_PcIsUplink() ? 1 : alarmOn);
}

static void peripheralStateNotificationCB(gapRole_States_t newState, gapRoleEvent_t *pEvent);
static void simpleProfileChangeCB(uint16_t connHandle, uint8_t paramID, uint8_t *pValue, uint16_t len);
static void peripheralParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                    uint16_t connSlaveLatency, uint16_t connTimeout);
static void peripheralInitConnItem(uint8_t task_id, peripheralConnItem_t *peripheralConnList);
static void peripheralRssiCB(uint16_t connHandle, int8_t rssi);
static void peripheralChar4Notify(uint16_t connHandle, uint8_t *pValue, uint16_t len);
static void peripheralNotifySendChunk(peripheralNotifyItem_t *pItem);
static void peripheralNotifyEnqueue(uint16_t connHandle, const uint8_t *pValue, uint16_t len);

static uint16_t connect0_ProcessEvent(uint8_t task_id, uint16_t events);
static uint16_t connect1_ProcessEvent(uint8_t task_id, uint16_t events);
static uint16_t connect2_ProcessEvent(uint8_t task_id, uint16_t events);

/*********************************************************************
 * PROFILE CALLBACKS
 */

// GAP Role Callbacks
static gapRolesCBs_t Peripheral_PeripheralCBs = {

    peripheralStateNotificationCB, // Profile State Change Callbacks
    peripheralRssiCB,              // When a valid RSSI is read from controller (not used by application)
    peripheralParamUpdateCB};

// Broadcast Callbacks
static gapRolesBroadcasterCBs_t Broadcaster_BroadcasterCBs = {
    NULL, // Not used in peripheral role
    NULL  // Receive scan request callback
};

// GAP Bond Manager Callbacks
static gapBondCBs_t Peripheral_BondMgrCBs = {
    NULL, // Passcode callback (not used by application)
    NULL  // Pairing / Bonding state Callback (not used by application)
};

// Simple GATT Profile Callbacks
static simpleProfileCBs_t Peripheral_SimpleProfileCBs = {

    simpleProfileChangeCB // Characteristic value change callback
};
/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      Peripheral_Init
 *
 * @brief   Initialization function for the Peripheral App Task.
 *          This is called during initialization and should contain
 *          any application specific initialization (ie. hardware
 *          initialization/setup, table initialization, power up
 *          notification ... ).
 *
 * @param   task_id - the ID assigned by TMOS.  This ID should be
 *                    used to send messages and set timers.
 *
 * @return  none
 */
void Peripheral_Init(void)
{
    Peripheral_TaskID = TMOS_ProcessEventRegister(Peripheral_ProcessEvent);
    PRINT("task id peri %x\n", Peripheral_TaskID);

    // Setup the GAP Peripheral Role Profile
    {
        uint8_t  initial_advertising_enable = TRUE;
        uint16_t desired_min_interval = DEFAULT_DESIRED_MIN_CONN_INTERVAL;
        uint16_t desired_max_interval = DEFAULT_DESIRED_MAX_CONN_INTERVAL;

        // Set the GAP Role Parameters
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
        GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, sizeof(scanRspData), scanRspData);
        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);
        GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL, sizeof(uint16_t), &desired_min_interval);
        GAPRole_SetParameter(GAPROLE_MAX_CONN_INTERVAL, sizeof(uint16_t), &desired_max_interval);
    }

    {
        uint16_t advInt = DEFAULT_ADVERTISING_INTERVAL;

        // Set advertising interval
        GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, advInt);
        GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, advInt);

        // Enable scan req notify
        GAP_SetParamValue(TGAP_ADV_SCAN_REQ_NOTIFY, ENABLE);
    }

    // Setup the GAP Bond Manager
    {
        uint32_t passkey = 0; // passkey "000000"
        uint8_t  pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
        uint8_t  mitm = TRUE;
        uint8_t  bonding = TRUE;
        uint8_t  ioCap = GAPBOND_IO_CAP_DISPLAY_ONLY;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    // Initialize GATT attributes
    GGS_AddService(GATT_ALL_SERVICES);           // GAP
    GATTServApp_AddService(GATT_ALL_SERVICES);   // GATT attributes
    DevInfo_AddService();                        // Device Information Service
    SimpleProfile_AddService(GATT_ALL_SERVICES); // Simple GATT Profile

    // Set the GAP Characteristics
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, attDeviceName);

    // Setup the SimpleProfile Characteristic Values
    {
        uint8_t charValue1[SIMPLEPROFILE_CHAR1_LEN] = {1};
        uint8_t charValue2[SIMPLEPROFILE_CHAR2_LEN] = {2};
        uint8_t charValue3[SIMPLEPROFILE_CHAR3_LEN] = {3};
        uint8_t charValue4[SIMPLEPROFILE_CHAR4_LEN] = {4};
        uint8_t charValue5[SIMPLEPROFILE_CHAR5_LEN] = {1, 2, 3, 4, 5};

        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR1, SIMPLEPROFILE_CHAR1_LEN, charValue1);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR2, SIMPLEPROFILE_CHAR2_LEN, charValue2);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR3, SIMPLEPROFILE_CHAR3_LEN, charValue3);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR4, SIMPLEPROFILE_CHAR4_LEN, charValue4);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR5, SIMPLEPROFILE_CHAR5_LEN, charValue5);
    }

    // Init Connection Item
    peripheralInitConnItem(Peripheral_TaskID, peripheralConnList);

    // Register callback with SimpleGATTprofile
    SimpleProfile_RegisterAppCBs(&Peripheral_SimpleProfileCBs);

    // Register receive scan request callback
    GAPRole_BroadcasterSetCB(&Broadcaster_BroadcasterCBs);

    // Setup a delayed profile startup
    tmos_set_event(Peripheral_TaskID, SBP_START_DEVICE_EVT);
}

/*********************************************************************
 * @fn      peripheralInitConnItem
 *
 * @brief   Init Connection Item
 *
 * @param   peripheralConnList -
 *
 * @return  NULL
 */
static void peripheralInitConnItem(uint8_t task_id, peripheralConnItem_t *peripheralConnList)
{
    uint8_t connItem;
    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        peripheralConnList[connItem].taskID = TMOS_ProcessEventRegister(Peripheral_ProcessEvent);
        PRINT("task id peri conn %d %x\n", connItem, peripheralConnList[connItem].taskID);
        peripheralConnList[connItem].connHandle = GAP_CONNHANDLE_INIT;
        peripheralConnList[connItem].connInterval = 0;
        peripheralConnList[connItem].connSlaveLatency = 0;
        peripheralConnList[connItem].connTimeout = 0;
        peripheralConnList[connItem].mtu = ATT_MTU_SIZE;
    }
}

/*********************************************************************
 * @fn      Peripheral_ProcessEvent
 *
 * @brief   Peripheral Application Task event processor.  This function
 *          is called to process all events for the task.  Events
 *          include timers, messages and any other user defined events.
 *
 * @param   task_id - The TMOS assigned task ID.
 * @param   events - events to process.  This is a bit map and can
 *                   contain more than one event.
 *
 * @return  events not processed
 */
uint16_t Peripheral_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  VOID task_id; // TMOS required parameter that isn't used in this function

    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(task_id)) != NULL)
        {
            Peripheral_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);
            // Release the TMOS message
            tmos_msg_deallocate(pMsg);
        }
        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & SBP_START_DEVICE_EVT)
    {
        // Start the Device
        GAPRole_PeripheralStartDevice(Peripheral_TaskID, &Peripheral_BondMgrCBs, &Peripheral_PeripheralCBs);
        return (events ^ SBP_START_DEVICE_EVT);
    }

    if(events & SBP_NOTIFY_EVT)
    {
        uint8_t i;
        uint8_t busy = 0;

        /* Push out one more chunk of every frame that did not fit into a single
         * notification.  A connection event carries only one packet, so the
         * chunks have to be spread over several events. */
        for(i = 0; i < SBP_NOTIFY_ITEM_MAX; i++)
        {
            if(peripheralNotifyList[i].active)
            {
                /* Drop a queued frame whose link is gone: retrying it forever
                 * keeps a queue slot and a GATT buffer busy. */
                if(!Peripheral_LinkUp(peripheralNotifyList[i].connHandle))
                {
                    peripheralNotifyList[i].active = 0;
                    continue;
                }
                peripheralNotifySendChunk(&peripheralNotifyList[i]);
                busy = 1;
            }
        }
        if(busy)
        {
            tmos_start_task(Peripheral_TaskID, SBP_NOTIFY_EVT, MS1_TO_SYSTEM_TIME(SBP_NOTIFY_EVT_PERIOD_MS));
        }
        return (events ^ SBP_NOTIFY_EVT);
    }

    // 连接0的任务处理
    if(task_id == peripheralConnList[CONNECT0_ITEM].taskID)
    {
        return connect0_ProcessEvent(task_id, events);
    }
    // 连接1的任务处理
    else if(task_id == peripheralConnList[CONNECT1_ITEM].taskID)
    {
        return connect1_ProcessEvent(task_id, events);
    }
    // 连接2的任务处理
    else if(task_id == peripheralConnList[CONNECT2_ITEM].taskID)
    {
        return connect2_ProcessEvent(task_id, events);
    }

    // Discard unknown events
    return 0;
}

/*********************************************************************
 * @fn      connect0_ProcessEvent
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static uint16_t connect0_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  PRINT("peripheral task id 1 : %#x\n",peripheralConnList[CONNECT0_ITEM].taskID);

    if(events & SBP_PARAM_UPDATE_EVT)
    {
        // Send connect param update request
        GAPRole_PeripheralConnParamUpdateReq(peripheralConnList[CONNECT0_ITEM].connHandle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             peripheralConnList[CONNECT0_ITEM].taskID);

        return (events ^ SBP_PARAM_UPDATE_EVT);
    }

    if(events & SBP_PHY_UPDATE_EVT)
    {
        // start phy update
        PRINT("PHY Update %x...\n", GAPRole_UpdatePHY(peripheralConnList[CONNECT0_ITEM].connHandle, 
                    0, GAP_PHY_BIT_LE_2M, GAP_PHY_BIT_LE_2M, 0));

        return (events ^ SBP_PHY_UPDATE_EVT);
    }

    if(events & SBP_READ_RSSI_EVT)
    {
        GAPRole_ReadRssiCmd(peripheralConnList[CONNECT0_ITEM].connHandle);
        tmos_start_task(peripheralConnList[CONNECT0_ITEM].taskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);
        return (events ^ SBP_READ_RSSI_EVT);
    }

    // Discard unknown events
    return 0;
}

/*********************************************************************
 * @fn      connect1_ProcessEvent
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static uint16_t connect1_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  PRINT("peripheral task id 2 : %#x\n",peripheralConnList[CONNECT1_ITEM].taskID);
    if(events & SBP_PARAM_UPDATE_EVT)
    {
        // Send connect param update request
        GAPRole_PeripheralConnParamUpdateReq(peripheralConnList[CONNECT1_ITEM].connHandle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             peripheralConnList[CONNECT1_ITEM].taskID);

        return (events ^ SBP_PARAM_UPDATE_EVT);
    }

    if(events & SBP_PHY_UPDATE_EVT)
    {
        // start phy update
        PRINT("PHY Update %x...\n", GAPRole_UpdatePHY(peripheralConnList[CONNECT1_ITEM].connHandle, 
                    0, GAP_PHY_BIT_LE_2M, GAP_PHY_BIT_LE_2M, 0));

        return (events ^ SBP_PHY_UPDATE_EVT);
    }

    if(events & SBP_READ_RSSI_EVT)
    {
        GAPRole_ReadRssiCmd(peripheralConnList[CONNECT1_ITEM].connHandle);
        tmos_start_task(peripheralConnList[CONNECT1_ITEM].taskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);
        return (events ^ SBP_READ_RSSI_EVT);
    }
    // Discard unknown events
    return 0;
}

/*********************************************************************
 * @fn      connect2_ProcessEvent
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static uint16_t connect2_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  PRINT("peripheral task id 3 : %#x\n",peripheralConnList[CONNECT2_ITEM].taskID);
    if(events & SBP_PARAM_UPDATE_EVT)
    {
        // Send connect param update request
        GAPRole_PeripheralConnParamUpdateReq(peripheralConnList[CONNECT2_ITEM].connHandle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             peripheralConnList[CONNECT2_ITEM].taskID);

        return (events ^ SBP_PARAM_UPDATE_EVT);
    }

    if(events & SBP_PHY_UPDATE_EVT)
    {
        // start phy update
        PRINT("PHY Update %x...\n", GAPRole_UpdatePHY(peripheralConnList[CONNECT2_ITEM].connHandle, 
                    0, GAP_PHY_BIT_LE_2M, GAP_PHY_BIT_LE_2M, 0));

        return (events ^ SBP_PHY_UPDATE_EVT);
    }

    if(events & SBP_READ_RSSI_EVT)
    {
        GAPRole_ReadRssiCmd(peripheralConnList[CONNECT2_ITEM].connHandle);
        tmos_start_task(peripheralConnList[CONNECT2_ITEM].taskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);
        return (events ^ SBP_READ_RSSI_EVT);
    }
    // Discard unknown events
    return 0;
}

/*********************************************************************
 * @fn      Peripheral_ProcessGAPMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void Peripheral_ProcessGAPMsg(gapRoleEvent_t *pEvent)
{
    switch(pEvent->gap.opcode)
    {
        case GAP_SCAN_REQUEST_EVENT:
        {
            // PRINT("Receive scan req from %x %x %x %x %x %x  ..\n", pEvent->scanReqEvt.scannerAddr[0],
            //       pEvent->scanReqEvt.scannerAddr[1], pEvent->scanReqEvt.scannerAddr[2], pEvent->scanReqEvt.scannerAddr[3],
            //       pEvent->scanReqEvt.scannerAddr[4], pEvent->scanReqEvt.scannerAddr[5]);
            break;
        }

        case GAP_PHY_UPDATE_EVENT:
        {
            PRINT("Phy update Rx:%x Tx:%x ..\n", pEvent->linkPhyUpdate.connRxPHYS, pEvent->linkPhyUpdate.connTxPHYS);
            break;
        }

        default:
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_ProcessTMOSMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void Peripheral_ProcessTMOSMsg(tmos_event_hdr_t *pMsg)
{
    switch(pMsg->event)
    {
        case GAP_MSG_EVENT:
        {
            Peripheral_ProcessGAPMsg((gapRoleEvent_t *)pMsg);
            break;
        }

        case GATT_MSG_EVENT:
        {
            gattMsgEvent_t *pMsgEvent;

            pMsgEvent = (gattMsgEvent_t *)pMsg;
            if(pMsgEvent->method == ATT_MTU_UPDATED_EVENT)
            {
                uint8_t connItem;

                /* The MTU belongs to one link only: remember it there instead
                 * of globally, so a later connection cannot shrink the payload
                 * of an already raised link. */
                for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
                {
                    if(peripheralConnList[connItem].connHandle == pMsgEvent->connHandle)
                    {
                        peripheralConnList[connItem].mtu = pMsgEvent->msg.mtuEvt.MTU;
                        break;
                    }
                }

                PRINT("mtu exchange: %d\n", pMsgEvent->msg.mtuEvt.MTU);
            }
            else
            {
                /* Anything else the peer asks for, so a rejected request is
                 * visible instead of leaving the PC to report it. */
                PRINT("peri gatt msg %x st %x\n", pMsgEvent->method, pMsgEvent->hdr.status);
            }
            break;
        }

        default:
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkEstablished
 *
 * @brief   Process link established.
 *
 * @param   pEvent - event to process
 *
 * @return  none
 */
static void Peripheral_LinkEstablished(gapRoleEvent_t *pEvent)
{
    gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;
    // See if already connected
    uint8_t connItem;
    // 查询是否有空余连接条目
    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        if(peripheralConnList[connItem].connHandle == GAP_CONNHANDLE_INIT)
            break;
    }
    if(connItem == PERIPHERAL_MAX_CONNECTION)
    {
        GAPRole_TerminateLink(pEvent->linkCmpl.connectionHandle);
        PRINT("Connection max...\n");
    }
    else
    {
        PRINT("connItem : %#x\n", connItem);
        peripheralConnList[connItem].connHandle = event->connectionHandle;
        peripheralConnList[connItem].connInterval = event->connInterval;
        peripheralConnList[connItem].connSlaveLatency = event->connLatency;
        peripheralConnList[connItem].connTimeout = event->connTimeout;
        peripheralConnList[connItem].mtu = ATT_MTU_SIZE;

        /* Claim this connection's CCCD slot right away.  While it has none, the
         * first subscribe from the PC is answered with 0x11 and the PC drops
         * the link again, over and over. */
        SimpleProfile_InitCharCfg(event->connectionHandle);

        /* NOTE: the Client Characteristic Configuration belongs to the peer and
         * is written by whoever subscribes (phone, PC or a peer board).  It must
         * NOT be enabled locally here: BlueZ refuses "StartNotify" when the CCCD
         * it read is already enabled, which is why "notify on" failed on Linux
         * while the Windows debugging tools still worked. */

        // Set timer for param update event
        tmos_start_task(peripheralConnList[connItem].taskID, SBP_PARAM_UPDATE_EVT, SBP_PARAM_UPDATE_DELAY);

        // Set timer for phy update event
        tmos_start_task(peripheralConnList[connItem].taskID, SBP_PHY_UPDATE_EVT, SBP_PHY_UPDATE_DELAY);

        // Start read rssi
        tmos_start_task(peripheralConnList[connItem].taskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);

        PRINT("Conn %x - Int %x \n", event->connectionHandle, event->connInterval);

        // 查询是否有空余连接条目
        for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
        {
            PRINT("connItem check: %#x\n", connItem);
            PRINT("connHandle : %#x\n", peripheralConnList[connItem].connHandle);
            if(peripheralConnList[connItem].connHandle == GAP_CONNHANDLE_INIT)
            {
                break;
            }
        }
        if(connItem < PERIPHERAL_MAX_CONNECTION)
        {
            // Restart advertising
            {
                PRINT("Advertising.. \n");
                uint8_t advertising_enable = TRUE;
                GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
            }
        }
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkTerminated
 *
 * @brief   Process link terminated.
 *
 * @param   pEvent - event to process
 *
 * @return  none
 */
static void Peripheral_LinkTerminated(gapRoleEvent_t *pEvent)
{
    gapTerminateLinkEvent_t *event = (gapTerminateLinkEvent_t *)pEvent;

    uint8_t connItem;
    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        if(peripheralConnList[connItem].connHandle == event->connectionHandle)
            break;
    }
    if(connItem == PERIPHERAL_MAX_CONNECTION)
    {
        //不会执行到此处
        return;
    }
    peripheralConnList[connItem].connHandle = GAP_CONNHANDLE_INIT;
    peripheralConnList[connItem].connInterval = 0;
    peripheralConnList[connItem].connSlaveLatency = 0;
    peripheralConnList[connItem].connTimeout = 0;
    peripheralConnList[connItem].mtu = ATT_MTU_SIZE;
    tmos_stop_task(peripheralConnList[connItem].taskID, SBP_READ_RSSI_EVT);

    /* 上位机若正连在这条链路上, 立刻判离线并通知全网, 不用等心跳超时 */
    MESH_OnPcLinkClosed(event->connectionHandle);

    // Restart advertising
    {
        PRINT("Advertising.. \n");
        uint8_t advertising_enable = TRUE;
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
    }
}

/*********************************************************************
 * @fn      peripheralRssiCB
 *
 * @brief   RSSI callback.
 *
 * @param   connHandle - connection handle
 * @param   rssi - RSSI
 *
 * @return  none
 */
static void peripheralRssiCB(uint16_t connHandle, int8_t rssi)
{
    /* 每 10 次才打一条(约 20 s), 否则串口日志会被这一条刷满 */
    static uint8_t rssiCnt;

    if(++rssiCnt >= 10)
    {
        rssiCnt = 0;
        PRINT("RSSI -%d dB Conn  %x \n", -rssi, connHandle);
    }
}

/*********************************************************************
 * @fn      peripheralParamUpdateCB
 *
 * @brief   Parameter update complete callback
 *
 * @param   connHandle - connect handle
 *          connInterval - connect interval
 *          connSlaveLatency - connect slave latency
 *          connTimeout - connect timeout
 *
 * @return  none
 */
static void peripheralParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                    uint16_t connSlaveLatency, uint16_t connTimeout)
{
    uint8_t connItem;
    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        if(connHandle == peripheralConnList[connItem].connHandle)
        {
            peripheralConnList[connItem].connInterval = connInterval;
            peripheralConnList[connItem].connSlaveLatency = connSlaveLatency;
            peripheralConnList[connItem].connTimeout = connTimeout;

            PRINT("Update %x - Int %d \n", connHandle, connInterval);
            if(connInterval < DEFAULT_DESIRED_MIN_CONN_INTERVAL || connInterval > DEFAULT_DESIRED_MAX_CONN_INTERVAL)
            {
                //if connect interval not between with the define scope ,  Set timer for param update event
                tmos_start_task(peripheralConnList[connItem].taskID, SBP_PARAM_UPDATE_EVT, SBP_PARAM_UPDATE_DELAY);
            }
            break;
        }
    }
}

/*********************************************************************
 * @fn      peripheralStateNotificationCB
 *
 * @brief   Notification from the profile of a state change.
 *
 * @param   newState - new state
 *
 * @return  none
 */
static void peripheralStateNotificationCB(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    /*
     * 蓝牙状态提示只由真实事件驱动。广播开关会被"广播门控"每 250 ms 切一次,
     * 用 GAP 状态推提示会让提示被反复重启、永远走不完 2 秒, 表现为卡在蓝色
     * (或绿色) 提示上。所以: 连接建立提示每次都给, "广播中"提示只在开机后
     * 第一次开始广播时给一次。
     */
    {
        static uint8_t advertHintShown;
        uint8_t        roleState = (uint8_t)(newState & GAPROLE_STATE_ADV_MASK);

        if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
        {
            IND_SetBleState(IND_BLE_CONN);
        }
        else if((roleState == GAPROLE_ADVERTISING) && !advertHintShown &&
                ((pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT) ||
                 (pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)))
        {
            advertHintShown = 1;
            IND_SetBleState(IND_BLE_ADV);
        }
    }

    switch(newState & GAPROLE_STATE_ADV_MASK)
    {
        case GAPROLE_STARTED:
            PRINT("Initialized..\n");
            break;

        case GAPROLE_ADVERTISING:
            if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                Peripheral_LinkTerminated(pEvent);
                PRINT("Disconnected.. Reason:%x\n", pEvent->linkTerminate.reason);
                PRINT("Advertising..\n");
            }
            else if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Advertising..\n");
            }
            break;

        case GAPROLE_CONNECTED:
            if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                Peripheral_LinkEstablished(pEvent);
                PRINT("Connected..\n");
            }
            break;

        case GAPROLE_CONNECTED_ADV:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Connected Advertising..\n");
            }
            break;

        case GAPROLE_WAITING:
            if(pEvent->gap.opcode == GAP_END_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Waiting for advertising..\n");
            }
            else if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                Peripheral_LinkTerminated(pEvent);
                PRINT("Disconnected.. Reason:%x\n", pEvent->linkTerminate.reason);
            }
            else if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                if(pEvent->gap.hdr.status != SUCCESS)
                {
                    PRINT("Waiting for advertising..\n");
                }
                else
                {
                    PRINT("Error..\n");
                }
            }
            else
            {
                PRINT("Error..%x\n", pEvent->gap.opcode);
            }
            break;

        case GAPROLE_ERROR:
            PRINT("Error..\n");
            break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      peripheralNotifySend
 *
 * @brief   Send one raw notification on a connection.
 *
 * @param   connHandle - connection handle
 * @param   pValue - data to notify
 * @param   len - length of data
 *
 * @return  1 - handed over to the stack, 0 - no buffer available right now,
 *          -1 - refused for good (peer not subscribed or link gone)
 */
static int8_t peripheralNotifySend(uint16_t connHandle, const uint8_t *pValue, uint16_t len)
{
    sysMark(0x31);
    attHandleValueNoti_t noti;
    bStatus_t            status;

    noti.len = len;
    noti.pValue = GATT_bm_alloc(connHandle, ATT_HANDLE_VALUE_NOTI, noti.len, NULL, 0);
    if(noti.pValue == NULL)
    {
        return 0;
    }
    tmos_memcpy(noti.pValue, (void *)pValue, noti.len);

    status = simpleProfile_Notify(connHandle, &noti);
    if(status != SUCCESS)
    {
        GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);

        /* Only a busy stack is worth retrying.  A refused notification (peer
         * has not subscribed, link is gone) can never succeed: retrying it
         * spins the sender at full speed and floods the UART, which starves
         * the BLE stack until the chip resets. */
        if(status == blePending)
        {
            return 0;
        }
        return -1;
    }
    return 1;
}

/*********************************************************************
 * @fn      peripheralNotifySendChunk
 *
 * @brief   Push the next chunk of a queued frame out.  Called from the
 *          SBP_NOTIFY_EVT timer, so a busy stack simply retries later.
 *
 * @param   pItem - queued frame
 *
 * @return  none
 */
static void peripheralNotifySendChunk(peripheralNotifyItem_t *pItem)
{
    static uint8_t chunk[SBP_NOTIFY_BUF_LEN];
    uint16_t       chunkData = (uint16_t)(peripheralMtuOf(pItem->connHandle) - 3 - SBP_NOTIFY_CHUNK_HEAD_LEN);

    /* Never let an odd MTU turn this into an endless run of empty chunks. */
    if(chunkData == 0 || chunkData > SBP_NOTIFY_BUF_LEN)
    {
        chunkData = SBP_NOTIFY_BUF_LEN;
    }
    uint16_t       remain;
    uint16_t       i;
    int8_t         sent;

    remain = (uint16_t)(pItem->len - pItem->offset);
    if(remain == 0)
    {
        pItem->active = 0;
        return;
    }
    if(remain > chunkData)
    {
        remain = chunkData;
    }

    chunk[0] = SBP_NOTIFY_CHUNK_MAGIC;
    chunk[1] = pItem->seq;
    chunk[2] = pItem->total;
    for(i = 0; i < remain; i++)
    {
        chunk[SBP_NOTIFY_CHUNK_HEAD_LEN + i] = pItem->buf[pItem->offset + i];
    }

    sent = peripheralNotifySend(pItem->connHandle, chunk, (uint16_t)(remain + SBP_NOTIFY_CHUNK_HEAD_LEN));
    if(sent > 0)
    {
        pItem->offset = (uint16_t)(pItem->offset + remain);
        pItem->seq++;
        if(pItem->offset >= pItem->len)
        {
            pItem->active = 0;
        }
    }
    else if(sent < 0)
    {
        /* Refused for good (no subscription on that link): drop it instead of
         * hammering the stack once per SBP_NOTIFY_EVT tick. */
        pItem->active = 0;
    }
}

/*********************************************************************
 * @fn      peripheralNotifyEnqueue
 *
 * @brief   Queue a frame that is too long for a single notification.  The
 *          chunks are then pushed out by the SBP_NOTIFY_EVT timer.
 *
 * @param   connHandle - connection handle
 * @param   pValue - frame data
 * @param   len - frame length
 *
 * @return  none
 */
static void peripheralNotifyEnqueue(uint16_t connHandle, const uint8_t *pValue, uint16_t len)
{
    uint16_t chunkData = (uint16_t)(peripheralMtuOf(connHandle) - 3 - SBP_NOTIFY_CHUNK_HEAD_LEN);
    uint8_t  i;

    if(chunkData == 0 || chunkData > SBP_NOTIFY_BUF_LEN)
    {
        chunkData = SBP_NOTIFY_BUF_LEN;
    }

    for(i = 0; i < SBP_NOTIFY_ITEM_MAX; i++)
    {
        if(!peripheralNotifyList[i].active)
        {
            break;
        }
    }
    if(i == SBP_NOTIFY_ITEM_MAX)
    {
        PRINT("peri notify queue full, frame dropped\n");
        return;
    }
    if(len > SBP_NOTIFY_BUF_LEN)
    {
        len = SBP_NOTIFY_BUF_LEN;
    }

    tmos_memcpy(peripheralNotifyList[i].buf, (void *)pValue, len);
    peripheralNotifyList[i].connHandle = connHandle;
    peripheralNotifyList[i].len = len;
    peripheralNotifyList[i].offset = 0;
    peripheralNotifyList[i].seq = 0;
    peripheralNotifyList[i].total = (uint8_t)((len + chunkData - 1) / chunkData);
    peripheralNotifyList[i].active = 1;

    tmos_start_task(Peripheral_TaskID, SBP_NOTIFY_EVT, MS1_TO_SYSTEM_TIME(SBP_NOTIFY_EVT_PERIOD_MS));
    PRINT("peri notify %d bytes split into %d chunks (mtu %d)\n",
          len, peripheralNotifyList[i].total, peripheralMtuOf(connHandle));
}

/*********************************************************************
 * @fn      peripheralChar4Notify
 *
 * @brief   Prepare and send simpleProfileChar4 notification
 *
 * @param   connHandle - connection handle
 * @param   pValue - data to notify
 * @param   len - length of data
 *
 * @return  none
 */
/*********************************************************************
 * @fn      peripheralChar4Notify
 *
 * @brief   Prepare and send simpleProfileChar4 notification.  A frame that
 *          fits into the current MTU goes out as a single packet; a longer one
 *          is queued and sent as a chunked sequence (chunk header: 0x5A,
 *          index, count) that the PC has to reassemble.
 *
 * @param   connHandle - connection handle
 * @param   pValue - data to notify
 * @param   len - length of data
 *
 * @return  none
 */
static void peripheralChar4Notify(uint16_t connHandle, uint8_t *pValue, uint16_t len)
{
    int8_t sent;

    if(len == 0)
    {
        return;
    }
    if(len <= (peripheralMtuOf(connHandle) - 3))
    {
        sent = peripheralNotifySend(connHandle, pValue, len);
        if(sent > 0)
        {
            return;
        }
        if(sent < 0)
        {
            /* The link does not take notifications at all (the peer did not
             * subscribe): queueing it would only burn CPU, so drop the frame
             * and let the CCCD state be reported once by the profile. */
            return;
        }
        /* No GATT buffer free right now.  Dropping the frame here is what made
         * NFC reports disappear every now and then, so hand it to the chunked
         * sender instead: it keeps retrying until the stack takes it. */
        PRINT("peri notify busy, queueing\n");
    }

    peripheralNotifyEnqueue(connHandle, pValue, len);
}

/*********************************************************************
 * @fn      simpleProfileChangeCB
 *
 * @brief   Callback from SimpleBLEProfile indicating a value change
 *
 * @param   paramID - parameter ID of the value that was changed.
 *          pValue - pointer to data that was changed
 *          len - length of data
 *
 * @return  none
 */
static void simpleProfileChangeCB(uint16_t connHandle, uint8_t paramID, uint8_t *pValue, uint16_t len)
{
    switch(paramID)
    {
        case SIMPLEPROFILE_CHAR1:
        {
            /* Downlink frame from the PC or from another board acting as a
             * central: hand it over to the mesh layer. */
            PRINT("mesh in from central %x len %d\n", connHandle, len);
            MESH_OnFrame(pValue, len, 0, connHandle);
            break;
        }

        case SIMPLEPROFILE_CHAR3:
        {
            uint8_t newValue[SIMPLEPROFILE_CHAR3_LEN];
            tmos_memcpy(newValue, pValue, len);
            PRINT("profile ChangeCB CHAR3.. \n");
            break;
        }

        default:
            // should not reach here!
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_SendExcept
 *
 * @brief   Notify a raw mesh frame to every connected central except one.
 *
 * @param   skipConn - connection handle to skip, 0xFFFF to notify all
 * @param   data - frame data
 * @param   len - frame length
 *
 * @return  none
 */
void Peripheral_SendExcept(uint16_t skipConn, const uint8_t *data, uint16_t len)
{
    uint8_t connItem;

    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        uint16_t connHandle = peripheralConnList[connItem].connHandle;

        if(connHandle == GAP_CONNHANDLE_INIT)
        {
            continue;
        }
        if(connHandle == skipConn)
        {
            continue;
        }
        peripheralChar4Notify(connHandle, (uint8_t *)data, len);
        PRINT("peri notify %x len %d\n", connHandle, len);
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkCount
 *
 * @brief   Number of currently connected centrals.
 *
 * @return  connection count
 */
uint8_t Peripheral_LinkCount(void)
{
    uint8_t connItem;
    uint8_t count = 0;

    for(connItem = 0; connItem < PERIPHERAL_MAX_CONNECTION; connItem++)
    {
        if(peripheralConnList[connItem].connHandle != GAP_CONNHANDLE_INIT)
        {
            count++;
        }
    }
    return count;
}

/*********************************************************************
*********************************************************************/
