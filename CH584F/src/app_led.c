/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_led.c
 * Description        : LED control hook for the BLE/NFC mesh node.
 *******************************************************************************/

#include "app_led.h"
#include "ws2812.h"

/*********************************************************************
 * @fn      LED_Ctrl
 *
 * @brief   根据 36 字节的 Payload 批量控制 LED
 *
 * @param   payload - 指向帧中 data 起始位置的指针
 *                    payload[0]   = 亮度 (0~255)
 *                    payload[1]   = 红色 (0~255)
 *                    payload[2]   = 绿色 (0~255)
 *                    payload[3]   = 蓝色 (0~255)
 *                    payload[4..35] = 256 个 LED 的位图 (Bitmap)
 *
 * @return  none
 */
void LED_Ctrl(const uint8_t *payload)
{
    uint8_t brightness = payload[0];
    uint8_t r          = payload[1];
    uint8_t g          = payload[2];
    uint8_t b          = payload[3];

    PRINT("--- LED_Ctrl Bitmap Received ---\n");
    PRINT("Brightness: %d, RGB: (%d, %d, %d)\n", brightness, r, g, b);
    PRINT("Bitmap (32 bytes):\n");
    
    for (int i = 0; i < 32; i++) {
        PRINT("%02X ", payload[4 + i]);
        if ((i + 1) % 8 == 0) PRINT("\n"); // 每 8 字节换行
    }
    
    // 1. 设置全局亮度
    WS2812_SetBrightness(brightness);

    // 2. 遍历 32 字节的位图
    for (int byte_idx = 0; byte_idx < 32; byte_idx++)
    {
        uint8_t mask = payload[4 + byte_idx];
        for (int bit = 0; bit < 8; bit++)
        {
            int led_index = byte_idx * 8 + bit;
            
            // 位为 1，点亮对应的颜色；位为 0，熄灭
            if (mask & (1 << bit)) {
                WS2812_SetPixel(led_index, r, g, b);
            } else {
                WS2812_SetPixel(led_index, 0, 0, 0);
            }
        }
    }

    // 3. 统一刷新显示 (所有LED同时亮起，无先后闪烁)
    WS2812_Show();
}