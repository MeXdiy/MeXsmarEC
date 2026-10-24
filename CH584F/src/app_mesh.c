/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_mesh.c
 * Description        : Application level mesh frame handling.
 *
 * Every board is at the same time a BLE peripheral (can be connected by the
 * PC or by another board) and a BLE central (connects other boards).  A frame
 * received on one link is flooded to all other links, with a small
 * (msgId + source address + type) de-duplication table, so the whole group
 * behaves like a mesh and the PC can reach any board through any board.
 *******************************************************************************/

#include "CONFIG.h"
#include "CH58x_common.h"
#include "app_mesh.h"
#include "app_ind.h"
#include "app_led.h"
#include "app_nfc.h"
#include "ble_peripheral.h"
#include "ble_central.h"

/*********************************************************************
 * MACROS
 */
#define MESH_DUP_TBL_SIZE       24

/* 上位机在线状态: 心跳 2 s 广播, 6 s 无心跳判离线 */
#define MESH_PC_TICK_MS         500
#define MESH_PC_TIMEOUT_MS      6000
#define MESH_PC_TICKS           (MESH_PC_TIMEOUT_MS / MESH_PC_TICK_MS)
#define MESH_PC_BCAST_MS        2000
#define MESH_PC_BCAST_TICKS     (MESH_PC_BCAST_MS / MESH_PC_TICK_MS)
#define MESH_PC_GRACE_TICKS     4                       /* 上电宽限 2 s */
#define MESH_PC_EVT             0x0001

/*********************************************************************
 * TYPEDEFS
 */
typedef struct
{
    uint32_t msgId;                     /* 32 bit sequence, MSB first on air */
    uint8_t  addr[MESH_ADDR_LEN];
    uint8_t  type;
    uint8_t  valid;
} meshDupItem_t;

/*********************************************************************
 * LOCAL VARIABLES
 */
static meshDupItem_t meshDupTbl[MESH_DUP_TBL_SIZE];
static uint8_t       meshDupIdx;
static uint32_t      meshMsgId;
static uint8_t       meshLocalMac[MESH_ADDR_LEN];

/* 上位机在线状态 */
static uint16_t meshPcTaskId;
static uint16_t meshPcAliveTicks;   /* 非 0: 心跳还有效                 */
static uint16_t meshPcBcastCnt;     /* 本板状态广播计数                 */
static uint8_t  meshPcOnline;       /* 本板认为上位机在线               */
static uint16_t meshPcLink;         /* 电脑所在的从机连接, 无则 INVALID */
static uint8_t  meshPcLastSrc[MESH_ADDR_LEN];   /* 最近报"在线"的来源   */

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void     meshSetMsgId(uint8_t *p, uint32_t id);
static uint32_t meshGetMsgId(const uint8_t *p);
static uint8_t  meshAddrIsLocal(const uint8_t *addr);
static uint8_t  meshAddrIsBroadcast(const uint8_t *addr);
static uint8_t  meshIsDup(uint32_t msgId, const uint8_t *addr, uint8_t type);
static void     meshMarkDup(uint32_t msgId, const uint8_t *addr, uint8_t type);
static void     meshFlood(const uint8_t *frame, uint16_t len);
static void     meshSendLedAck(uint8_t on);
static void     meshSendPcState(uint8_t online);
static void     meshSetPcOnline(uint8_t online);
static uint16_t meshPcProcessEvent(uint8_t task_id, uint16_t events);

/*********************************************************************
 * @fn      MESH_Init
 *
 * @brief   Init the mesh module and latch the local BLE MAC address.
 *
 * @return  none
 */
void MESH_Init(void)
{
    tmos_memset(meshDupTbl, 0, sizeof(meshDupTbl));
    meshDupIdx = 0;
    meshMsgId = 0;

    /*
     * 上电给 2 s 宽限期: 期间收到上位机心跳就保持在线, 收不到就转离线并让状态
     * 灯红闪。宽限期内不广播"在线", 免得给全网一个假状态。
     */
    meshPcAliveTicks = MESH_PC_GRACE_TICKS;
    meshPcBcastCnt = 0;
    meshPcOnline = 1;
    meshPcLink = MESH_CONN_INVALID;

    MESH_RefreshLocalMac();

    meshPcTaskId = TMOS_ProcessEventRegister(meshPcProcessEvent);
    tmos_start_task(meshPcTaskId, MESH_PC_EVT, MS1_TO_SYSTEM_TIME(MESH_PC_TICK_MS));
}

