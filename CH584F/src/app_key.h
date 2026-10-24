/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_key.h
 * Description        : Two active-low buttons on PA15 / PB23
 *******************************************************************************/

#ifndef __APP_KEY_H__
#define __APP_KEY_H__

#include "CONFIG.h"
#include "CH58x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Both buttons are wired to GND, so a pressed key reads 0.  The internal
 * pull-up keeps the line high while the key is released. */
#define KEY1_PIN                GPIO_Pin_15    /* PA15 */
#define KEY2_PIN                GPIO_Pin_22    /* PB22 */

#define KEY_COUNT               2

/* Poll the keys every 10 ms and accept a level only after 3 identical
 * readings in a row, which is 30 ms of contact bounce. */
#define KEY_SCAN_PERIOD_MS      10
#define KEY_DEBOUNCE_CNT        3

/* Hold KEY2 for this long to reboot the chip.  The scan period turns the time
 * into the number of scans that have to stay pressed in a row. */
#define KEY_RESTART_HOLD_MS     3000
#define KEY_RESTART_TICKS       (KEY_RESTART_HOLD_MS / KEY_SCAN_PERIOD_MS)

typedef enum
{
    KEY_ID_1 = 0,
    KEY_ID_2 = 1,
} keyId_t;

/*********************************************************************
 * @fn      APP_KEY_Init
 *
 * @brief   Configure the key pins as pulled-up inputs and start the scan.
 *
 * @return  none
 */
void APP_KEY_Init(void);

/*********************************************************************
 * @fn      APP_KEY_OnEvent
 *
 * @brief   Called on every key edge, defined weak so a project can override
 *          it to attach an action to a button.
 *
 * @param   keyId   - KEY_ID_1 / KEY_ID_2
 * @param   pressed - 1 on press, 0 on release
 *
 * @return  none
 */
void APP_KEY_OnEvent(uint8_t keyId, uint8_t pressed);

#ifdef __cplusplus
}
#endif

#endif /* __APP_KEY_H__ */
