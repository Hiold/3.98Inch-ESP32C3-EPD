# 条带流式帧缓冲实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 用 16 行 ≈ 3.2 KB 的条带缓冲替代当前 106 KB（768×552，2bpp，分 24 块）的全帧绘图缓冲。ESP32 逐条带渲染，并把条带流式送进面板控制器自带的帧内存（800×600×2bit=120 KB 在控制器里），ESP32 自身不再保留完整帧。

**架构：** 渲染由「填充全局帧缓冲」改为「按条带重跑绘制序列」：对每个 16 行的条带，先 `epd_strip_begin(y0)` 清空并设置裁剪窗口，重跑整页绘制（所有图元经 `epd_fb_set_pixel` 在**原生坐标**处裁剪到当前条带，自动兼容旋转和跨条带字体），再 `epd_strip_flush()` 把条带加 8 字节白色左填充成 200 字节物理行，在**一次 CS 会话**内连续 SPI 发送进控制器帧内存。`epd_display()` 收敛为 `PON → epd_write_strips(回调) → refresh`。

**技术栈：** ESP-IDF 6.x，ESP32-C3，JD79665 4 色墨水屏驱动。无主机单元测试框架，验证方式为「代码编译通过 + 上板视觉核对」。

**规格：** `docs/superpowers/specs/2026-09-30-strip-streaming-framebuffer-design.md`

---

## 文件结构

修改的文件及其职责：

- `main/epd_panel.h` — 新增条带生命周期 API（`epd_strip_begin`、`epd_strip_flush`、`epd_strip_copy`）、条带渲染回调类型 `epd_strip_render_fn`、新 `epd_display` 签名；移除 `epd_fb_raw`、`epd_fb_copy_from`、`epd_fb_read_byte`。
- `main/epd_panel.c` — 用 16 行条带缓冲 `s_strip[16][192]` 替换 24 块全帧缓冲；`epd_fb_set_pixel`/`epd_fb_fill` 改为写入活动条带并裁剪；新增 `epd_strip_begin`/`epd_strip_flush`/`epd_write_strips`/`epd_strip_copy`；`epd_display` 改为条带循环。
- `main/display/manual_canvas.c` — `manual_canvas_init` 不再依赖 `epd_fb_raw()`；`manual_canvas_clear` 语义随 `epd_fb_fill` 变为「清当前条带」。
- `main/ui/ui_app.c` — `ui_app_render` 改为构建一次 canvas + 本地时间，然后通过 `epd_display_page`/`epd_display` 的条带回调重跑页面绘制。
- `main/test_patterns.c` — 各测试页改为条带回调模式；`ref_image` 用新的 `epd_strip_copy` 逐条带流式；`test_dump_fb` 改为对条带校验或移除。
- `main/main.c` — 适配 `epd_display` 新签名；移除对 `epd_fb_raw` 的依赖（如有）。

---

### 任务 1：在 epd_panel 中用条带缓冲替换全帧缓冲并实现条带生命周期

**文件：**
- 修改：`main/epd_panel.c`
- 修改：`main/epd_panel.h`

这是核心任务。目标：删掉 24 块全帧缓冲，换成 16 行条带缓冲；所有绘制图元改为写入活动条带并在原生坐标裁剪；新增条带生命周期与流式发送。

- [ ] **步骤 1：替换缓冲存储与 `epd_fb_alloc` 段**

在 `main/epd_panel.c` 中，删除第 65-67 行附近的以下内容：

```c
#define EPD_FB_CHUNKS 24u
#define EPD_FB_CHUNK_BYTES ((EPD_FRAME_BYTES + EPD_FB_CHUNKS - 1u) / EPD_FB_CHUNKS)
static uint8_t *s_fb_chunks[EPD_FB_CHUNKS];
```

及其辅助函数 `fb_byte_ptr`（70-76 行）、`fb_row_ptr`（78-83 行）、`fb_ready`（85-91 行）。

替换为条带缓冲定义：