/*********************************************************************
 * @fn      MESH_RefreshLocalMac
 *
 * @brief   Re-read the local BLE MAC address from the GAP layer and latch
 *          it.  The GAP address is only valid after the GAP role has been
 *          started, so this is called again from the GAP init done event.
 *
 * @return  none
 */
void MESH_RefreshLocalMac(void)
{
    uint8_t mac[B_ADDR_LEN];
    uint8_t i;

    tmos_memset(mac, 0, sizeof(mac));
    GAPRole_GetParameter(GAPROLE_BD_ADDR, mac);

    /*
     * The GAP address is stored least significant byte first, while a PC side
     * BLE scanner displays it most significant byte first.  Latch it in
     * display order so the MAC reported to the PC matches what the PC sees
     * when it scans or connects to this board.
     */
    for(i = 0; i < MESH_ADDR_LEN; i++)
    {
        meshLocalMac[i] = mac[MESH_ADDR_LEN - 1 - i];
    }

    PRINT("mesh local mac: %02x %02x %02x %02x %02x %02x\n",
          meshLocalMac[0], meshLocalMac[1], meshLocalMac[2],
          meshLocalMac[3], meshLocalMac[4], meshLocalMac[5]);
}

/*********************************************************************
 * @fn      MESH_GetLocalMac
 *
 * @brief   Copy the local BLE MAC address out.
 *
 * @return  none
 */
void MESH_GetLocalMac(uint8_t *mac)
{
    tmos_memcpy(mac, meshLocalMac, MESH_ADDR_LEN);
}

/*********************************************************************
 * @fn      meshSetMsgId
 *
 * @brief   Store a 32 bit message sequence into a frame, MSB first.
 *
 * @return  none
 */
static void meshSetMsgId(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 24);
    p[1] = (uint8_t)(id >> 16);
    p[2] = (uint8_t)(id >> 8);
    p[3] = (uint8_t)id;
}

/*********************************************************************
 * @fn      meshGetMsgId
 *
 * @brief   Read the 32 bit message sequence of a frame, MSB first.
 *
 * @return  message sequence
 */
