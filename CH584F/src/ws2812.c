#include "ws2812.h"
#include <string.h>

/* SPI 波形编码 */
#define WS2812_GRB_0 0x80 /* 模拟 '0' 码 */
#define WS2812_GRB_1 0xE0 /* 模拟 '1' 码 */

/* 内部缓冲区 */
__attribute__((aligned(4))) static uint8_t s_rgb_raw[WS2812_LED_NUM * 3];     /* 原始 RGB */
__attribute__((aligned(4))) static uint8_t s_grb_scale[WS2812_LED_NUM * 3];   /* 亮度缩放后 GRB */
__attribute__((aligned(4))) static uint8_t s_spi_buf[WS2812_LED_NUM * 3 * 4]; /* 最终 SPI 波形 */

static uint8_t s_brightness = WS2812_BRIGHTNESS;

/* 布局静态变量 */
static ws2812_layout_cfg_t s_layout = {
    .mode   = WS2812_LAYOUT_LINE,
    .start  = 0,
    .width  = WS2812_LED_NUM,
    .height = 1,
    .count  = WS2812_LED_NUM,
    .flip_x = 0,
    .flip_y = 0,
};

/*--------------------------------------------------------------------
 * 把 GRB 字节流位展开为 SPI 波形
 * 每个 GRB 字节 -> 4 个 SPI 字节，每 SPI 字节承载 2 bit
 *------------------------------------------------------------------*/
static void ws2812_grb_2_spi(uint8_t *pdest, const uint8_t *psrc, uint16_t len)
{
    memset(pdest, 0, len * 3 * 4);
    for (uint16_t i = 0; i < len; i++)
    {
        for (uint8_t j = 0; j < 3; j++)
        {
            for (uint8_t k = 0; k < 4; k++)
            {
                for (uint8_t m = 0; m < 2; m++)
                {
                    if (psrc[3 * i + j] & (0x80 >> (2 * k + m)))
                    {
                        pdest[12 * i + 4 * j + k] |= (WS2812_GRB_1 >> (m * 4));
                    }
                    else
                    {
                        pdest[12 * i + 4 * j + k] |= (WS2812_GRB_0 >> (m * 4));
                    }
                }
            }
        }
    }
}

/*--------------------------------------------------------------------
 * 原始 RGB -> 亮度缩放 -> 重排成 GRB 顺序
 *------------------------------------------------------------------*/
static void ws2812_rgb_to_grb(void)
{
    for (uint16_t i = 0; i < WS2812_LED_NUM; i++)
    {
        uint8_t r              = s_rgb_raw[3 * i + 0];
        uint8_t g              = s_rgb_raw[3 * i + 1];
        uint8_t b              = s_rgb_raw[3 * i + 2];
        s_grb_scale[3 * i + 0] = (uint16_t)g * s_brightness / 255;
        s_grb_scale[3 * i + 1] = (uint16_t)r * s_brightness / 255;
        s_grb_scale[3 * i + 2] = (uint16_t)b * s_brightness / 255;
    }
}

/*--------------------------------------------------------------------
 * 初始化
 *------------------------------------------------------------------*/
void WS2812_Init(void)
{
    /* SPI0 引脚: PA13=CLK, PA14=MOSI (与原工程一致) */
    GPIOA_ResetBits(GPIO_Pin_14);
    GPIOA_ModeCfg(GPIO_Pin_13 | GPIO_Pin_14, GPIO_ModeOut_PP_5mA);
    SPI0_MasterDefInit();

    /* 60MHz 下分频 18，得到 3.33MHz SPI 时钟 */
    SPI0_CLKCfg(19);

    /* 先发一段低电平 (复位帧)，让所有灯进入已知状态 */
    memset(s_spi_buf, 0, 24);
    SPI0_MasterDMATrans(s_spi_buf, 24);

    /* 清空内部数据 */
    memset(s_rgb_raw, 0, sizeof(s_rgb_raw));
    memset(s_grb_scale, 0, sizeof(s_grb_scale));
    s_brightness = WS2812_BRIGHTNESS;

    /* 默认布局：全串当作 LINE, 从物理索引 0 开始 */
    s_layout.mode   = WS2812_LAYOUT_LINE;
    s_layout.start  = 0;
    s_layout.width  = WS2812_LED_NUM;
    s_layout.height = 1;
    s_layout.count  = WS2812_LED_NUM;
    s_layout.flip_x = 0;
    s_layout.flip_y = 0;
}

/*--------------------------------------------------------------------
 * 亮度
 *------------------------------------------------------------------*/
void WS2812_SetBrightness(uint8_t brightness)
{
    s_brightness = brightness;
}

uint8_t WS2812_GetBrightness(void)
{
    return s_brightness;
}

