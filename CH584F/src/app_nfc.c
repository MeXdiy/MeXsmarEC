/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_nfc.c
 * Description        : NFC-A reader (PCD) polling task.
 *
 * The board polls for a card periodically.  When a card is detected its UID
 * and the NDEF text record it carries are pushed into the mesh (see
 * app_mesh.c), so the PC sees the UID, the text and the BLE MAC address of
 * the board that read the card.  A text frame armed by the mesh layer
 * (MESH_TYPE_NFC_WRITE) is written into the next card placed on the antenna.
 *******************************************************************************/

#include "CONFIG.h"
#include "CH58x_common.h"
#include "app_nfc.h"
#include "app_mesh.h"
#include "app_ind.h"
#include "wch_nfca_pcd_bsp.h"
#include "wch_nfca_ntag21x.h"

/*********************************************************************
 * MACROS
 */
#define NFC_POLL_EVT            0x0001
#define NFC_POLL_PERIOD_MS      300

/*
 * NFC 写卡自测开关
 *
 *   1 = 上电后 NFC 轮询只做写卡自测：检测到卡片就把 NFC_WRITE_TEST_TEXT
 *       写成 NDEF 文本记录，失败原地重试，全过程打印到 UART1
 *       （115200-8-N-1）。此时读卡上报和电脑远程写文本都不工作。
 *   0 = 恢复正常的读卡上报 + 电脑远程写文本。
 *
 * 也可以在工程预处理宏里定义 NFC_WRITE_TEST 覆盖下面的默认值。
 */
#ifndef NFC_WRITE_TEST
#define NFC_WRITE_TEST          0
#endif

#ifndef NFC_WRITE_TEST_TEXT
#define NFC_WRITE_TEST_TEXT     "hello"
#endif

/* Consecutive "no card" polls before the card is considered removed.
 * NFC polling may miss a card for a poll or two, so the count has to stay
 * above that jitter, but it must also stay short enough that taking the tag
 * off the antenna and putting it back reports again within about a second. */
#define NFC_CARD_GONE_CNT       6

/*
 * A card that keeps sitting on the antenna is reported again after this long,
 * so the upper computer keeps seeing it instead of the reports going silent.
 * A different card is always reported at once.
 */
#define NFC_REPORT_REPEAT_MS    5000

/* NDEF / NTAG21x layout */
#define NFC_CC_PAGE             3       /* capability container page          */
#define NFC_DATA_PAGE           4       /* first page of the NDEF data area   */
#define NFC_CC_MAGIC            0xE1    /* CC byte 0 of an NDEF formatted tag */
#define NFC_TLV_NDEF            0x03
#define NFC_TLV_TERMINATOR      0xFE
#define NFC_NDEF_TNF_WELLKNOWN  0x01
#define NFC_NDEF_TYPE_TEXT      'T'
#define NFC_NDEF_LANG_EN        0x02    /* UTF-8 text, 2 byte language code */

/* TLV header(2) + terminator(1) + short text record header(7) */
#define NFC_TEXT_OVERHEAD       10
#define NFC_DEFAULT_DATA_SIZE   144     /* NDEF data area of a NTAG213 */

#define NFC_NDEF_BUF_SIZE       256
#define NFC_WRITE_BUF_SIZE      (MESH_NFC_TEXT_MAX + NFC_TEXT_OVERHEAD + 8)

/*********************************************************************
 * LOCAL VARIABLES
 */
static uint8_t  nfcTaskId;
static uint8_t  nfcMissCnt;
static uint8_t  nfcReported;
static uint32_t nfcReportCnt;
static uint8_t  nfcNdefBuf[NFC_NDEF_BUF_SIZE];
static uint8_t  nfcTextBuf[MESH_NFC_TEXT_MAX + 1];
static uint8_t  nfcWriteText[MESH_NFC_TEXT_MAX + 1];
static uint8_t  nfcWriteBuf[NFC_WRITE_BUF_SIZE];
static uint16_t nfcWriteLen;
static uint8_t  nfcWritePending;
#if NFC_WRITE_TEST
static uint8_t  nfcTestDone;
#endif
static uint32_t nfcLastReportTick = 0;

/**
 * @brief   Whether the card currently on the antenna has to be reported.
 *
 * @return  1 when a report is due, 0 while the same presence is still fresh
 */
static uint8_t nfcShouldReport(void)
{
    if(!nfcReported)
    {
        return 1;
    }

    return ((TMOS_GetSystemClock() - nfcLastReportTick) >= MS1_TO_SYSTEM_TIME(NFC_REPORT_REPEAT_MS)) ? 1 : 0;
}

