/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_mesh.h
 * Description        : Application level mesh frame handling.
 *
 * Frame layout:
 *
 *   offset  len  field
 *   0       1    head   = MESH_HEAD
 *   1       4    msgId  message sequence (uint32_t, MSB first), used for
 *                       flood de-duplication
 *   5       1    type   MESH_TYPE_NFC / MESH_TYPE_LED_SET / MESH_TYPE_LED_ACK
 *                       / MESH_TYPE_NFC_WRITE
 *   6       6    addr   NFC / LED_ACK -> source node MAC,
 *                       LED_SET / NFC_WRITE -> target MAC
 *   12      N    data   NFC -> card UID + NDEF text, LED -> on/off(1 byte),
 *                       NFC_WRITE -> text to be written into a tag
 *
 * A frame is limited by the ATT MTU of the links: the PC link and the mesh
 * links exchange an MTU of 247, which leaves 244 octets of payload.
 *******************************************************************************/

#ifndef __APP_MESH_H
#define __APP_MESH_H

#include "CH58x_common.h"

/*
 * Records which path the firmware is in right now; the fault handler in Main.c
 * prints it, which is what makes a crash after a while point somewhere useful.
 */
extern volatile uint32_t sysLastMarker;
extern void sysMark(uint32_t marker);

#define MESH_HEAD               0xA5

#define MESH_TYPE_NFC           0x01    /* board -> PC : NFC card event     */
#define MESH_TYPE_LED_SET       0x02    /* PC -> board : LED control        */
#define MESH_TYPE_LED_ACK       0x03    /* board -> PC : LED control ack    */
#define MESH_TYPE_NFC_WRITE     0x04    /* PC -> board : write NDEF text    */
/*
 * 0xFF: upper computer presence.
 *   from the PC    : addr = FF:FF:FF:FF:FF:FF, data[0] = 1  (1 Hz ping)
 *   from a board   : addr = that board's MAC,  data[0] = 1: PC reachable,
 *                                                         0: PC lost
 */
#define MESH_TYPE_PC_STATE      0xFF

#define MESH_MSGID_LEN          4
#define MESH_ADDR_LEN           6
#define MESH_UID_LEN            7

/*
 * Upper limit of the NDEF text carried by one frame.  The ATT MTU of the PC
 * link and of the mesh links is 247, which leaves 244 octets of payload, and
 * a short NDEF record allows at most 255 octets of text; 200 fits both.
 */
#define MESH_NFC_TEXT_MAX       200

#define MESH_OFF_HEAD           0
#define MESH_OFF_MSGID          1
#define MESH_OFF_TYPE           5
#define MESH_OFF_ADDR           6
#define MESH_OFF_DATA           12

/* 
 * LED 控制帧长度：
 * 12(头部) + 1(亮度) + 3(RGB) + 32(位图) = 48 字节
 */
#define MESH_LEN_LED            (MESH_OFF_DATA + 36)                   /* 48 */
#define MESH_LEN_NFC_MIN        (MESH_OFF_DATA + MESH_UID_LEN + 1)     /* 20 */
#define MESH_LEN_NFC_MAX        (MESH_LEN_NFC_MIN + MESH_NFC_TEXT_MAX) /* 220 */
#define MESH_LEN_NFC_WRITE_MAX  (MESH_OFF_DATA + MESH_NFC_TEXT_MAX)    /* 212 */
#define MESH_LEN_PC_STATE       (MESH_OFF_DATA + 1)                    /* 13  */

#define MESH_CONN_INVALID       0xFFFF

/*
 * @brief  Init mesh module, latch the local BLE MAC address.
 */
void MESH_Init(void);

/*
 * @brief  Copy the local BLE MAC address (display order, MSB first).
 */
void MESH_GetLocalMac(uint8_t *mac);

/*
 * @brief  Re-read the local BLE MAC address from the GAP layer.  The GAP
 *         address is only valid once the GAP role has been started, so this
 *         runs again from the GAP init done event.
 */
void MESH_RefreshLocalMac(void);

/*
 * @brief  Report an NFC card event to the whole mesh (and thus to the PC).
 * @param  uid     - card UID, MESH_UID_LEN bytes, unused tail filled with 0
 * @param  text    - NDEF text read from the tag, NULL when the tag holds no
 *                   text record
 * @param  textLen - length of the text, 0 when the tag holds no text record
 */
void MESH_ReportNfc(const uint8_t *uid, const uint8_t *text, uint8_t textLen);

/*
 * @brief  Handle one received mesh frame.
 * @param  data        - raw frame
 * @param  len         - frame length
 * @param  fromCentral - 1: came from a link where this board is central,
 *                       0: came from a link where this board is peripheral
 * @param  fromConn    - connection handle the frame came from
 */
void MESH_OnFrame(const uint8_t *data, uint16_t len, uint8_t fromCentral, uint16_t fromConn);

/*
 * @brief  A peripheral link went down.  If the upper computer was pinging on
 *         that link, the presence state is cleared and the others are told
 *         right away instead of waiting for the heartbeat to time out.
 * @param  connHandle - the link that closed
 */
void MESH_OnPcLinkClosed(uint16_t connHandle);

/*
 * @brief  1 when the upper computer is attached to this board (its pings
 *         arrive on one of our own links), 0 otherwise.  Used to decide
 *         whether this board must keep advertising.
 */
uint8_t MESH_PcIsUplink(void);

#endif /* __APP_MESH_H */