```c
#define EPD_STRIP_ROWS 16u
static uint8_t s_strip[EPD_STRIP_ROWS][EPD_BYTES_PER_LINE];
static int s_strip_y0;             /* 当前活动条带的首行（原生 y），-1 = 无活动条带 */
static bool s_strip_active;

static inline uint8_t *strip_row_ptr(int r)
{
    return (r >= 0 && r < (int)EPD_STRIP_ROWS) ? s_strip[r] : NULL;
}
```

- [ ] **步骤 2：重写 `epd_fb_alloc` 与 `epd_fb_raw`**

把 `epd_fb_alloc`（898-925 行）改成空操作（不再需要运行时分配），返回 `ESP_OK`：

```c
esp_err_t epd_fb_alloc(void)
{
    /* 条带缓冲为静态数组，无需运行时分配。 */
    s_strip_active = false;
    return ESP_OK;
}
```

删除 `epd_fb_raw`（927 行）。

- [ ] **步骤 3：实现条带生命周期 API**

新增：

```c
void epd_strip_begin(int y0)
{
    if (y0 < 0) y0 = 0;
    if (y0 >= EPD_HEIGHT) y0 = EPD_HEIGHT - (int)EPD_STRIP_ROWS;
    s_strip_y0 = y0;
    s_strip_active = true;
    /* 白 = 0x55（4 像素均为白 01）。 */
    const uint8_t white = 0x55u;
    for (size_t i = 0; i < EPD_STRIP_ROWS; ++i) {
        memset(s_strip[i], white, EPD_BYTES_PER_LINE);
    }
}

bool epd_strip_active(void) { return s_strip_active; }

void epd_strip_copy(const uint8_t *src, size_t src_stride, int rows)
{
    if (!src || !s_strip_active) return;
    if (rows > (int)EPD_STRIP_ROWS) rows = (int)EPD_STRIP_ROWS;
    for (int r = 0; r < rows; ++r) {
        memcpy(s_strip[r], src + (size_t)r * src_stride, EPD_BYTES_PER_LINE);
    }
}
```

- [ ] **步骤 4：改写绘制图元写入活动条带**

`epd_fb_set_pixel`（958-967 行）改为在原生坐标裁剪：

```c
void epd_fb_set_pixel(int x, int y, uint8_t color)
{
    if (!s_strip_active || x < 0 || y < 0 || x >= EPD_WIDTH) {
        return;
    }
    const int r = y - s_strip_y0;
    if (r < 0 || r >= (int)EPD_STRIP_ROWS) {
        return; /* 不在当前条带，丢弃 */
    }
    size_t idx = (size_t)(x >> 2);
    int shift = 6 - 2 * (x & 3);
    uint8_t *byte = &s_strip[r][idx];
    *byte = (uint8_t)((*byte & ~(0x03u << shift)) | ((color & 0x03u) << shift));
}
```

`epd_fb_fill`（946-956 行）改为只清当前条带：

```c
void epd_fb_fill(uint8_t color)
{
    if (!s_strip_active) return;
    uint8_t packed = (uint8_t)((color << 6) | (color << 4) | (color << 2) | color);
    for (size_t i = 0; i < EPD_STRIP_ROWS; ++i) {
        memset(s_strip[i], packed, EPD_BYTES_PER_LINE);
    }
}
```

其余高层图元（`hline`/`vline`/`rect`/`rect_fill`/`rect_fill_frame`/`char`）都经 `epd_fb_set_pixel`，**无需改动**，自动获得条带裁剪。

- [ ] **步骤 5：实现条带流式发送 `epd_write_strips`**

删除旧的 `phys_line`（403-416 行）、`frame_line`（436-447 行）、`epd_write_physical_frame`（1122-1177 行）、`epd_write_frame`（1179-1227 行）、`epd_write_frame_interleaved`（140-165 行）、`build_wire_buffer`（243-265 行）及不再使用的 `s_wire`、`s_physical_frame` 相关临时逻辑。

新增条带流式发送（复用 `epd_write_physical_frame` 已被证明的「一次拿总线 + 逐行连续 SPI」机制）：