static uint16_t nfcProcessEvent(uint8_t task_id, uint16_t events);
static uint8_t  nfcSelectCard(uint8_t *uid);
static uint16_t nfcReadText(void);
static uint8_t  nfcWriteTextTag(uint16_t len, uint16_t *written);
static void     nfcCardMiss(void);
#if NFC_WRITE_TEST
static void     nfcWriteTest(void);
#endif

/*********************************************************************
 * @fn      nfcSelectCard
 *
 * @brief   Detect a card and select it (4 byte or 7 byte double cascade
 *          UID).  The card is left selected, so the caller can exchange
 *          further commands with it and has to halt it afterwards.
 *
 * @param   uid - output buffer, MESH_UID_LEN bytes (tail left untouched)
 *
 * @return  UID length (4 or 7), 0 when no card / read failed
 */
static uint8_t nfcSelectCard(uint8_t *uid)
{
    uint16_t res;
    uint8_t  len = 0;

    res = PcdRequest(PICC_REQALL);
    if(res == 0x0044 || res == 0x0004 || res == 0x0002)
    {
        res = PcdAnticoll(PICC_ANTICOLL1);
        if(res == PCD_NO_ERROR)
        {
            if(g_nfca_pcd_recv_buf[0] == 0x88)
            {
                /* 7 byte UID, second cascade level needed */
                uint8_t cascade[3];

                cascade[0] = g_nfca_pcd_recv_buf[1];
                cascade[1] = g_nfca_pcd_recv_buf[2];
                cascade[2] = g_nfca_pcd_recv_buf[3];

                if(PcdSelect(PICC_ANTICOLL1, g_nfca_pcd_recv_buf) == PCD_NO_ERROR)
                {
                    if(PcdAnticoll(PICC_ANTICOLL2) == PCD_NO_ERROR)
                    {
                        uid[0] = cascade[0];
                        uid[1] = cascade[1];
                        uid[2] = cascade[2];
                        uid[3] = g_nfca_pcd_recv_buf[0];
                        uid[4] = g_nfca_pcd_recv_buf[1];
                        uid[5] = g_nfca_pcd_recv_buf[2];
                        uid[6] = g_nfca_pcd_recv_buf[3];

                        if(PcdSelect(PICC_ANTICOLL2, g_nfca_pcd_recv_buf) == PCD_NO_ERROR)
                        {
                            len = 7;
                        }
                    }
                }
            }
            else
            {
                /* 4 byte UID */
                uid[0] = g_nfca_pcd_recv_buf[0];
                uid[1] = g_nfca_pcd_recv_buf[1];
                uid[2] = g_nfca_pcd_recv_buf[2];
                uid[3] = g_nfca_pcd_recv_buf[3];

                if(PcdSelect(PICC_ANTICOLL1, uid) == PCD_NO_ERROR)
                {
                    len = 4;
                }
            }
        }
    }

    return len;
}

/*********************************************************************
 * @fn      nfcReadText
 *
 * @brief   Read the first NDEF text record of the selected tag.
 *
 * @return  text length, the text is stored NUL terminated in nfcTextBuf
 */
