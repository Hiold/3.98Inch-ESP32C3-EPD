# 条带流式帧缓冲设计

## 目标

将当前 106 KB（768×552，2bpp，分 24 块）的**全帧绘图缓冲**替换为
**16 行 ≈ 3.2 KB 的条带缓冲**。ESP32 逐条带渲染，并把条带流式送进
面板控制器自带的帧内存（800×600×2bit = 120 KB 在控制器里），
因此 ESP32 自身不再需要保留完整帧。

最终内存占用从 ~106 KB 降至 ~3.2 KB（约 3%）。

## 背景与约束

- 面板为 JD79665 4 色（黑/白/黄/红），768×552 可视区，2bpp 打包，4 像素/字节。
- 控制器要求每次 DTM 写入按物理帧 800×600 几何传输；可视图像在 x=32,y=0，
  其余像素打包为白色。
- 当前激活配置：`epd_set_physical_frame(true)`、`epd_set_rowmap(EPD_MAP_LINEAR)`。
  因此物理行 `py` 直接对应逻辑行 `py`（图像区 y=0..551），仅 x 方向偏移 32px
  （8 字节），传输时逐行加白色左填充即可。
- 传输侧当前已是逐行流式（`epd_write_physical_frame`：一次拿住 SPI 总线，逐行发
  200 字节，CS 持续拉低）。本设计沿用这套已被证明的传输机制。
- 绘图层（`manual_canvas` / `ui_app.c` / `test_patterns.c`）当前是**全图随机访问**：
  任意 (x,y) 画点、字体跨行、相框像素经旋转映射。这是必须重构的核心。

## 架构

渲染由「填充全局帧缓冲」改为「按条带重跑绘制序列」：

```
refresh_task:
  for strip_y0 in 0..EPD_HEIGHT-1 step EPD_STRIP_ROWS:
    strip_begin(strip_y0)          # 清空条带缓冲为白，设置裁剪窗口
    draw_<page>(strip_canvas)      # 重跑绘制序列，图元裁剪到当前条带
    strip_flush()                  # 构建带物理填充的行，流式送入控制器
  epd_refresh()
```

### 1. 条带缓冲

替换 `s_fb_chunks[]`（24 块 × ~4.4 KB）为：

- `s_strip[EPD_STRIP_ROWS][EPD_BYTES_PER_LINE]` = 16 × 192 = 3072 字节（图像行）。
- 一块 200 字节的物理行暂存，用于加 8 字节白色左填充。

`EPD_STRIP_ROWS = 16`。

### 2. 裁剪模型

绘制图元统一收敛到 `epd_fb_set_pixel(x, y, color)`：

- 先把逻辑坐标经旋转映射到原生坐标（现有 `display_rotation_map_point`）。
- 若原生 `y` 落在当前活动条带 `[strip_y0, strip_y0+EPD_STRIP_ROWS)` 内，
  写入 `s_strip[native_y - strip_y0][native_x / 4]`；否则丢弃。

所有高层图元（`hline`、`vline`、`rect`、`rect_fill`、`char`、文本、天气/图标、
相框像素）都经 `epd_fb_set_pixel`，因此在**这一处**裁剪即可统一处理，包括跨条带
字体和任意图形。裁剪点在原生坐标做，天然兼容 0/90/180/270 旋转，无需特判。

### 3. 条带生命周期 API

新增到 `epd_panel.h`：

- `void epd_strip_begin(int y0);`  清条带为白、记录活动条带起始行。
- `void epd_strip_flush(void);`   把当前条带（含物理填充）流式送进控制器。

保留现有绘制 API，但改为写入活动条带：
- `epd_fb_fill` 只清**当前条带**（不是全帧）。
- `epd_fb_set_pixel` 按上述裁剪写条带。

### 4. 控制器写入路径

新增 `epd_write_strips()`：

- 打开一次 DTM，全物理窗口。
- 对每个条带，逐行构建 200 字节物理行（`phys_line` 逻辑保留），在**一次 CS
  拿住**的会话内连续发送（复用 `epd_write_physical_frame` 的传输细节）。
- 全部条带发完后 `epd_refresh(true)`。

### 5. API 变更

移除（不再需要全帧）：
- `epd_fb_raw()`
- `epd_fb_copy_from()`
- `epd_fb_read_byte()`
- 分块存储 `s_fb_chunks[]` / `fb_byte_ptr` / `fb_row_ptr` / `fb_ready`

`epd_display()` 不再从全帧缓冲推图；条带循环由**上层绘制函数**驱动：

```
draw_<page>(canvas):                       # 上层，仍画逻辑坐标
    for strip_y0 in 0..EPD_HEIGHT-1 step 16:
        epd_strip_begin(strip_y0)
        ... 重跑该页绘制，图元裁剪到当前条带 ...
        epd_strip_flush()
```

`epd_display()` 收敛为 `epd_power_on() -> epd_write_strips(回调) -> epd_refresh(true)`，
其中回调即上层的 `draw_<page>`，由 `epd_write_strips` 逐条带调用。底层只负责
「条带缓冲 + 流式发送」，不感知页面内容。

### 6. test_patterns 适配

- 全帧填充类图案：`epd_fb_fill` 逐条带执行即可，接口不变。
- `ref_image`（const 全帧数组）：直接逐行从常量数组流式发送，ESP32 不缓冲。
- 校验和诊断（`epd_fb_read_byte` 扫 106 KB）：改为对条带缓冲逐条带校验，或移除。

### 7. 相框（BLE 照片）

照片存于 flash（非 RAM），`draw_photo_page` 逐行读取。条带化后每个条带只重读
与其相交的相片行，天然适配，无需额外缓冲。

## 数据流

```
上层页面绘制函数 (逻辑坐标, 任意顺序)
        │  每个条带重跑一次
        ▼
manual_canvas_pixel ──旋转映射──▶ 原生坐标 ──裁剪──▶ s_strip[native_y][native_x/4]
        │                                               │ 条带满
        ▼                                               ▼
   strip_flush(): 逐行加白色左填充成 200 字节 ──▶ 一次 CS 会话内连续 SPI 发送 ──▶ 控制器帧内存
```

## 错误处理

- 条带发送任一 SPI 事务失败 → 终止本次刷新，报告 `EPD_STAGE_DATA` 失败。
- 条带缓冲为静态分配，无需失败路径（不再依赖运行时大块分配）。

## 测试

- 现有 `test_patterns` 全量跑一遍（solid / info / checker / ref_image / 校准 /
  条纹等），确认渲染与刷屏与改前一致。
- 日历（横/竖）、状态页、相框三种模式各刷一次。
- 旋转 0/90/180/270 各刷一次，确认裁剪在原生坐标的处理正确。
- 编译并烧录验证 ESP32-C3 上无大块分配失败（对比改前 106 KB 分配）。

## 范围

只涉及 `main/epd_panel.c/h`、`main/display/manual_canvas.*`、
`main/ui/ui_app.c`、`main/test_patterns.c`、`main/main.c`。
不触碰 BLE 上传、照片存储、NTP/天气等与绘制无关的子系统。