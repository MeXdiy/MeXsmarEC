/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_key.c
 * Description        : Two active-low buttons on PA15 / PB23, scanned by a
 *                      TMOS task with a debounce and one report per edge.
 *******************************************************************************/

#include "CONFIG.h"
#include "CH58x_common.h"
#include "HAL.h"
#include "app_key.h"
#include "ws2812.h"
/* 每个文件单独debug打印的开关，置0可以禁止本文件内部打印 */
#define DEBUG_PRINT_IN_THIS_FILE 1
#if DEBUG_PRINT_IN_THIS_FILE
#define PRINTF(...) PRINT(__VA_ARGS__)
#else
#define PRINTF(...) \
    do              \
    {               \
    } while (0)
#endif

#define KEY_SCAN_EVT 0x0001

static uint16_t keyTaskId;

/* Debounce state per key: the level that is currently accepted as the key
 * state (1 = released, 0 = pressed) and how many scans in a row have been
 * carrying a different level. */
static uint8_t keyLevel[KEY_COUNT];
static uint8_t keyEdgeCnt[KEY_COUNT];
/* How many scans in a row each key has been accepted as pressed, used by the
 * KEY2 long-press reboot. */
static uint16_t keyHoldCnt[KEY_COUNT];

/*********************************************************************
 * @fn      keyReadLevel
 *
 * @brief   Read the raw pin level of one key.
 *
 * @param   keyId - KEY_ID_1 / KEY_ID_2
 *
 * @return  1 - released (high), 0 - pressed (low)
 */
static uint8_t keyReadLevel(uint8_t keyId)
{
    if (keyId == KEY_ID_1)
    {
        return (GPIOA_ReadPortPin(KEY1_PIN) != 0) ? 1 : 0;
    }

    return (GPIOB_ReadPortPin(KEY2_PIN) != 0) ? 1 : 0;
}

/*********************************************************************
 * @fn      APP_KEY_OnEvent
 *
 * @brief   Weak hook so a project can attach an action to a button without
 *          touching the scan layer.
 *
 * @param   keyId   - KEY_ID_1 / KEY_ID_2
 * @param   pressed - 1 on press, 0 on release
 *
 * @return  none
 */
__attribute__((weak)) void APP_KEY_OnEvent(uint8_t keyId, uint8_t pressed)
{
    (void)keyId;
    (void)pressed;
    if (keyId == KEY_ID_1 && pressed == 1)
    {
        WS2812_SetAllAndShow(0, 0, 0);
    }
}

/*********************************************************************
 * @fn      keyProcessEvent
 *
 * @brief   Key scan task: sample both keys, debounce them and report the
 *          accepted press / release edges.
 *
 * @return  events not processed
 */
static uint16_t keyProcessEvent(uint8_t task_id, uint16_t events)
{
    uint8_t i;

    (void)task_id;

    if (events & KEY_SCAN_EVT)
    {
        for (i = 0; i < KEY_COUNT; i++)
        {
            uint8_t level = keyReadLevel(i);

            if (level == keyLevel[i])
            {
                /* Still the same level: nothing pending, nothing to report. */
                keyEdgeCnt[i] = 0;
                continue;
            }

            if (++keyEdgeCnt[i] < KEY_DEBOUNCE_CNT)
            {
                /* Too short to be a real press yet: keep the old level. */
                continue;
            }

            /* The new level held long enough to be accepted. */
            keyEdgeCnt[i] = 0;
            keyLevel[i]   = level;

            if (level == 0)
            {
                PRINT("key %d pressed\n", i + 1);
                APP_KEY_OnEvent(i, 1);
            }
            else
            {
                PRINT("key %d released\n", i + 1);
                APP_KEY_OnEvent(i, 0);
            }
        }

        /* Long-press action: holding KEY2 reboots the chip.  The counter works
         * on the accepted (debounced) level, so contact bounce cannot move it. */
        for (i = 0; i < KEY_COUNT; i++)
        {
            if (keyLevel[i] != 0)
            {
                keyHoldCnt[i] = 0;
                continue;
            }

            if (keyHoldCnt[i] < KEY_RESTART_TICKS)
            {
                keyHoldCnt[i]++;
            }

            if ((i == KEY_ID_2) && (keyHoldCnt[i] >= KEY_RESTART_TICKS))
            {
                keyHoldCnt[i] = 0;
                PRINT("key 2 hold %dms, reboot\n", KEY_RESTART_HOLD_MS);
                /* Let the debug UART drain before the chip resets. */
                mDelaymS(50);
                SYS_ResetExecute();
            }
        }

        tmos_start_task(keyTaskId, KEY_SCAN_EVT, MS1_TO_SYSTEM_TIME(KEY_SCAN_PERIOD_MS));
        return (events ^ KEY_SCAN_EVT);
    }

    return 0;
}

/*********************************************************************
 * @fn      APP_KEY_Init
 *
 * @brief   Configure the key pins as pulled-up inputs and start the scan task.
 *
 * @return  none
 */
void APP_KEY_Init(void)
{
    keyTaskId = TMOS_ProcessEventRegister(keyProcessEvent);

    /* Pressed = pin tied to GND, released = pulled up internally. */
    GPIOA_ModeCfg(KEY1_PIN, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(KEY2_PIN, GPIO_ModeIN_PU);

    keyEdgeCnt[KEY_ID_1] = 0;
    keyEdgeCnt[KEY_ID_2] = 0;

    /* Let the pull-up charge the line before believing the first reading,
     * otherwise a key that is already held at power-up is reported as an edge. */
    mDelaymS(5);
    keyLevel[KEY_ID_1] = keyReadLevel(KEY_ID_1);
    keyLevel[KEY_ID_2] = keyReadLevel(KEY_ID_2);

    PRINT("key init: PA15/PB23 active-low\n");

    tmos_start_task(keyTaskId, KEY_SCAN_EVT, MS1_TO_SYSTEM_TIME(KEY_SCAN_PERIOD_MS));
}