```c
/* 一条 200 字节物理行：8 字节白色左填充 + 192 字节图像行。 */
static void strip_phys_line(int r, uint8_t *out)
{
    memset(out, 0x55, EPD_PHYS_BYTES_PER_LINE); /* 白 */
    memcpy(out + EPD_PHYS_X_OFFSET / 4, s_strip[r], EPD_BYTES_PER_LINE);
}

typedef esp_err_t (*epd_strip_render_fn)(void *ctx, int y0);

esp_err_t epd_write_strips(epd_strip_render_fn render, void *ctx)
{
    if (!s_bus_ready) return ESP_ERR_INVALID_STATE;
    s_diag.last_stage = EPD_STAGE_DATA;

    esp_err_t err = epd_set_full_window();
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "set full window"); return err; }
    err = write_cmd(CMD_DTM);
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "DTM"); return err; }

    static uint8_t line[EPD_PHYS_BYTES_PER_LINE];
    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "acquire strip bus"); return err; }
    gpio_set_level(PIN_EPD_DC, 1);

    int py = 0;
    for (int y0 = 0; y0 < EPD_HEIGHT; y0 += (int)EPD_STRIP_ROWS) {
        epd_strip_begin(y0);
        if (render) {
            err = render(ctx, y0);
            if (err != ESP_OK) break;
        }
        for (int r = 0; r < (int)EPD_STRIP_ROWS; ++r) {
            strip_phys_line(r, line);
            spi_transaction_t t = {
                .length = EPD_PHYS_BYTES_PER_LINE * 8,
                .tx_buffer = line,
                .flags = (py + 1 < EPD_PHYS_HEIGHT) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            };
            err = spi_device_polling_transmit(s_spi, &t);
            if (err != ESP_OK) break;
            ++py;
            if (py >= EPD_PHYS_HEIGHT) break;
        }
        if (err != ESP_OK) break;
    }
    /* 补足图像区之后的物理行（552..599）为白色，凑满 600 行控制器帧。 */
    while (err == ESP_OK && py < EPD_PHYS_HEIGHT) {
        memset(line, 0x55, EPD_PHYS_BYTES_PER_LINE);
        spi_transaction_t t = {
            .length = EPD_PHYS_BYTES_PER_LINE * 8,
            .tx_buffer = line,
            .flags = (py + 1 < EPD_PHYS_HEIGHT) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(s_spi, &t);
        ++py;
    }
    spi_device_release_bus(s_spi);
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "strip payload"); return err; }

    s_diag.frame_count++;
    s_diag.bytes_sent += EPD_PHYS_FRAME_BYTES;
    s_diag.last_stage = EPD_STAGE_IDLE;
    s_diag.last_err = ESP_OK;
    ESP_LOGI(TAG, "strip frame sent: %d rows, %u bytes total",
             EPD_PHYS_HEIGHT, (unsigned)EPD_PHYS_FRAME_BYTES);
    return ESP_OK;
}
```

注意：`s_diag`、`write_cmd`、`spi_device_acquire_bus`、`EPD_PHYS_BYTES_PER_LINE` 等在本文件内已存在，直接复用。

- [ ] **步骤 6：重写 `epd_display`**

把 `epd_display`（1343-1361 行）改为接收条带回调：

```c
esp_err_t epd_display(epd_strip_render_fn render, void *ctx)
{
    esp_err_t err = epd_power_on();
    if (err != ESP_OK) return err;
    err = epd_write_strips(render, ctx);
    if (err != ESP_OK) return err;
    return epd_refresh(true);
}
```

- [ ] **步骤 7：更新头文件 API**

在 `main/epd_panel.h`：

- 新增 `EPD_STRIP_ROWS` 宏、`epd_strip_begin`、`epd_strip_active`、`epd_strip_copy`、`epd_write_strips` 与 `epd_strip_render_fn` 类型声明。
- 把 `epd_display` 签名改为 `esp_err_t epd_display(epd_strip_render_fn render, void *ctx);`。
- 删除 `epd_fb_raw`、`epd_fb_copy_from`、`epd_fb_read_byte` 声明。

