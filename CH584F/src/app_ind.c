/********************************** (C) COPYRIGHT *******************************
 * File Name          : app_ind.c
 * Description        : Status indicator (IND_LED) for the BLE/NFC mesh node.
 *
 *                      Only the indicator pixel is driven here; the layout area
 *                      is left to the mesh LED command (LED_Ctrl in app_led.c).
 *                      A mesh LED command rewrites every pixel including index
 *                      0, so the indicator checks its pixel on every tick and
 *                      re-asserts its colour if it was overwritten.
 *******************************************************************************/

#include "CONFIG.h"
#include "CH58x_common.h"
#include "HAL.h"
#include "ws2812.h"
#include "app_ind.h"

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

#define IND_TICK_EVT    0x0001

static uint16_t indTaskId;

/* 提示类型 */
#define IND_HINT_NFC_READ     0
#define IND_HINT_NFC_WRITE    1
#define IND_HINT_BLE_ADV      2
#define IND_HINT_BLE_CONN     3

static uint8_t indBleState = IND_BLE_OFF;

static uint8_t  indHintKind;    /* IND_HINT_xxx, 仅提示期间有效            */
static uint16_t indHintTicks;   /* 非 0: 正在提示; 到 0 立刻放手           */
static uint16_t indTickCnt;     /* 自由计数, 用于闪烁相位                  */

/* 提示开始前索引 0 的内容(上位机指令写进去的), 提示结束后原样放回 */
static ws2812_color_t indBase = {0, 0, 0};
static uint8_t        indBaseValid;

/* 我们最后写进索引 0 的颜色: 提示结束时据此判断期间有没有人改写 */
static ws2812_color_t indShown = {0, 0, 0};

/* 1 = 上位机断开了: 索引 0 持续红闪, 直到上位机回来 */
static uint8_t indPcOffline;

/*********************************************************************
 * @fn      indApply
 *
 * @brief   Write the indicator colour to the strip.
 *
 * @param   color - wanted colour
 * @param   force - 1 to write even when it matches the last written colour
 *
 * @return  none
 */
static void indApply(const ws2812_color_t *color, uint8_t force)
{
    if(!force && color->r == indShown.r && color->g == indShown.g && color->b == indShown.b)
    {
        return;
    }

    indShown = *color;
    WS2812_SetPixel(IND_LED, color->r, color->g, color->b);

#if IND_OWN_BRIGHTNESS
    {
        /* The strip brightness is shared by all pixels and is applied when
         * Show() renders, so use the indicator's own level for this frame and
         * restore the previous one: a mesh command with brightness 1 or 2 must
         * not leave the indicator invisible. */
        uint8_t saved = WS2812_GetBrightness();

        WS2812_SetBrightness(IND_BRIGHTNESS);
        WS2812_Show();
        WS2812_SetBrightness(saved);
    }
#else
    WS2812_Show();
#endif
}

/*********************************************************************
 * @fn      indStartHint
 *
 * @brief   Start a transient hint.  Before the hint, pixel 0 carried
 *          whatever the upper computer's LED command wrote; that value is
 *          saved here and put back when the hint ends.
 *
 * @param   kind - IND_HINT_xxx
 *
 * @return  none
 */
static void indStartHint(uint8_t kind)
{
    uint8_t r, g, b;

    /* 已经在提示中就不要重新保存底图, 否则会把提示色当成底图存下来;
     * 上位机离线时更不能保存: 那时的像素是红闪报警色, 存下来会在上位机
     * 回来的瞬间变成一个"常亮红"(或熄灭)被放回灯带。 */
    if(indHintTicks == 0 && !indBaseValid && !indPcOffline)
    {
        if(WS2812_GetPixel(IND_LED, &r, &g, &b) == 0)
        {
            indBase.r    = r;
            indBase.g    = g;
            indBase.b    = b;
            indBaseValid = 1;
        }
    }

    indHintKind  = kind;
    indHintTicks = (uint16_t)(IND_HINT_MS / IND_TICK_MS);
    indTickCnt   = 0;
}

/*********************************************************************
 * @fn      IND_AlarmPhaseCB
 *
 * @brief   Weak default: nothing to do until a project overrides it.
 *
 * @param   alarmOn - 1 while the red phase is on
 *
 * @return  none
 */