static uint16_t nfcReadText(void)
{
    uint8_t  cc[16];
    uint16_t dataSize;
    uint16_t i;
    uint16_t hdrOff = 0;
    uint16_t msgLen = 0;
    uint16_t readBytes = 0;
    uint8_t *pMsg;
    uint8_t *pRec;
    uint32_t payloadLen;
    uint8_t  typeLen;
    uint8_t  langLen;
    uint16_t textLen;

    nfcTextBuf[0] = 0;

    if(PcdNTAG21xRead(NFC_CC_PAGE, cc) != PCD_NO_ERROR)
    {
        return 0;
    }
    if(cc[0] != NFC_CC_MAGIC)
    {
        return 0;   /* the tag holds no NDEF message */
    }

    dataSize = (uint16_t)cc[2] * 8;
    if(dataSize < 32)
    {
        dataSize = NFC_DEFAULT_DATA_SIZE;
    }
    if(dataSize > (NFC_NDEF_BUF_SIZE - 16))
    {
        dataSize = (NFC_NDEF_BUF_SIZE - 16);
    }

    /* read the NDEF data area in 16 byte (4 page) steps and stop as soon as
     * the TLV header told how long the message is */
    for(i = 0; (i + 16) <= dataSize; i += 16)
    {
        if(PcdNTAG21xRead((uint8_t)(NFC_DATA_PAGE + (i / 4)), &nfcNdefBuf[i]) != PCD_NO_ERROR)
        {
            return 0;
        }
        readBytes = (uint16_t)(i + 16);

        if(i == 0)
        {
            if(nfcNdefBuf[0] != NFC_TLV_NDEF)
            {
                return 0;   /* no NDEF message TLV */
            }
            if(nfcNdefBuf[1] == 0xFF)
            {
                hdrOff = 4;
                msgLen = ((uint16_t)nfcNdefBuf[2] << 8) | nfcNdefBuf[3];
            }
            else
            {
                hdrOff = 2;
                msgLen = nfcNdefBuf[1];
            }
        }

        if((hdrOff + msgLen) <= readBytes)
        {
            break;
        }
    }

    if((hdrOff == 0) || (msgLen == 0) || (readBytes < hdrOff))
    {
        return 0;
    }
    if((hdrOff + msgLen) > readBytes)
    {
        msgLen = (uint16_t)(readBytes - hdrOff);
    }

    pMsg = &nfcNdefBuf[hdrOff];
    if(msgLen < 6)
    {
        return 0;
    }
    if((pMsg[0] & 0x07) != NFC_NDEF_TNF_WELLKNOWN)
    {
        return 0;
    }

    typeLen = pMsg[1];
    if((pMsg[0] & 0x10) != 0)
    {
        /* short record: one byte of payload length */
        payloadLen = pMsg[2];
        pRec = &pMsg[3];
    }
    else
    {
        if(msgLen < 8)
        {
            return 0;
        }
        payloadLen = ((uint32_t)pMsg[2] << 24) | ((uint32_t)pMsg[3] << 16) |
                     ((uint32_t)pMsg[4] << 8) | (uint32_t)pMsg[5];
        pRec = &pMsg[6];
    }

    if(((uint16_t)(pRec - pMsg) + typeLen) > msgLen)
    {
        return 0;
    }
    if(typeLen != 1 || pRec[0] != NFC_NDEF_TYPE_TEXT)
    {
        return 0;   /* not a text record */
    }
    pRec += typeLen;

    if(payloadLen < 1 || ((uint16_t)(pRec - pMsg) + 1) > msgLen)
    {
        return 0;
    }
    langLen = pRec[0] & 0x3F;
    if(((uint32_t)langLen + 1) > payloadLen)
    {
        return 0;
    }

    textLen = (uint16_t)(payloadLen - 1 - langLen);
    if(((uint16_t)(pRec - pMsg) + 1 + langLen + textLen) > msgLen)
    {
        textLen = (uint16_t)(msgLen - (uint16_t)(pRec - pMsg) - 1 - langLen);
    }
    if(textLen > MESH_NFC_TEXT_MAX)
    {
        textLen = MESH_NFC_TEXT_MAX;
    }

    tmos_memcpy(nfcTextBuf, &pRec[1 + langLen], textLen);
    nfcTextBuf[textLen] = 0;

    return textLen;
}

/*********************************************************************
 * @fn      nfcWriteTextTag
 *
 * @brief   Write the pending text into the selected tag as an NDEF text
 *          record.  A text that does not fit into the data area of the tag
 *          is truncated.
 *
 * @param   len     - length of the pending text
 * @param   written - number of text bytes actually written
 *
 * @return  1 - written, 0 - failed / tag is not NDEF formatted
 */
