#ifndef __WS2812_H__
#define __WS2812_H__

#include "CH58x_common.h"

/*====================== 用户配置区 ======================*/
#ifndef WS2812_LED_NUM
#define WS2812_LED_NUM 101 /* 灯珠总数（指示灯 + 布局区） */
#endif

#define IND_LED 0

#ifndef WS2812_BRIGHTNESS
#define WS2812_BRIGHTNESS 30 /* 默认全局亮度 (0~255) */
#endif
/*=======================================================*/

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ws2812_color_t;

/*---------- 布局模式 ----------*/
typedef enum
{
    WS2812_LAYOUT_LINE = 0,              /* 直线 (一串)            */
    WS2812_LAYOUT_MATRIX_ROW_MAJOR,      /* 方阵: 行优先, 不蛇形   */
    WS2812_LAYOUT_MATRIX_COL_MAJOR,      /* 方阵: 列优先, 不蛇形   */
    WS2812_LAYOUT_MATRIX_ROW_SERPENTINE, /* 方阵: 行优先, 蛇形     */
    WS2812_LAYOUT_MATRIX_COL_SERPENTINE, /* 方阵: 列优先, 蛇形     */
} ws2812_layout_t;

/*---------- 布局配置 ----------*/
typedef struct
{
    ws2812_layout_t mode;
    uint16_t        start;  /* 布局区在物理灯串中的起始索引               */
    uint16_t        width;  /* 逻辑宽 (列数)                              */
    uint16_t        height; /* 逻辑高 (行数), LINE 模式下填 1             */
    uint16_t        count;  /* 布局区灯珠数, 填 0 表示自动 = width*height */
    uint8_t         flip_x; /* 水平镜像 (0=关, 1=开)                      */
    uint8_t         flip_y; /* 垂直镜像 (0=关, 1=开)                      */
} ws2812_layout_cfg_t;

/*==================== 生命周期 ====================*/
void WS2812_Init(void);

/*==================== 参数配置 ====================*/
void    WS2812_SetBrightness(uint8_t brightness);
uint8_t WS2812_GetBrightness(void);

/*==================== 布局系统 ====================*/
void                       WS2812_SetLayout(const ws2812_layout_cfg_t *cfg);
const ws2812_layout_cfg_t *WS2812_GetLayout(void);

/* 逻辑坐标 (x,y) → 物理索引; 越界返回 -1 */
int16_t WS2812_XYToIndex(uint16_t x, uint16_t y);

/*==================== 数据操作 ====================*/
/* ---- 用物理索引（指示灯、或不想走布局的场合） ---- */
void WS2812_SetPixel(uint16_t phys_index, uint8_t r, uint8_t g, uint8_t b);

/* ---- 用逻辑坐标（布局区内部像素） ---- */
void WS2812_SetPixelXY(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b);

/* ---- 整体 ---- */
void WS2812_SetAll(uint8_t r, uint8_t g, uint8_t b);       /* 所有物理灯珠 */
void WS2812_SetLayoutAll(uint8_t r, uint8_t g, uint8_t b); /* 只布局区     */
void WS2812_Clear(void);                                   /* 全部熄灭     */
void WS2812_ClearLayout(void);                             /* 只清布局区   */

/* ---- 批量设置（按物理顺序） ---- */
void WS2812_SetColors(const ws2812_color_t *colors, uint16_t count);

/*==================== 刷新输出 ====================*/
void WS2812_Show(void);
void WS2812_SetAllAndShow(uint8_t r, uint8_t g, uint8_t b);

/*==================== 信息查询 ====================*/
uint16_t WS2812_GetLedNum(void);

/*==================== 状态读取 ====================*/

/* 通过物理索引读取 —— 返回原始 RGB (未经亮度缩放)
 * 成功返回 0，索引越界返回 -1
 * 参数可为 NULL 表示不关心该分量
 */
int WS2812_GetPixel(uint16_t phys_index, uint8_t *r, uint8_t *g, uint8_t *b);

/* 通过逻辑坐标读取 —— 返回原始 RGB
 * 成功返回 0，坐标越界 / 不在布局区返回 -1
 */
int WS2812_GetPixelXY(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b);

/* 返回亮度缩放后的实际输出 RGB（灯珠真正收到的值）
 * 注意：必须在 WS2812_Show() 之后调用，否则拿到的是上一次输出的值
 */
int WS2812_GetPixelScaled(uint16_t phys_index, uint8_t *r, uint8_t *g, uint8_t *b);
int WS2812_GetPixelXYScaled(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b);

/* 一次性取出整个原始 RGB 缓冲的只读指针（物理顺序、RGB 交错）
 * 返回值可直接用于 memcpy / 比对
 */
const uint8_t *WS2812_GetRawBuffer(void);

/* 便利判断 —— 该物理索引是否被点亮（原始 RGB 中任一非 0 即算亮）
 * 越界返回 -1
 */
int WS2812_IsPixelOn(uint16_t phys_index);
int WS2812_IsPixelXYOn(uint16_t x, uint16_t y);

/* 结构体风格读取（适配 ws2812_color_t 用法） */
int WS2812_GetColor(uint16_t phys_index, ws2812_color_t *out);
int WS2812_GetColorXY(uint16_t x, uint16_t y, ws2812_color_t *out);

#endif /* __WS2812_H__ */