__attribute__((weak)) void IND_AlarmPhaseCB(uint8_t alarmOn)
{
    (void)alarmOn;
}

/*********************************************************************
 * @fn      indUpdate
 *
 * @brief   Decide what the indicator must show right now and apply it.
 *
 * @return  none
 */
static void indUpdate(void)
{
    ws2812_color_t color = {IND_COLOR_OFF};
    uint16_t       half;
    uint8_t        r, g, b;

    indTickCnt++;

    /* 报警(上位机离线红闪)的相位对外可见: 上层可据此联动, 例如让蓝牙广播
     * 跟着红灯节奏开关。相位与下面真正点亮索引 0 的那一相同源。 */
    half = IND_PC_BLINK_MS / IND_TICK_MS;
    if(half == 0)
    {
        half = 1;
    }
    IND_AlarmPhaseCB((indPcOffline && (((indTickCnt / half) & 1) == 0)) ? 1 : 0);

    /* 不提示时: 若上位机断开则持续红闪, 否则完全不碰索引 0(灯带按上位机指令) */
    if(indHintTicks == 0)
    {
        if(indPcOffline)
        {
            half = IND_PC_BLINK_MS / IND_TICK_MS;
            if(half == 0)
            {
                half = 1;
            }

            if((indTickCnt / half) & 1)
            {
                color.r = 0;
                color.g = 0;
                color.b = 0;
            }
            else
            {
                ws2812_color_t c = {IND_COLOR_PC_LOST};
                color            = c;
            }

            indApply(&color, 1);
        }

        return;
    }

    indHintTicks--;

    switch(indHintKind)
    {
        case IND_HINT_NFC_WRITE:
        case IND_HINT_NFC_READ:
        {
            half = IND_NFC_BLINK_MS / IND_TICK_MS;
            if(half == 0)
            {
                half = 1;
            }

            if((indHintTicks / half) & 1)
            {
                color.r = 0;
                color.g = 0;
                color.b = 0;
            }
            else if(indHintKind == IND_HINT_NFC_WRITE)
            {
                ws2812_color_t c = {IND_COLOR_NFC_WRITE};
                color            = c;
            }
            else
            {
                ws2812_color_t c = {IND_COLOR_NFC_READ};
                color            = c;
            }
        }
        break;

        case IND_HINT_BLE_ADV:
            half = IND_BLE_ADV_BLINK_MS / IND_TICK_MS;
            if(half == 0)
            {
                half = 1;
            }

            if((indHintTicks / half) & 1)
            {
                color.r = 0;
                color.g = 0;
                color.b = 0;
            }
            else
            {
                ws2812_color_t c = {IND_COLOR_BLE_ADV};
                color            = c;
            }
            break;

        case IND_HINT_BLE_CONN:
        {
            ws2812_color_t c = {IND_COLOR_BLE_CONN};
            color            = c;
        }
        break;

        default:
            break;
    }

    indApply(&color, 1);

    if(indHintTicks == 0)
    {
        if(indPcOffline)
        {
            /* 上位机断开期间提示结束: 底图必须留着 —— 上位机回来时还要放回。
             * 以前这里无条件清掉底图, 回来时无处可还原, 索引 0 就停在报警红
             * 上变成"常亮", 停在哪一相随机, 于是表现为"有时这块板有时那块"。 */
        }
        else
        {
            /* 提示结束: 若期间没人改写索引 0, 就把上位机原来的内容放回去。 */
            if(indBaseValid &&
               WS2812_GetPixel(IND_LED, &r, &g, &b) == 0 &&
               r == indShown.r && g == indShown.g && b == indShown.b)
            {
                indApply(&indBase, 1);
            }
            indBaseValid = 0;
        }

        PRINT("ind: hint done\n");
    }
}

/*********************************************************************
 * @fn      indProcessEvent
 *
 * @brief   Indicator refresh task.
 *
 * @return  events not processed
 */
static uint16_t indProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;

    if(events & IND_TICK_EVT)
    {
        indUpdate();

        tmos_start_task(indTaskId, IND_TICK_EVT, MS1_TO_SYSTEM_TIME(IND_TICK_MS));
        return (events ^ IND_TICK_EVT);
    }

    return 0;
}