头文件需要：

```c
#define EPD_STRIP_ROWS 16u

/* 条带生命周期：渲染器对每个 16 行条带调用 begin -> 绘制 -> flush。 */
typedef esp_err_t (*epd_strip_render_fn)(void *ctx, int y0);
void epd_strip_begin(int y0);
bool epd_strip_active(void);
void epd_strip_copy(const uint8_t *src, size_t src_stride, int rows);
esp_err_t epd_write_strips(epd_strip_render_fn render, void *ctx);

esp_err_t epd_display(epd_strip_render_fn render, void *ctx);
```

- [ ] **步骤 8：编译验证（用户执行）**

```bash
idf.py build
```
预期：能编译到除 `manual_canvas.c`/`ui_app.c`/`test_patterns.c`/`main.c` 之外可能仍引用已删 API 的报错——本任务后这些文件**仍会报错**，属预期，下一步处理。先确保 `epd_panel.c`/`.h` 自身无语法错误（可暂时只编译该文件或先注释外部引用）。

- [ ] **步骤 9：Commit**

```bash
git add main/epd_panel.c main/epd_panel.h
git commit -m "refactor(epd): replace full-frame buffer with 16-row strip streaming"
```

---

### 任务 2：适配 manual_canvas 与 UI 渲染器到条带模型

**文件：**
- 修改：`main/display/manual_canvas.c`
- 修改：`main/ui/ui_app.c`

目标：让日历/状态/相框页面按条带重跑绘制，去掉对全帧缓冲的依赖，适配新 `epd_display` 签名。

- [ ] **步骤 1：修改 `manual_canvas_init` 的缓冲检查**

`manual_canvas.c` 第 31-35 行 `#ifdef ESP_PLATFORM if (!epd_fb_raw()) return 0; #endif` 改为检查条带可用（条带缓冲是静态的，恒可用，故直接去掉该检查或改为检查面板已初始化）：

```c
#ifdef ESP_PLATFORM
    /* 条带缓冲为静态数组，恒可用；无需检查 epd_fb_raw。 */
#endif
```

`manual_canvas_clear`（40-48 行）调用 `epd_fb_fill`——语义已变为「清当前条带」，保持不变，但调用时机须在 `epd_strip_begin` 之后。

- [ ] **步骤 2：改造 `ui_app_render` 为条带回调**

`ui_app.c` 的 `ui_app_render`（462-488 行）当前一次性初始化 canvas、清屏、画整页。改为：初始化 canvas 与本地时间，然后调用 `epd_display`，并把整页绘制函数作为条带回调（每 16 行重跑一次，图元自动裁剪到当前条带）。

新增一个上下文结构与回调，替换原有 `ui_app_render`：

```c
typedef struct {
    manual_canvas_t canvas;
    struct tm local;
} ui_render_ctx_t;

static esp_err_t ui_draw_strip(void *ctx, int y0)
{
    (void)y0;
    ui_render_ctx_t *uc = (ui_render_ctx_t *)ctx;
    const manual_canvas_t *canvas = &uc->canvas;

    if (current.mode == UI_MODE_STATUS) {
        draw_status_page(canvas);
    } else if (current.mode == UI_MODE_PHOTO) {
        draw_photo_page(canvas);
    } else if (canvas->width >= 600) {
        draw_calendar_landscape(canvas, &uc->local,
                                current.mode == UI_MODE_CALENDAR_PHOTO);
    } else {
        draw_calendar_portrait(canvas, &uc->local,
                               current.mode == UI_MODE_CALENDAR_PHOTO);
    }
    return ESP_OK;
}

int ui_app_render(void)
{
    ui_render_ctx_t uc;
    const display_rotation_t rotation =
        (display_rotation_t)((current.rotation & 0x03u) * 90u);
    if (!manual_canvas_init(&uc.canvas, rotation)) return -1;

    time_t timestamp = time(NULL);
    struct tm local = {0};
    const bool time_valid = timestamp >= 1000000000 &&
                            localtime_r(&timestamp, &local) != NULL;
    if (!time_valid) use_build_date(&local);
    uc.local = local;

    /* 去掉独立 manual_canvas_clear：epd_strip_begin 每个条带清一次。 */
    return epd_display(ui_draw_strip, &uc) == ESP_OK ? 0 : -1;
}
```