static uint8_t nfcWriteTextTag(uint16_t len, uint16_t *written)
{
    uint8_t  cc[16];
    uint16_t dataSize;
    uint16_t maxText;
    uint16_t total;
    uint16_t pages;
    uint16_t i;

    *written = 0;

    if(PcdNTAG21xRead(NFC_CC_PAGE, cc) != PCD_NO_ERROR)
    {
        return 0;
    }
    if(cc[0] != NFC_CC_MAGIC)
    {
        PRINT("nfc tag has no NDEF capability container, skip write\n");
        return 0;
    }

    dataSize = (uint16_t)cc[2] * 8;
    if(dataSize < 32)
    {
        dataSize = NFC_DEFAULT_DATA_SIZE;
    }
    if(dataSize > NFC_WRITE_BUF_SIZE)
    {
        dataSize = NFC_WRITE_BUF_SIZE;
    }

    /* truncate the text so that the TLV fits into the data area of the tag */
    maxText = (uint16_t)(dataSize - NFC_TEXT_OVERHEAD);
    if(maxText > MESH_NFC_TEXT_MAX)
    {
        maxText = MESH_NFC_TEXT_MAX;
    }
    if(len > maxText)
    {
        len = maxText;
    }

    nfcWriteBuf[0] = NFC_TLV_NDEF;
    nfcWriteBuf[1] = (uint8_t)(7 + len);
    nfcWriteBuf[2] = 0xD1;                  /* MB | ME | SR | TNF well known */
    nfcWriteBuf[3] = 1;
    nfcWriteBuf[4] = (uint8_t)(3 + len);    /* status + "en" + text */
    nfcWriteBuf[5] = NFC_NDEF_TYPE_TEXT;
    nfcWriteBuf[6] = NFC_NDEF_LANG_EN;
    nfcWriteBuf[7] = 'e';
    nfcWriteBuf[8] = 'n';
    if(len != 0)
    {
        tmos_memcpy(&nfcWriteBuf[9], nfcWriteText, len);
    }
    nfcWriteBuf[9 + len] = NFC_TLV_TERMINATOR;

    total = (uint16_t)(NFC_TEXT_OVERHEAD + len);
    pages = (uint16_t)((total + 3) / 4);
    tmos_memset(&nfcWriteBuf[total], 0, (pages * 4) - total);

    /* Write the record page by page.  A single page WRITE (command + address
     * + 4 bytes in one frame) is answered by the tag and is the form every
     * NTAG21x works with.  The 4 page compatibility WRITE is deliberately not
     * used: some tags never answer its first step and the reader side only
     * gets a timeout, which then also breaks the following commands. */
    for(i = 0; i < pages; i++)
    {
        uint16_t res = PcdNTAG21xWrite((uint8_t)(NFC_DATA_PAGE + i), &nfcWriteBuf[i * 4]);

        if(res != PCD_NO_ERROR)
        {
            PRINT("nfc write failed at page %d (write res 0x%04x)\n", NFC_DATA_PAGE + i, res);
            return 0;
        }
    }

    /* Verify by reading the first data block back: the tag may ACK a write and
     * still drop it when the field is too weak to program the EEPROM. */
    {
        uint8_t  verify[16];
        uint16_t cmpLen = (total >= sizeof(verify)) ? (uint16_t)sizeof(verify) : total;

        if(PcdNTAG21xRead(NFC_DATA_PAGE, verify) != PCD_NO_ERROR)
        {
            PRINT("nfc write verify read failed\n");
            return 0;
        }
        if(tmos_memcmp(verify, nfcWriteBuf, cmpLen) == 0)
        {
            PRINT("nfc write verify mismatch\n");
            return 0;
        }
    }

    *written = len;

    return 1;
}

/*********************************************************************
 * @fn      nfcCardMiss
 *
 * @brief   Count a poll that found no card.  After NFC_CARD_GONE_CNT misses
 *          in a row the card is considered removed, so the next card placed
 *          on the antenna is handled (and reported) once more.
 *
 * @return  none
 */
static void nfcCardMiss(void)
{
    if(nfcReported)
    {
        if(++nfcMissCnt >= NFC_CARD_GONE_CNT)
        {
            nfcReported = 0;
            nfcMissCnt = 0;
        }
    }
}

#if NFC_WRITE_TEST
/*********************************************************************
 * @fn      nfcWriteTest
 *
 * @brief   NFC 写卡自测：检测到卡片就把 NFC_WRITE_TEST_TEXT 写成 NDEF
 *          文本记录。写失败会原地一直重试（日志会指出失败在哪一层），
 *          写成功后停手；把卡片拿开再放回来可以重测。
 *
 * @return  none
 */