/*--------------------------------------------------------------------
 * 像素操作
 *------------------------------------------------------------------*/
void WS2812_SetPixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= WS2812_LED_NUM) return;
    s_rgb_raw[3 * index + 0] = r;
    s_rgb_raw[3 * index + 1] = g;
    s_rgb_raw[3 * index + 2] = b;
}

void WS2812_SetAll(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint16_t i = 0; i < WS2812_LED_NUM; i++)
    {
        s_rgb_raw[3 * i + 0] = r;
        s_rgb_raw[3 * i + 1] = g;
        s_rgb_raw[3 * i + 2] = b;
    }
}

void WS2812_SetColors(const ws2812_color_t *colors, uint16_t count)
{
    if (colors == NULL) return;
    uint16_t n = (count < WS2812_LED_NUM) ? count : WS2812_LED_NUM;

    for (uint16_t i = 0; i < n; i++)
    {
        s_rgb_raw[3 * i + 0] = colors[i].r;
        s_rgb_raw[3 * i + 1] = colors[i].g;
        s_rgb_raw[3 * i + 2] = colors[i].b;
    }
    /* 剩余未指定的灯珠熄灭 */
    for (uint16_t i = n; i < WS2812_LED_NUM; i++)
    {
        s_rgb_raw[3 * i + 0] = 0;
        s_rgb_raw[3 * i + 1] = 0;
        s_rgb_raw[3 * i + 2] = 0;
    }
}

void WS2812_Clear(void)
{
    memset(s_rgb_raw, 0, sizeof(s_rgb_raw));
}

/*--------------------------------------------------------------------
 * 设置布局
 *------------------------------------------------------------------*/
void WS2812_SetLayout(const ws2812_layout_cfg_t *cfg)
{
    if (cfg == NULL) return;

    /* 拷贝一份，并做合法性修正 */
    s_layout = *cfg;

    if (s_layout.width == 0) s_layout.width = 1;
    if (s_layout.height == 0) s_layout.height = 1;

    /* count 为 0 时自动 = width*height */
    if (s_layout.count == 0)
    {
        s_layout.count = (uint16_t)(s_layout.width * s_layout.height);
    }

    /* 截断：布局区不能越界到物理灯串之外 */
    if ((uint32_t)s_layout.start + s_layout.count > WS2812_LED_NUM)
    {
        s_layout.count = (s_layout.start < WS2812_LED_NUM) ? (uint16_t)(WS2812_LED_NUM - s_layout.start) : 0;
    }
}

const ws2812_layout_cfg_t *WS2812_GetLayout(void)
{
    return &s_layout;
}

/*--------------------------------------------------------------------
 * 逻辑坐标 → 物理索引
 *
 * 说明：
 *   - 坐标先应用镜像 (flip_x / flip_y)，再做蛇形/行列映射
 *   - LINE 模式：如果 height==1 用 x 当线性索引；
 *                如果 width ==1 用 y 当线性索引
 *   - 返回值 = start + offset，即物理灯串上的真实下标
 *------------------------------------------------------------------*/
int16_t WS2812_XYToIndex(uint16_t x, uint16_t y)
{
    if (x >= s_layout.width || y >= s_layout.height) return -1;

    /* 镜像：用户视角的镜像 */
    if (s_layout.flip_x) x = (uint16_t)(s_layout.width - 1 - x);
    if (s_layout.flip_y) y = (uint16_t)(s_layout.height - 1 - y);

    uint32_t offset = 0;

    switch (s_layout.mode)
    {
    case WS2812_LAYOUT_LINE:
        offset = (s_layout.height <= 1) ? x : y;
        break;

    case WS2812_LAYOUT_MATRIX_ROW_MAJOR:
        offset = (uint32_t)y * s_layout.width + x;
        break;

    case WS2812_LAYOUT_MATRIX_COL_MAJOR:
        offset = (uint32_t)x * s_layout.height + y;
        break;

    case WS2812_LAYOUT_MATRIX_ROW_SERPENTINE:
        /* 偶数行从左往右, 奇数行从右往左 */
        offset = (uint32_t)y * s_layout.width
                 + ((y & 1u) ? (s_layout.width - 1 - x) : x);
        break;

    case WS2812_LAYOUT_MATRIX_COL_SERPENTINE:
        /* 偶数列从上往下, 奇数列从下往上 */
        offset = (uint32_t)x * s_layout.height
                 + ((x & 1u) ? (s_layout.height - 1 - y) : y);
        break;

    default:
        return -1;
    }

    if (offset >= s_layout.count) return -1;
    return (int16_t)(s_layout.start + offset);
}

/*--------------------------------------------------------------------
 * 逻辑坐标设置像素
 *------------------------------------------------------------------*/