注意：`current`、`draw_status_page`、`draw_photo_page`、`draw_calendar_landscape`、`draw_calendar_portrait`、`use_build_date` 均为本文件已有静态符号，直接复用。

- [ ] **步骤 3：编译验证（用户执行）**

```bash
idf.py build
```
预期：`ui_app.c`、`manual_canvas.c` 通过；剩余报错集中在 `test_patterns.c` 和 `main.c`（仍用旧签名/已删 API），下一步处理。

- [ ] **步骤 4：Commit**

```bash
git add main/display/manual_canvas.c main/ui/ui_app.c
git commit -m "refactor(ui): render calendar/status/photo per strip"
```

---

### 任务 3：适配 test_patterns 到条带模型

**文件：**
- 修改：`main/test_patterns.c`

目标：所有测试页改为条带回调模式；`ref_image` 用 `epd_strip_copy` 逐条带流式；`test_dump_fb` 移除对 `epd_fb_raw`/`epd_fb_read_byte` 的依赖。

- [ ] **步骤 1：修改 `label()` 移除 `epd_fb_raw` 依赖**

`label()`（20-30 行）当前先 `epd_fb_raw()` 判空再画。删掉判空，改为直接调用 `epd_fb_char`（`epd_fb_char` 会经 `set_pixel` 裁剪到当前条带）：

```c
static void label(int x, int y, const char *s, uint8_t color, int scale)
{
    for (; *s; s++) {
        epd_fb_char(x, y, *s, color, scale);
        x += 6 * scale;
    }
}
```

- [ ] **步骤 2：新增条带绘制包装**

所有测试页都通过一个统一的条带回调把「绘制函数 + 参数」交给 `epd_display`。用一个小结构打包函数指针与参数：

```c
typedef void (*page_draw_arg_fn)(void *arg);

typedef struct {
    page_draw_arg_fn fn;
    void *arg;
} page_call_t;

static esp_err_t page_strip_render(void *ctx, int y0)
{
    (void)y0;
    page_call_t *call = (page_call_t *)ctx;
    call->fn(call->arg);
    return ESP_OK;
}

static esp_err_t display_page_arg(page_draw_arg_fn fn, void *arg)
{
    static page_call_t call;
    call.fn = fn;
    call.arg = arg;
    return epd_display(page_strip_render, &call);
}
```

无参绘制是带参的特例：各页把整页绘制体抽成 `static void draw_xxx(void *arg)`（未用 `arg` 则 `(void)arg;`），末尾 `return display_page_arg(draw_xxx, NULL);`。

例如 `test_pattern_solid`（41-45 行）：

```c
static void draw_solid(void *arg)
{
    epd_fb_fill((uint8_t)(uintptr_t)arg);
}

esp_err_t test_pattern_solid(uint8_t color)
{
    /* epd_fb_fill 只清当前条带，逐条带重跑即可填满整帧。 */
    return display_page_arg(draw_solid, (void *)(uintptr_t)color);
}
```

需要转换的页（列出函数名与现有行号）：
- `test_pattern_info`（54 行）→ 抽 `draw_info`
- `test_pattern_checker`（107 行）→ 抽 `draw_checker`（带 `cell` 参数，用 ctx 传）
- `test_pattern_corners`（166 行）→ 抽 `draw_corners`
- `test_pattern_stripes_x`（207 行）→ 抽 `draw_stripes_x`
- `test_pattern_axis_probe_h`（236 行）→ 抽 `draw_probe_h`
- `test_pattern_axis_probe_v`（255 行）→ 抽 `draw_probe_v`
- `test_pattern_stripes`（282 行）→ 抽 `draw_stripes`
- `test_pattern_stripes_v`（301 行）→ 抽 `draw_stripes_v`
- `test_pattern_info_b`（316 行）→ 复用 `draw_info` + 下半覆盖，抽 `draw_info_b`
- `test_pattern_probe_a`（361 行）→ 抽 `draw_probe_a`
- `test_pattern_probe_b`（380 行）→ 抽 `draw_probe_b`