static void nfcWriteTest(void)
{
    uint8_t uid[MESH_UID_LEN];

    tmos_memset(uid, 0, sizeof(uid));

    nfca_pcd_start();

    if(nfca_pcd_lpcd_check() != 0)
    {
        /* 手机等模拟卡设备需要一段连续波才会切到卡模式 */
        mDelaymS(2);

        if(nfcSelectCard(uid) != 0)
        {
            nfcMissCnt = 0;

            if(!nfcTestDone)
            {
                uint16_t written = 0;
                uint8_t  page[16];

                PRINT("nfc write test: uid %02x %02x %02x %02x %02x %02x %02x, writing \"%s\"\n",
                      uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], NFC_WRITE_TEST_TEXT);

                /* Ask the card what it is: NTAG213/215/216 answer GET_VERSION,
                 * other 14443A cards do not. */
                {
                    ntag_21x_get_version_res_t ver;

                    tmos_memset(&ver, 0, sizeof(ver));

                    if(PcdNTAG21xGetVersion(&ver) == PCD_NO_ERROR)
                    {
                        PRINT("nfc write test: GET_VERSION type %02x/%02x ver %d.%d storage %02x\n",
                              ver.product_type, ver.product_subtype,
                              ver.major_product_version, ver.minor_product_version, ver.storage_size);
                    }
                    else
                    {
                        PRINT("nfc write test: GET_VERSION failed, card may not be an NTAG21x\n");
                    }
                }

                if(PcdNTAG21xRead(NFC_CC_PAGE, page) == PCD_NO_ERROR)
                {
                    PRINT("nfc write test: CC page %02x %02x %02x %02x\n",
                          page[0], page[1], page[2], page[3]);
                }

                /* Probe a single page WRITE before the 4 page write: page 4 is
                 * read first and the very same bytes are written back, so the
                 * tag content does not change.  This separates "WRITE works,
                 * the 4 page write does not" from "no write works at all". */
                {
                    uint16_t res = PcdNTAG21xRead(NFC_DATA_PAGE, page);

                    PRINT("nfc write test: page4 read res 0x%04x\n", res);

                    if(res == PCD_NO_ERROR)
                    {
                        PRINT("nfc write test: page4 before %02x %02x %02x %02x\n",
                              page[0], page[1], page[2], page[3]);

                        res = PcdNTAG21xWrite(NFC_DATA_PAGE, page);
                        PRINT("nfc write test: single page write res 0x%04x\n", res);
                    }
                }

                if(nfcWriteTextTag(nfcWriteLen, &written))
                {
                    nfcTestDone = 1;
                    PRINT("nfc write test OK: \"%s\" written(%d bytes)\n", NFC_WRITE_TEST_TEXT, written);
                    PRINT("nfc write test: take the card away and put it back to test again\n");
                }
                else
                {
                    /* Read the first data page again: after a failed write the
                     * tag still holds whatever it had before. */
                    {
                        uint16_t res = PcdNTAG21xRead(NFC_DATA_PAGE, page);

                        PRINT("nfc write test: page4 reread res 0x%04x\n", res);

                        if(res == PCD_NO_ERROR)
                        {
                            PRINT("nfc write test: page4 after %02x %02x %02x %02x\n",
                                  page[0], page[1], page[2], page[3]);
                        }
                    }
                    PRINT("nfc write test: failed, retrying while the card stays\n");
                }
            }

            PcdHalt();
        }
    }
    else if(nfcTestDone)
    {
        /* 卡片已离开，复位后可以再测一次 */
        if(++nfcMissCnt >= NFC_CARD_GONE_CNT)
        {
            nfcTestDone = 0;
            nfcMissCnt = 0;
            PRINT("nfc write test: card removed, ready for the next card\n");
        }
    }

    nfca_pcd_stop();
}
#endif

/*********************************************************************
 * @fn      nfcProcessEvent
 *
 * @brief   NFC polling task event processor.
 *
 * @return  events not processed
 */
static uint16_t nfcProcessEvent(uint8_t task_id, uint16_t events)
{
    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;
        if((pMsg = tmos_msg_receive(task_id)) != NULL)
        {
            tmos_msg_deallocate(pMsg);
        }
        return (events ^ SYS_EVENT_MSG);
    }

    #if NFC_WRITE_TEST
    if(events & NFC_POLL_EVT)
    {
        nfcWriteTest();
        tmos_start_task(nfcTaskId, NFC_POLL_EVT, MS1_TO_SYSTEM_TIME(NFC_POLL_PERIOD_MS));
        return (events ^ NFC_POLL_EVT);
    }

    return 0;