void WS2812_SetPixelXY(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    int16_t idx = WS2812_XYToIndex(x, y);
    if (idx < 0) return;
    WS2812_SetPixel((uint16_t)idx, r, g, b);
}

/*--------------------------------------------------------------------
 * 只控制布局区
 *------------------------------------------------------------------*/
void WS2812_SetLayoutAll(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t end = (uint16_t)(s_layout.start + s_layout.count);
    for (uint16_t i = s_layout.start; i < end; i++)
    {
        WS2812_SetPixel(i, r, g, b);
    }
}

void WS2812_ClearLayout(void)
{
    WS2812_SetLayoutAll(0, 0, 0);
}

/*--------------------------------------------------------------------
 * 刷新输出
 *------------------------------------------------------------------*/
void WS2812_Show(void)
{
    ws2812_rgb_to_grb();
    ws2812_grb_2_spi(s_spi_buf, s_grb_scale, WS2812_LED_NUM);
    SPI0_MasterDMATrans(s_spi_buf, WS2812_LED_NUM * 3 * 4);
    /* 注: DMA 为异步启动，连续快速调用 Show() 之间请保证足够延时，
     *     或自行加入 SPI 完成标志判断 (原工程依靠 DelayMs 满足) */
}

void WS2812_SetAllAndShow(uint8_t r, uint8_t g, uint8_t b)
{
    WS2812_SetAll(r, g, b);
    WS2812_Show();
}

/*--------------------------------------------------------------------
 * 查询
 *------------------------------------------------------------------*/
uint16_t WS2812_GetLedNum(void)
{
    return WS2812_LED_NUM;
}

/*--------------------------------------------------------------------
 * 状态读取 —— 原始 RGB
 *------------------------------------------------------------------*/
int WS2812_GetPixel(uint16_t phys_index,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (phys_index >= WS2812_LED_NUM) return -1;
    if (r) *r = s_rgb_raw[3 * phys_index + 0];
    if (g) *g = s_rgb_raw[3 * phys_index + 1];
    if (b) *b = s_rgb_raw[3 * phys_index + 2];
    return 0;
}

int WS2812_GetPixelXY(uint16_t x, uint16_t y,
                      uint8_t *r, uint8_t *g, uint8_t *b)
{
    int16_t idx = WS2812_XYToIndex(x, y);
    if (idx < 0) return -1;
    return WS2812_GetPixel((uint16_t)idx, r, g, b);
}

/*--------------------------------------------------------------------
 * 状态读取 —— 亮度缩放后的实际输出值
 *
 * s_grb_scale 内的排列是 GRB (物理顺序 = WS2812 接收顺序)
 * 这里转回 RGB 给调用者
 *------------------------------------------------------------------*/
int WS2812_GetPixelScaled(uint16_t phys_index,
                          uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (phys_index >= WS2812_LED_NUM) return -1;
    uint8_t gg = s_grb_scale[3 * phys_index + 0];
    uint8_t rr = s_grb_scale[3 * phys_index + 1];
    uint8_t bb = s_grb_scale[3 * phys_index + 2];
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
    return 0;
}

int WS2812_GetPixelXYScaled(uint16_t x, uint16_t y,
                            uint8_t *r, uint8_t *g, uint8_t *b)
{
    int16_t idx = WS2812_XYToIndex(x, y);
    if (idx < 0) return -1;
    return WS2812_GetPixelScaled((uint16_t)idx, r, g, b);
}

/*--------------------------------------------------------------------
 * 原始缓冲只读指针
 *------------------------------------------------------------------*/
const uint8_t *WS2812_GetRawBuffer(void)
{
    return s_rgb_raw;
}

/*--------------------------------------------------------------------
 * 便利判断
 *------------------------------------------------------------------*/
int WS2812_IsPixelOn(uint16_t phys_index)
{
    if (phys_index >= WS2812_LED_NUM) return -1;
    return (s_rgb_raw[3 * phys_index + 0] || s_rgb_raw[3 * phys_index + 1] || s_rgb_raw[3 * phys_index + 2]) ? 1 : 0;
}

int WS2812_IsPixelXYOn(uint16_t x, uint16_t y)
{
    int16_t idx = WS2812_XYToIndex(x, y);
    if (idx < 0) return -1;
    return WS2812_IsPixelOn((uint16_t)idx);
}

/*--------------------------------------------------------------------
 * 结构体风格
 *------------------------------------------------------------------*/
int WS2812_GetColor(uint16_t phys_index, ws2812_color_t *out)
{
    if (out == NULL) return -1;
    return WS2812_GetPixel(phys_index, &out->r, &out->g, &out->b);
}

int WS2812_GetColorXY(uint16_t x, uint16_t y, ws2812_color_t *out)
{
    if (out == NULL) return -1;
    return WS2812_GetPixelXY(x, y, &out->r, &out->g, &out->b);
}