`test_pattern_stripes_x` 内先调用 `test_pattern_solid(WHITE)` 的写法在条带模型下改为：直接让 `draw_stripes_x` 先 `epd_fb_fill(EPD_COLOR_WHITE)` 再画条纹，无需先刷一帧。

带参数的绘制函数（如 `checker` 的 `cell`）通过 `display_page_arg` 的 `arg` 传递。

- [ ] **步骤 3：处理 `ref_image` 特殊路径**

`test_pattern_ref_image`（127-134 行）当前 `epd_fb_copy_from(ref_image, REF_IMAGE_BYTES)`。改为逐条带从 const 数组流式加载（`epd_strip_copy`），不做全帧缓冲：

```c
static esp_err_t ref_image_strip_render(void *ctx, int y0)
{
    (void)ctx;
    epd_strip_copy(ref_image + (size_t)y0 * EPD_BYTES_PER_LINE,
                   EPD_BYTES_PER_LINE, EPD_STRIP_ROWS);
    return ESP_OK;
}

esp_err_t test_pattern_ref_image(void)
{
    return epd_display(ref_image_strip_render, NULL);
}
```

注意 `EPD_STRIP_ROWS`、`EPD_BYTES_PER_LINE` 已在 `epd_config.h`/`epd_panel.h` 定义。

- [ ] **步骤 4：处理 `test_dump_fb`**

`test_dump_fb`（409-433 行）依赖 `epd_fb_raw`/`epd_fb_read_byte`。移除全帧扫描，改为打印面板诊断信息即可（或直接删除该函数及其调用点）：

```c
void test_dump_fb(void)
{
    const epd_diag_t *d = epd_diag();
    ESP_LOGI(TAG, "framebuffer: strip-based (%d rows x %d bytes), no full-frame buffer",
             (int)EPD_STRIP_ROWS, EPD_BYTES_PER_LINE);
    ESP_LOGI(TAG, "last frame bytes sent: %llu", (unsigned long long)d->bytes_sent);
}
```

- [ ] **步骤 5：编译验证（用户执行）**

```bash
idf.py build
```
预期：`test_patterns.c` 通过。若 `main.c` 或 `manual_canvas.c` 还有对已删 API 的引用，一并在本任务修正。

- [ ] **步骤 6：Commit**

```bash
git add main/test_patterns.c
git commit -m "refactor(test): render test patterns and ref image per strip"
```

---

### 任务 4：清理 main.c 适配与死代码

**文件：**
- 修改：`main/main.c`
- 修改：`main/epd_panel.c`

目标：让 `main.c` 适配新 `epd_display` 签名；删除 `epd_fb_copy_from`、`epd_fb_read_byte` 等已无调用者的旧函数。

- [ ] **步骤 1：检查并适配 `main.c` 的调用点**

`main.c` 第 352 行 `epd_fb_alloc()` 仍有效（现为空操作），保留。第 640 行 `result = epd_display();` 需改为带条带回调的形式。查看上下文：该处由 `refresh_task` 调用，前面已 `ui_app_render()`。由于 `ui_app_render` 内部现在自己调用了 `epd_display`，`main.c` 的 `epd_display()` 调用应移除，避免重复刷新：

```c
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        ui_app_set_state(&ui_state);
        int result = ui_app_render();   /* 内部已包含条带流式 epd_display */
        xSemaphoreGive(s_display_mutex);
        if (result != 0) ESP_LOGE(TAG, "queued EPD refresh failed: %d", result);
```

- [ ] **步骤 2：删除已无调用者的旧函数**

在 `epd_panel.c` 中删除：`epd_fb_copy_from`（929-939 行）、`epd_fb_read_byte`（941-944 行），以及任务 1 未清干净的全帧相关残留（`s_wire`、`WIRE_BYTES`、`build_wire_buffer` 相关声明）。确认 `grep` 无残留引用：

