# BLE 与微信小程序交接说明

## 交付范围

本版本固件已实现协议 v1 的全部 14 个命令：设备信息、旋转/模式/今日计划配置、显式刷新、相框切换、时区/位置/Wi-Fi 凭据设置、天气更新和照片上传。协议字段和响应布局以同目录的 [`ble_protocol.md`](ble_protocol.md) 为准；小程序开发以该文件为唯一接口合同，不要另行推导结构体布局。

## 小程序连接流程

1. 扫描 Service UUID `7b4d0001-6a6c-4c61-9f20-65706463616c`，连接单设备 `EPD-Calendar`。
2. 不需要配对、绑定或等待 LE 加密；连接建立后直接发现特征即可。
3. 发现 Service 下 Control RX `...0002`、Notify TX `...0003`、Image RX `...0004`。订阅 Notify 后再发命令。
4. Control/Image 使用普通 GATT 写入；普通设置与控制命令写 Control，只有 `PHOTO_DATA` 写 Image。
5. 用统一的字节流解析器处理 Notify：累加通知数据，寻找 `45 50`，读版本/长度，等到完整帧后验证 CRC16；不要按单条 BLE 通知切帧。
6. 每条新命令使用递增 16 位序号，等待响应类型 `request_type | 0x80` 且序号匹配。Payload 首字节为状态码。原样重试时保持同一序号；对暂时忙的响应准备重新执行时必须分配新序号。

## 客户端实现建议

- 连接后先发 `GET_INFO`，根据返回 mode/rotation/current slot、Wi-Fi/NTP/天气状态渲染配置页。
- 配置命令成功后可再发 `GET_INFO` 更新界面。状态码 `05` 表示暂时忙，可退避后重试；`02` 不应无限重试；参数错误 `01` 应修正数据。
- `WIFI_SET_CREDENTIALS` 会先持久化凭据再异步连接；成功响应只表示已保存且已请求连接，IP 状态稍后通过 `GET_INFO` 查询。`WEATHER_REFRESH` 同样为异步受理命令。
- 刷新请求会由设备串行执行并合并，不应在小程序侧快速连发。显式 `REFRESH` 可越过自动 15 分钟门限。
- `SET_TODAY_PLAN` 传 UTF-8 原始字节，不带 NUL；最多 30 字节（按编码后的字节数，不是汉字数）。空 payload 将显示“今日计划:无”。
- 当前 GATT 不提供链路层保密或设备身份认证；协议 CRC 只用于检测传输损坏。小程序应避免在开放 BLE 链路上传输不必要的敏感数据，并在产品层自行确认设备身份。

## BLE 配网与凭据保存

配网和凭据保存统一使用私有 BLE 协议命令 `08 WIFI_SET_CREDENTIALS`，不再调用 ESPTouch/SmartConfig。设备未配置 Wi-Fi 时会启动 STA 并等待小程序通过 Control RX 写入凭据；已配置时启动后自动连接。凭据不会通过 `GET_INFO` 回传。

小程序编码命令 `08` 时，Payload 为：`ssid_len:u8`、`password_len:u8`、SSID UTF-8/ASCII 字节、密码 UTF-8/ASCII 字节。SSID 1..32 字节，密码 0..64 字节；密码长度 0 表示开放网络。清除设备已保存的凭据使用 `[00, 00]`。Payload 必须严格符合长度，不能包含 NUL、ASCII 控制字符或 DEL。长度按编码后的字节数计算；32 字节 SSID 可完整使用，无需预留结尾 NUL。请按 BLE MTU 分片写入完整协议帧；单个属性写仍需满足 `MTU - 3`。

命令成功表示新凭据已原子写入 NVS，并已请求 STA 应用配置/连接，不代表路由器已接受凭据。随后每 2~3 秒用新序号发送 `GET_INFO`，直至 flags bit0（Wi-Fi 已连接）置位或由 UI 超时。bit4 表示 NVS 中已有 SSID。连接成功后固件启动 SNTP，并在取得 IP 后启动天气任务。连接失败不会清除保存值；小程序可修正密码后再次发送命令。清除凭据后 STA 断开并等待下一次 BLE 配置。

安全边界：当前 GATT 按兼容性要求保持开放，不配对、不加密。该命令中的 Wi-Fi 密码会以明文经过 BLE 空中链路，因此仅应在用户确认的可信近距离场景配网，不要记录到日志、埋点或崩溃报告。协议 CRC 只检测意外传输损坏，不提供认证或保密；固件不会回读密码。

## 图片上传参考

小程序负责把图像裁剪/缩放并量化为设备四色 2bpp 格式，生成 768×552 原始字节流（总长必须正好 105984）。照片方向需与设备最终软件旋转匹配；显示层仍按设备旋转映射 framebuffer。

建议先用 stop-and-wait；命令队列只有 4 项，确认整条链路稳定后最多再试 1..2 项的小窗口：