#else
    if(events & NFC_POLL_EVT)
    {
        uint8_t  uid[MESH_UID_LEN];
        uint16_t textLen;
        uint8_t  cardSeen = 0;
        static uint8_t nfcPollTick = 0;

        tmos_memset(uid, 0, sizeof(uid));

        nfca_pcd_start();

        if(nfca_pcd_lpcd_check() != 0)
        {
            /* a short continuous wave is needed to wake up card emulation
             * devices */
            mDelaymS(2);

            sysMark(0x12);
            if(nfcSelectCard(uid) != 0)
            {
                cardSeen = 1;
                nfcMissCnt = 0;

                /* Report the card when it appears, and while it stays on the
                 * antenna repeat the report every NFC_REPORT_REPEAT_MS so the
                 * upper computer keeps seeing it. */
                if(nfcShouldReport())
                {
                    nfcReported = 1;
                    nfcReportCnt++;
                    nfcLastReportTick = TMOS_GetSystemClock();

                    if(nfcWritePending)
                    {
                        uint16_t written = 0;

                        if(nfcWriteTextTag(nfcWriteLen, &written))
                        {
                            nfcWritePending = 0;
                            PRINT("nfc text written(%d)\n", written);
                            IND_NfcEvent(IND_NFC_WRITE);
                        }
                        else
                        {
                            PRINT("nfc text write failed\n");
                        }
                    }

                    sysMark(0x13);
                    textLen = nfcReadText();
                    MESH_ReportNfc(uid, nfcTextBuf, (uint8_t)textLen);
                    IND_NfcEvent(IND_NFC_READ);
                }
            }
            else
            {
                nfcCardMiss();
            }

            PcdHalt();
        }
        else
        {
            /* no card in the field at all */
            nfcCardMiss();
        }

        nfca_pcd_stop();

        /* Heartbeat every ~6 s: proves the poll loop is still running and tells
         * a dead reader apart from a report that is held back on purpose. */
        if(++nfcPollTick >= 20)
        {
            nfcPollTick = 0;
            PRINT("nfc poll alive: card %d miss %d reports %d\n",
                  cardSeen, nfcMissCnt, nfcReportCnt);
        }

        tmos_start_task(nfcTaskId, NFC_POLL_EVT, MS1_TO_SYSTEM_TIME(NFC_POLL_PERIOD_MS));
        return (events ^ NFC_POLL_EVT);
    }

    return 0;
#endif
}

/*********************************************************************
 * @fn      NFC_Init
 *
 * @brief   Init the NFC reader and start the polling task.
 *
 * @return  none
 */
void NFC_Init(void)
{
    nfcMissCnt = 0;
    nfcReported = 0;
    nfcWritePending = 0;
    nfcWriteLen = 0;

    nfcTaskId = TMOS_ProcessEventRegister(nfcProcessEvent);

#if NFC_WRITE_TEST
    nfcTestDone = 0;

    /* 待写入的文本先放进写缓冲，之后每张卡片都用它 */
    NFC_SetWriteText((const uint8_t *)NFC_WRITE_TEST_TEXT, sizeof(NFC_WRITE_TEST_TEXT) - 1);
#endif

    nfca_pcd_init();

    /*
     * CH584F/CH585F have PA9 internally shorted to PB9.  PA9 must also be
     * configured as analog input with the digital input disabled, otherwise
     * the NFC antenna signal is disturbed.
     */
    GPIOA_ModeCfg(GPIO_Pin_9, GPIO_ModeIN_Floating);
    R32_PIN_IN_DIS |= GPIO_Pin_9;

    nfca_pcd_set_recv_gain(NFCA_PCD_REC_GAIN_24DB);
    nfca_pcd_lpcd_calibration();
    PRINT("nfc reader ready\n");

#if NFC_WRITE_TEST
    PRINT("nfc write test mode diag2: put an NTAG213/215/216 tag on the antenna, \"%s\" will be written\n",
          NFC_WRITE_TEST_TEXT);
#endif

    tmos_start_task(nfcTaskId, NFC_POLL_EVT, MS1_TO_SYSTEM_TIME(NFC_POLL_PERIOD_MS));
}

/*********************************************************************
 * @fn      NFC_SetWriteText
 *
 * @brief   Arm the NDEF text to be written: the next card placed on the
 *          antenna is written with it, a text longer than the data area of
 *          the tag is truncated.  An empty text cancels a pending write.
 *
 * @param   text - text to write, len bytes
 * @param   len  - text length, 0 cancels the pending write
 *
 * @return  none
 */
void NFC_SetWriteText(const uint8_t *text, uint16_t len)
{
    if((len == 0) || (text == NULL))
    {
        nfcWritePending = 0;
        nfcWriteLen = 0;
        PRINT("nfc pending text cleared\n");
        return;
    }

    if(len > MESH_NFC_TEXT_MAX)
    {
        len = MESH_NFC_TEXT_MAX;
    }

    tmos_memcpy(nfcWriteText, (void *)text, len);
    nfcWriteText[len] = 0;
    nfcWriteLen = len;
    nfcWritePending = 1;

    PRINT("nfc pending text(%d): %s\n", nfcWriteLen, nfcWriteText);
}

/******************************** endfile @ app_nfc ****************************/