```bash
grep -rn "epd_fb_copy_from\|epd_fb_read_byte\|epd_fb_raw\|s_wire\|WIRE_BYTES" main/
```

- [ ] **步骤 3：编译验证（用户执行）**

```bash
idf.py build
```
预期：全项目编译通过，无 `-Werror` 之外的警告（有未使用函数/变量警告则清理）。

- [ ] **步骤 4：Commit**

```bash
git add main/main.c main/epd_panel.c
git commit -m "refactor(main): drop duplicate display call, remove dead fb code"
```

---

### 任务 5：上板视觉验证与内存确认

**文件：** 无代码改动（验证任务）

目标：确认渲染正确、旋转正确、内存占用显著下降。

- [ ] **步骤 1：确认内存占用**

在 `main.c` 引导日志附近（393 行）已打印 `heap before background tasks`。改前全帧缓冲 106 KB；改后为静态 3.2 KB 条带。对比改前改后的 `esp_get_free_heap_size()` 日志，确认可用堆增加约 100 KB。

- [ ] **步骤 2：跑全部 test_patterns**

用控制台命令逐个跑：`solid`、`info`、`checker`、`ref_image`、`corners`、`stripes_x`、`stripes_y`、`axis_probe_h`、`axis_probe_v`、`stripes`、`info_b`、`probe_a`、`probe_b`。视觉核对每页与改前一致（尤其颜色条、1px 细节、边界、旋转）。

- [ ] **步骤 3：跑三种 UI 模式与旋转**

- 日历（横 + 竖）
- 状态页
- 相框（BLE 上传一张图后显示）
- 旋转 0/90/180/270 各刷一次

核对：整屏内容完整、无遗漏条带、无错位、无旧帧残留；跨条带字体（如日历大数字、天气图标）显示完整无截断。

- [ ] **步骤 4：Commit（如发现问题修复后再提交）**

无代码改动则本任务不产生提交；若发现条带边界 bug，回到对应任务修复后提交。

---

## 自检

**规格覆盖度对照：**

| 规格章节 | 对应任务 |
|---------|---------|
| 1 条带缓冲（16 行 / 3.2 KB） | 任务 1 步骤 1 |
| 2 裁剪模型（原生坐标统一裁剪） | 任务 1 步骤 4 |
| 3 条带生命周期 API | 任务 1 步骤 3 |
| 4 控制器写入路径（epd_write_strips） | 任务 1 步骤 5-6 |
| 5 API 变更（移除 raw/copy_from/read_byte） | 任务 1 步骤 7、任务 4 步骤 2 |
| 6 test_patterns 适配 | 任务 3 |
| 7 相框适配 | 任务 2 步骤 2（draw_photo_page 经 set_pixel 裁剪） |
| 旋转支持 | 任务 1 步骤 4（原生坐标裁剪）、任务 5 步骤 3 |

**占位符扫描：** 无 TODO/待定；每个代码步骤都有完整代码。任务 3 中"逐页"转换列出了所有页函数名与行号，无模糊占位。

**类型/签名一致性：**
- `epd_strip_render_fn` 在所有任务中统一为 `esp_err_t (*)(void *ctx, int y0)`。
- `epd_display(render, ctx)` 签名在任务 1、2、3 一致。
- `epd_strip_copy(src, src_stride, rows)` 在任务 1 定义、任务 3 使用，签名一致。
- `EPD_STRIP_ROWS` 宏名一致（任务 1 定义于 `.h`，任务 3 使用）。
- `manual_canvas_clear` 语义随 `epd_fb_fill` 变化在任务 2 说明，且调用时机已移到 `epd_strip_begin` 之后（由 `epd_strip_begin` 每条带清一次，移除独立 clear）。

**风险说明：** `main.c` 中 `refresh_task` 原为 `ui_app_render()` 后 `epd_display()`；改后 `ui_app_render` 内部完成刷新，需移除 `main.c` 的重复调用，否则会连续刷两次。已在任务 4 步骤 1 处理。