1. 对最终原始字节流计算 IEEE CRC32。
2. `PHOTO_BEGIN(photo_id, slot, total_len, crc32)`，记住响应的 `next_offset`。
3. 每个 `PHOTO_DATA(offset, bytes)` 的 offset 指向原始文件偏移；收到响应后更新设备返回的第一个未收 offset。可以乱序，但控制队列是串行的，初版建议按序上传。
4. BLE 数据段不超过 `协商 MTU - 10 - 4` 字节（MTU 减 ATT 3 字节、协议头尾 10 字节、offset 4 字节）。协商 MTU 若是 23，每片数据最多 6 字节，性能会很差；小程序应请求更高写入 MTU，或按平台上限自动适配。
5. 断连重连后发送完全相同的 `PHOTO_BEGIN` 查询进度，再从返回 offset 继续。该进度只存 RAM，设备掉电/复位后不可续传。
6. 全部到齐后发送 `PHOTO_END(crc32)`。收到成功响应后照片元数据已原子提交且目标槽被选中；再刷新 `GET_INFO` 或等待设备显示刷新。
7. 放弃任务时发 `PHOTO_ABORT`。不要在已有上传未结束时更换 photo_id/槽位；设备会以 `05 busy` 拒绝并行会话。

`PHOTO_DATA` 分片重发允许，但完整 CRC 是最终正确性校验。CRC/Flash 错误时不要将本地进度当作已提交结果；重新查询或重新开始该槽上传。四槽目标建议由用户明确选择，避免小程序默认覆盖唯一照片。

## 固件工作线程与看门狗

NimBLE 写入回调目前只解协议流并把一帧拷贝到 FreeRTOS 队列；命令执行、NVS、天气请求和照片 Flash 操作在 `ble_commands` 任务里完成。Notify 结果按 peer MTU 拆片发送。EPD 首刷和后续刷新统一经 `refresh_task` 串行处理，刷新任务与 CPU0 Idle 同为优先级 0；BLE 初始化也在独立的优先级 0 任务运行。IDF 6.0.2 FreeRTOS 配置启用了同优先级 time slicing（`configUSE_TIME_SLICING=1`），因此两个低优先级长任务也会让 Idle 获得 WDT 喂狗时间片。

提供的看门狗日志发生于控制器 `hci_le_enh_tx_test_cmd_handler`，同时 CPU0 Idle 未得到调度。产品用不到 Direct Test Mode，sdkconfig defaults 现明确关闭 `CONFIG_BT_CTRL_DTM_ENABLE` 和 NimBLE DTM 测试项；此前 12 次 BLE 功率设置也已收敛为默认/广播/扫描各一次及连接建立后的单次连接等级设置。构建生成配置已确认 DTM 关闭。此修复仍需刷入设备后连续冷启动、普通 BLE 连接和刷新压测确认，不能仅凭构建声称硬件看门狗问题已最终消失。

## 构建和当前限制

验证命令：

```powershell
. .\idf6.ps1
idfpy reconfigure
idfpy build
```

当前 IDF 6.0.2 构建通过，镜像 `0x1812b0` 字节；最小 app 分区 `0x190000`，剩余 `0xed50`（约 4%）。不要再无预算地增加字体/第三方组件。`tests/test_core.c` 已用 Zig C 编译器在 Windows 主机编译并运行通过，覆盖命令处理和 host photo-store 上传 bookkeeping；ESP 专属 Flash bank/断电原子提交仍需板上测验。当前镜像已成功烧录到 COM19，并完成冷启动验证：NimBLE GATT 正常上线，首次 EPD 刷新约 14.8 秒，无 `hci inits failed`、`nimble host init failed`、Guru Meditation 或 task watchdog。当前生成配置未启用 frame pointer；若 watchdog 复现，要在 flash 分区余量允许的前提下考虑启用 `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER` 采集更完整的回溯。

## 上板验收清单

- 清洁冷启动多次：启动 BLE 服务时 CPU0 Idle 不再触发 `task_wdt`；若复现，立即抓完整日志及 frame pointer backtrace，再定位调用栈。
- 空配置启动时通过 BLE 写入 Wi-Fi 凭据，确认 NVS 保存、连接状态和断电重连；使用无效长度/NUL、开放网络、修改密码和清除凭据覆盖边界行为。
- 手机完成普通 BLE 连接、Notify 订阅；无需配对即可读写 Control/Image。
- 逐条跑 14 个命令，验证请求类型/序号回显、响应状态码、NVS 重启后配置保持。
- 用 MTU 23 和较大 MTU 分别验证通知拼帧/拆帧和照片数据大小。
- 图片分片乱序、重发、CRC 错误、断连续传、主动取消、满图提交后断电重启，确认旧照片保护和新照片原子提交。
- EPD 刷新过程中发 BLE 控制命令；屏幕仍由唯一刷新任务访问，命令有响应且 CPU0 Idle watchdog 不触发。