static uint32_t meshGetMsgId(const uint8_t *p)
{
    return (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

/*********************************************************************
 * @fn      meshAddrIsLocal
 *
 * @brief   Check whether the address is this board.
 *
 * @return  1 - yes, 0 - no
 */
static uint8_t meshAddrIsLocal(const uint8_t *addr)
{
    return tmos_memcmp((void *)addr, meshLocalMac, MESH_ADDR_LEN);
}

/*********************************************************************
 * @fn      meshAddrIsBroadcast
 *
 * @brief   Check whether the address is the broadcast address.
 *
 * @return  1 - yes, 0 - no
 */
static uint8_t meshAddrIsBroadcast(const uint8_t *addr)
{
    uint8_t i;
    for(i = 0; i < MESH_ADDR_LEN; i++)
    {
        if(addr[i] != 0xFF)
        {
            return 0;
        }
    }
    return 1;
}

/*********************************************************************
 * @fn      meshIsDup
 *
 * @brief   Flood de-duplication check.
 *
 * @return  1 - already handled, 0 - new frame
 */
static uint8_t meshIsDup(uint32_t msgId, const uint8_t *addr, uint8_t type)
{
    uint8_t i;
    for(i = 0; i < MESH_DUP_TBL_SIZE; i++)
    {
        if(meshDupTbl[i].valid &&
           meshDupTbl[i].msgId == msgId &&
           meshDupTbl[i].type == type &&
           tmos_memcmp((void *)meshDupTbl[i].addr, (void *)addr, MESH_ADDR_LEN))
        {
            return 1;
        }
    }
    return 0;
}

/*********************************************************************
 * @fn      meshMarkDup
 *
 * @brief   Remember a handled frame, ring buffer style.
 *
 * @return  none
 */
static void meshMarkDup(uint32_t msgId, const uint8_t *addr, uint8_t type)
{
    meshDupTbl[meshDupIdx].msgId = msgId;
    meshDupTbl[meshDupIdx].type = type;
    tmos_memcpy(meshDupTbl[meshDupIdx].addr, (void *)addr, MESH_ADDR_LEN);
    meshDupTbl[meshDupIdx].valid = 1;
    meshDupIdx = (meshDupIdx + 1) % MESH_DUP_TBL_SIZE;
}

/*********************************************************************
 * @fn      meshFlood
 *
 * @brief   Send a frame generated locally to every link.
 *
 * @return  none
 */
static void meshFlood(const uint8_t *frame, uint16_t len)
{
    Peripheral_SendExcept(MESH_CONN_INVALID, frame, len);
    Central_SendExcept(MESH_CONN_INVALID, frame, len);
}

/*********************************************************************
 * @fn      meshSendLedAck
 *
 * @brief   Send a LED control acknowledgement carrying our own MAC.
 *
 * @return  none
 */
static void meshSendLedAck(uint8_t on)
{
    uint8_t  frame[MESH_LEN_LED];
    uint32_t id = meshMsgId++;

    frame[MESH_OFF_HEAD] = MESH_HEAD;
    meshSetMsgId(&frame[MESH_OFF_MSGID], id);
    frame[MESH_OFF_TYPE] = MESH_TYPE_LED_ACK;
    tmos_memcpy(&frame[MESH_OFF_ADDR], meshLocalMac, MESH_ADDR_LEN);
    frame[MESH_OFF_DATA] = on;

    meshMarkDup(id, meshLocalMac, MESH_TYPE_LED_ACK);
    meshFlood(frame, MESH_LEN_LED);
}

/*********************************************************************
 * @fn      meshSendPcState
 *
 * @brief   Tell the mesh whether the upper computer is reachable from this
 *          board.  addr = our own MAC, which is what tells it apart from the
 *          PC's own ping (addr = broadcast).
 *
 * @param   online - 1: reachable, 0: lost
 *
 * @return  none
 */
static void meshSendPcState(uint8_t online)
{
    uint8_t  frame[MESH_LEN_PC_STATE];
    uint32_t id = meshMsgId++;

    /* 广播这一刻就是全网的闪烁相位基准: 接收方在收到这帧时对齐, 发送方在
     * 发出这一刻对齐, 于是所有板子(含本板)相位一致。 */
    IND_SyncBlink();

    frame[MESH_OFF_HEAD] = MESH_HEAD;
    meshSetMsgId(&frame[MESH_OFF_MSGID], id);
    frame[MESH_OFF_TYPE] = MESH_TYPE_PC_STATE;
    tmos_memcpy(&frame[MESH_OFF_ADDR], meshLocalMac, MESH_ADDR_LEN);
    frame[MESH_OFF_DATA] = online ? 1 : 0;

    meshMarkDup(id, meshLocalMac, MESH_TYPE_PC_STATE);
    meshFlood(frame, MESH_LEN_PC_STATE);
}

/*********************************************************************
 * @fn      meshSetPcOnline
 *
 * @brief   Update the presence state, tell the mesh and the indicator.
 *
 * @param   online - 1: reachable, 0: lost
 *
 * @return  none
 */
static void meshSetPcOnline(uint8_t online)
{
    if(online == meshPcOnline)
    {
        return;
    }

    meshPcOnline = online;
    PRINT("pc state -> %d\n", online ? 1 : 0);
    IND_SetPcOnline(online);

    /* 只有真正接着上位机的那块板才有资格广播状态: 其它板子只是转发, 否则
     * 全网会出现多个"在线"来源, 相位基准和离线广播都跟着乱。 */
    if(meshPcLink != MESH_CONN_INVALID)
    {
        meshSendPcState(online);
    }
}

/*********************************************************************
 * @fn      MESH_OnPcLinkClosed
 *
 * @brief   A peripheral link went down.  If the upper computer was pinging on
 *          that link it is declared lost right away instead of waiting for
 *          the heartbeat to time out.
 *
 * @param   connHandle - the link that closed
 *
 * @return  none
 */
void MESH_OnPcLinkClosed(uint16_t connHandle)
{
    if((meshPcLink == MESH_CONN_INVALID) || (meshPcLink != connHandle))
    {
        return;
    }

    meshPcLink = MESH_CONN_INVALID;
    meshPcAliveTicks = 0;
    PRINT("pc link %x closed\n", connHandle);
    meshSetPcOnline(0);
}

/*********************************************************************
 * @fn      MESH_PcIsUplink
 *
 * @brief   1 when the upper computer is attached to this board.
 *
 * @return  1 - attached here, 0 - attached somewhere else (or nowhere)
 */
uint8_t MESH_PcIsUplink(void)
{
    return (meshPcLink != MESH_CONN_INVALID) ? 1 : 0;
}

/*********************************************************************
 * @fn      meshPcProcessEvent
 *
 * @brief   Presence timer: expire the upper computer heartbeat and broadcast
 *          our state every MESH_PC_BCAST_MS while it is online.
 *
 * @return  events not processed
 */
static uint16_t meshPcProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;

    if(events & MESH_PC_EVT)
    {
        if(meshPcAliveTicks)
        {
            meshPcAliveTicks--;
            if(meshPcAliveTicks == 0)
            {
                PRINT("pc heartbeat timeout\n");
                meshSetPcOnline(0);
            }
        }

        if(meshPcOnline && (meshPcLink != MESH_CONN_INVALID))
        {
            if(++meshPcBcastCnt >= MESH_PC_BCAST_TICKS)
            {
                meshPcBcastCnt = 0;
                meshSendPcState(1);
            }
        }
        else
        {
            meshPcBcastCnt = 0;
        }

        tmos_start_task(meshPcTaskId, MESH_PC_EVT, MS1_TO_SYSTEM_TIME(MESH_PC_TICK_MS));
        return (events ^ MESH_PC_EVT);
    }

    return 0;
}

/*********************************************************************
 * @fn      MESH_ReportNfc
 *
 * @brief   Report an NFC card event into the whole mesh, together with the
 *          NDEF text read from the tag.
 *
 * @param   uid     - card UID, MESH_UID_LEN bytes
 * @param   text    - NDEF text of the tag, NULL / empty when there is none
 * @param   textLen - length of the text
 *
 * @return  none
 */
void MESH_ReportNfc(const uint8_t *uid, const uint8_t *text, uint8_t textLen)
{
    uint8_t  frame[MESH_LEN_NFC_MAX];
    uint16_t len;
    uint32_t id = meshMsgId++;
    uint8_t  i;

    if(textLen > MESH_NFC_TEXT_MAX)
    {
        textLen = MESH_NFC_TEXT_MAX;
    }

    frame[MESH_OFF_HEAD] = MESH_HEAD;
    meshSetMsgId(&frame[MESH_OFF_MSGID], id);
    frame[MESH_OFF_TYPE] = MESH_TYPE_NFC;
    tmos_memcpy(&frame[MESH_OFF_ADDR], meshLocalMac, MESH_ADDR_LEN);
    for(i = 0; i < MESH_UID_LEN; i++)
    {
        frame[MESH_OFF_DATA + i] = uid[i];
    }
    frame[MESH_OFF_DATA + MESH_UID_LEN] = textLen;
    if((textLen != 0) && (text != NULL))
    {
        tmos_memcpy(&frame[MESH_OFF_DATA + MESH_UID_LEN + 1], (void *)text, textLen);
    }
    len = (uint16_t)(MESH_OFF_DATA + MESH_UID_LEN + 1 + textLen);

    PRINT("nfc uid: %02x %02x %02x %02x %02x %02x %02x\n",
          uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
    PRINT("nfc text(%d): %s\n", textLen, (text != NULL) ? (char *)text : "");

    meshMarkDup(id, meshLocalMac, MESH_TYPE_NFC);
    meshFlood(frame, len);
}

/*********************************************************************
 * @fn      MESH_OnFrame
 *
 * @brief   Handle one received mesh frame: act locally when needed and
 *          flood it to every other link.
 *
 * @param   data        - raw frame
 * @param   len         - frame length
 * @param   fromCentral - 1: came from a link where this board is central
 * @param   fromConn    - handle the frame came from
 *
 * @return  none
 */
void MESH_OnFrame(const uint8_t *data, uint16_t len, uint8_t fromCentral, uint16_t fromConn)
{
    uint8_t        type;
    uint32_t       msgId;
    const uint8_t *addr;
    const uint8_t *payload;
    uint8_t        textLen;

    if(len < MESH_LEN_PC_STATE || data[MESH_OFF_HEAD] != MESH_HEAD)
    {
        return;
    }

    msgId = meshGetMsgId(&data[MESH_OFF_MSGID]);
    type = data[MESH_OFF_TYPE];
    addr = &data[MESH_OFF_ADDR];
    payload = &data[MESH_OFF_DATA];

    /*
     * 0xFF with the broadcast address is the upper computer's own ping: it
     * refreshes our heartbeat, and we replace it with our own state frame so
     * the other boards learn the same presence without the ping being
     * forwarded (a forwarded ping would look like the PC is attached to every
     * board that relays it, and those boards would then report a false loss
     * when the relaying link drops).
     */
    if((type == MESH_TYPE_PC_STATE) && meshAddrIsBroadcast(addr))
    {
        if(meshPcAliveTicks == 0)
        {
            PRINT("pc ping received\n");
        }
        meshPcAliveTicks = MESH_PC_TICKS;
        if(fromCentral == 0)
        {
            meshPcLink = fromConn;
        }
        IND_SyncBlink();
        meshSetPcOnline(1);
        return;
    }

    if(type == MESH_TYPE_NFC)
    {
        if(len < MESH_LEN_NFC_MIN || len > MESH_LEN_NFC_MAX)
        {
            return;
        }
        /* the text length byte has to match the frame it arrived in */
        textLen = payload[MESH_UID_LEN];
        if(textLen > (uint8_t)(len - MESH_LEN_NFC_MIN))
        {
            textLen = (uint8_t)(len - MESH_LEN_NFC_MIN);
        }
    }
    else if(type == MESH_TYPE_NFC_WRITE)
    {
        if(len < MESH_OFF_DATA || len > MESH_LEN_NFC_WRITE_MAX)
        {
            return;
        }
        textLen = (uint8_t)(len - MESH_OFF_DATA);
    }
    else if(type == MESH_TYPE_LED_SET || type == MESH_TYPE_LED_ACK)
    {
        textLen = 36;
    }
    else if(type == MESH_TYPE_PC_STATE)
    {
        if(len < MESH_LEN_PC_STATE)
        {
            return;
        }
        textLen = 1;
    }
    else
    {
        return;
    }

    if(meshIsDup(msgId, addr, type))
    {
        return;
    }
    meshMarkDup(msgId, addr, type);

    if(type == MESH_TYPE_NFC)
    {
        PRINT("mesh nfc node %02x%02x%02x%02x%02x%02x uid %02x %02x %02x %02x %02x %02x %02x text len %d\n",
              addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
              payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], payload[6],
              textLen);
    }
    else if(type == MESH_TYPE_LED_SET)
    {
        // 由于 MESH_LEN_LED 已修改为 48，这里如果长度不够，前面的校验已经 return 了
        if(meshAddrIsLocal(addr))
        {
            PRINT("LED_Ctrl bitmap for this node\n");
            // 直接传入 payload 指针
            LED_Ctrl(payload);
            // 回执成功 (回传 1)
            meshSendLedAck(1);
        }
        else if(meshAddrIsBroadcast(addr))
        {
            PRINT("LED_Ctrl bitmap broadcast\n");
            // 直接传入 payload 指针
            LED_Ctrl(payload);
        }
    }
    else if(type == MESH_TYPE_NFC_WRITE)
    {
        if(meshAddrIsLocal(addr) || meshAddrIsBroadcast(addr))
        {
            PRINT("NFC write text(%d) for this node\n", textLen);
            NFC_SetWriteText(payload, textLen);
        }
    }
    else if(type == MESH_TYPE_PC_STATE)
    {
        if(payload[0])
        {
            tmos_memcpy(meshPcLastSrc, (void *)addr, MESH_ADDR_LEN);
            meshPcAliveTicks = MESH_PC_TICKS;
            IND_SyncBlink();
            meshSetPcOnline(1);
        }
        else if(tmos_memcmp((void *)addr, meshPcLastSrc, MESH_ADDR_LEN))
        {
            /* 只有"最近报过在线的那块板"才有资格宣布离线。否则一块自己链路
             * 断了的板子超时后会广播离线, 把全网的在线状态一起打掉(级联误报),
             * 真正的上位机明明还连着。 */
            meshPcAliveTicks = 0;
            meshSetPcOnline(0);
        }
    }
    else
    {
        PRINT("mesh led ack node %02x%02x%02x%02x%02x%02x on %d\n",
              addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], payload[0]);
    }

    /* flood further, but not back to the link it came from */
    Peripheral_SendExcept((fromCentral == 0) ? fromConn : MESH_CONN_INVALID, data, len);
    Central_SendExcept((fromCentral == 1) ? fromConn : MESH_CONN_INVALID, data, len);
}

/******************************** endfile @ app_mesh ***************************/
