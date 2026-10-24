/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_nfc.h
 * Description        : NFC-A reader (PCD) task, reports card UID into the mesh.
 *******************************************************************************/

#ifndef __APP_NFC_H
#define __APP_NFC_H

#include "CH58x_common.h"

/*
 * @brief  Init the NFC reader: pin config, library init, LPCD calibration
 *         and the periodic polling task.
 */
void NFC_Init(void);

/*
 * @brief  Arm the NDEF text to be written: the next card placed on the
 *         antenna is written with it.  An empty text cancels a pending write.
 * @param  text - text to write, len bytes
 * @param  len  - text length, 0 cancels the pending write
 */
void NFC_SetWriteText(const uint8_t *text, uint16_t len);

#endif /* __APP_NFC_H */
