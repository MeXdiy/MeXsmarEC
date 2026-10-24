/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_led.h
 * Description        : LED control hook for the BLE/NFC mesh node.
 *******************************************************************************/

#ifndef __APP_LED_H
#define __APP_LED_H

#include "CH58x_common.h"

/*
 * LED control hook.
 * Kept empty on purpose: the user provides the real LED driver body.
 * on = 0 : turn LED off
 * on != 0: turn LED on
 */
void LED_Ctrl(const uint8_t *payload);

#endif /* __APP_LED_H */