/*********************************************************************
 * @fn      IND_SetBleState
 *
 * @brief   Report the peripheral role state to the indicator.
 *
 * @param   state - IND_BLE_OFF / IND_BLE_ADV / IND_BLE_CONN
 *
 * @return  none
 */
void IND_SetBleState(uint8_t state)
{
    if(indBleState == state)
    {
        return;
    }

    indBleState = state;

    switch(state)
    {
        case IND_BLE_CONN:
            indStartHint(IND_HINT_BLE_CONN);
            PRINT("ind: ble conn %ds\n", (int)(IND_HINT_MS / 1000));
            break;

        case IND_BLE_ADV:
            indStartHint(IND_HINT_BLE_ADV);
            PRINT("ind: ble adv %ds\n", (int)(IND_HINT_MS / 1000));
            break;

        default:
            PRINT("ind: ble off\n");
            break;
    }
}

/*********************************************************************
 * @fn      IND_SetPcOnline
 *
 * @brief   Report whether the upper computer is reachable.  While it is
 *          not, pixel 0 blinks red; when it comes back the content that
 *          was there before is put back.
 *
 * @param   online - 1: upper computer reached, 0: lost
 *
 * @return  none
 */
void IND_SetPcOnline(uint8_t online)
{
    uint8_t r, g, b;

    if(online)
    {
        if(!indPcOffline)
        {
            return;
        }

        indPcOffline = 0;

        /* 上位机回来: 放回断开前那份内容 */
        if(indBaseValid)
        {
            indApply(&indBase, 1);
            indBaseValid = 0;
        }

        PRINT("ind: pc back\n");
        return;
    }

    if(indPcOffline)
    {
        return;
    }

    /* 断开: 先记下现在索引 0 是什么(上位机指令的), 回来时放回 */
    if(!indBaseValid)
    {
        if(WS2812_GetPixel(IND_LED, &r, &g, &b) == 0)
        {
            indBase.r    = r;
            indBase.g    = g;
            indBase.b    = b;
            indBaseValid = 1;
        }
    }

    indPcOffline = 1;
    PRINT("ind: pc lost\n");
}

/*********************************************************************
 * @fn      IND_SyncBlink
 *
 * @brief   Align the blink phase with the mesh heartbeat.  Every board
 *          resets on the same broadcast, and the heartbeat period is a whole
 *          multiple of the blink half period, so the boards keep blinking in
 *          step and a missed frame does not shift the phase either.
 *
 * @return  none
 */
void IND_SyncBlink(void)
{
    indTickCnt = 0;
}

/*********************************************************************
 * @fn      IND_NfcEvent
 *
 * @brief   Flash the indicator in the colour of the given NFC event, then
 *          hand the pixel back to the upper computer's LED command.
 *
 * @param   kind - IND_NFC_READ / IND_NFC_WRITE
 *
 * @return  none
 */
void IND_NfcEvent(uint8_t kind)
{
    /* 写卡成功后紧接着同一轮轮询里还会读卡上报: 写卡提示优先, 否则橙色会被
     * 几十毫秒后的青色顶掉, 看上去就像"写入没有闪烁"。 */
    if(kind == IND_NFC_READ && indHintTicks && indHintKind == IND_HINT_NFC_WRITE)
    {
        return;
    }

    indStartHint((kind == IND_NFC_WRITE) ? IND_HINT_NFC_WRITE : IND_HINT_NFC_READ);
    PRINT("ind: nfc %s %ds\n", (kind == IND_NFC_WRITE) ? "write" : "read", (int)(IND_HINT_MS / 1000));
}

/*********************************************************************
 * @fn      IND_Init
 *
 * @brief   Start the refresh task.  Pixel 0 is left alone until the first
 *          hint, so the strip keeps showing the upper computer's command.
 *
 * @return  none
 */
void IND_Init(void)
{
    indTaskId    = TMOS_ProcessEventRegister(indProcessEvent);
    indBleState  = IND_BLE_OFF;
    indHintTicks = 0;
    indTickCnt   = 0;
    indBaseValid = 0;
    indBase.r    = 0;
    indBase.g    = 0;
    indBase.b    = 0;
    indShown.r   = 0;
    indShown.g   = 0;
    indShown.b   = 0;

    tmos_start_task(indTaskId, IND_TICK_EVT, MS1_TO_SYSTEM_TIME(IND_TICK_MS));
}
