/********************************** (C) COPYRIGHT *******************************
 * File Name          : Main.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2020/08/06
 * Description        : 串口1收发演示
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#include "CONFIG.h"
#include "CH58x_common.h"
#include "HAL.h"
#include "gattprofile.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#include "app_mesh.h"
#include "app_nfc.h"
#include "app_key.h"
#include "app_ind.h"
#include "app_led.h"
#include "ws2812.h"
#include "stdio.h"

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

void DebugInit(void)
{
#if DEBUG
    /* 配置串口1：先配置IO口模式，再配置串口 */
    GPIOB_SetBits(bTXD1_);
    GPIOB_ModeCfg(bTXD1_, GPIO_ModeOut_PP_5mA);
    GPIOB_ModeCfg(bRXD1_, GPIO_ModeIN_PU);
    GPIOPinRemap(ENABLE, RB_PIN_UART1);
    UART1_DefInit();
#endif
}

/*********************************************************************
 * @fn      main
 *
 * @brief   主函数
 *
 * @return  none
 */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/*********************************************************************
 * @fn      Main_Circulation
 *
 * @brief   Main loop, only runs the TMOS scheduler.
 *
 * @return  none
 */
__HIGH_CODE
__attribute__((noinline)) void Main_Circulation(void)
{
    while (1)
    {
        TMOS_SystemProcess();
    }
}

int main()
{
    HSECFG_Capacitance(HSECap_12p);
    LSECFG_Capacitance(LSECap_12p);
    SetSysClock(SYSCLK_FREQ);

    DebugInit();
    PRINT("MeXsmarEC\n");
    PRINT("Start @ChipID=%02X\n", R8_CHIP_ID);
    PRINT("%s\n", VER_LIB);

    CH58x_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();
    GAPRole_CentralInit();

    MESH_Init();
    NFC_Init();
    APP_KEY_Init();

    Peripheral_Init();
    Central_Init();

    WS2812_Init();
    WS2812_SetBrightness(30);
    {
        ws2812_layout_cfg_t layouts = {
            .mode   = WS2812_LAYOUT_LINE,
            .start  = 1,
            .width  = 100,
            .height = 1,
            .count  = 0,
        };
        WS2812_SetLayout(&layouts);
    }

    IND_Init();

    Main_Circulation();
}

/*********************************************************************
 * @fn      sysMark
 *
 * @brief   Record what the firmware is doing right now, so a fault report can
 *          say which path it was in.  Cheap enough to call from the hot paths.
 */
volatile uint32_t sysLastMarker = 0;

void sysMark(uint32_t marker)
{
    sysLastMarker = marker;
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   Overrides the weak handler in CH58x_sys.c, which only resets the
 *          chip: that makes an exception look like "the board died after a
 *          while" with nothing to go on.  Print the fault registers and the
 *          last marker first, then take the same reset path.
 */
__INTERRUPT
__HIGH_CODE
void HardFault_Handler(void)
{
    uint32_t mcause = __get_MCAUSE();
    uint32_t mepc   = __get_MEPC();
    uint32_t mtval  = __get_MTVAL();
    PRINT("FAULT mcause %x mepc %x mtval %x marker %x\n",
          (unsigned int)mcause, (unsigned int)mepc,
          (unsigned int)mtval, (unsigned int)sysLastMarker);
    mDelaymS(50);

    FLASH_ROM_SW_RESET();
    sys_safe_access_enable();
    R16_INT32K_TUNE = 0xFFFF;
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET;
    sys_safe_access_disable();
    while (1);
}
