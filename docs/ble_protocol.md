# BLE 控制协议 v1

本文是固件与微信小程序的接口契约。协议核心实现在 `main/ble/ble_protocol.c`、命令在 `main/ble/ble_commands.c`，BLE 服务在 `main/ble/ble_service.cpp`。

## GATT 与安全

| 特征 | UUID | 属性与用途 |
| --- | --- | --- |
| Service | `7b4d0001-6a6c-4c61-9f20-65706463616c` | 私有服务 |
| Control RX | `7b4d0002-6a6c-4c61-9f20-65706463616c` | 普通 GATT 写入控制命令；除 `PHOTO_DATA` 外均写这里 |
| Notify TX | `7b4d0003-6a6c-4c61-9f20-65706463616c` | 普通 GATT 读取/订阅命令结果通知 |
| Image RX | `7b4d0004-6a6c-4c61-9f20-65706463616c` | 普通 GATT 写入 `PHOTO_DATA` |

固件使用开放的常规 GATT，不要求配对、绑定或链路加密；小程序连接后可直接发现特征、订阅 Notify 并写入 Control/Image。协议仍使用 CRC16、序号、长度和命令参数校验，但这些机制不提供机密性或身份认证。Wi-Fi 密码通过开放 BLE 明文传输并保存到设备 NVS；仅应在可信、近距离配网环境使用，不能把 CRC 当作安全保护。每条请求得到同序号响应；响应通知若超过对端 ATT MTU，会拆成连续字节片段，小程序应拼接字节流后按帧头、长度和 CRC 解帧。ATT 单次值长度为 `协商 MTU - 3`。如果没有启用 Notify 订阅，写入虽可能成功，结果通知不会到达。

## 帧格式

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 2 | Magic：`45 50`（ASCII `EP`） |
| 2 | 1 | 协议版本：`01` |
| 3 | 1 | 命令类型；响应类型为请求类型 OR `80` |
| 4 | 2 | 序号，小端 |
| 6 | 2 | Payload 长度，小端 |
| 8 | N | Payload |
| 8+N | 2 | CRC-16/CCITT，小端，覆盖帧头和 Payload；初值 `FFFF`、多项式 `1021`、不反射、无最终异或 |

协议整数均为小端。请求和解析器最大 Payload 为 1024 字节。一个 GATT 写可以只含帧的一部分，也可以与相邻帧粘连；固件按每个特征分别缓存和解帧。序号按 16 位回绕，由客户端分配；固件回显，并缓存最近 4 个序号对应的命令结果。相同序号/类型/长度/内容的重试会重放原响应而不再次执行副作用；相同序号但内容不同返回 `01`。因此客户端应在新命令或对忙状态重新尝试时使用新序号。收到 CRC 错误的帧不会执行命令，也不会发送响应。

## 响应约定

响应 Payload 首字节是状态码，其余是命令数据：`00` 成功、`01` 参数/CRC 无效、`02` NVS/Flash 错误、`03` 当前状态不允许、`04` 不支持、`05` 资源忙/队列满。响应帧类型是请求类型 `| 0x80`，序号原样回显。异步操作的成功表示已受理，不保证网络事务已完成；例如 Wi-Fi 凭据命令返回成功表示已保存且已请求连接，是否取得 IP 应随后用 `GET_INFO` 查询。

## 命令

所有响应数据在状态码之后。除特别注明，未列出的 Payload 长度必须为 0。

| ID | 命令 | 请求 Payload | 成功响应数据 | 特征/效果 |
| ---: | --- | --- | --- | --- |
| `01` | `GET_INFO` | 空 | 见下文 | Control，读取运行状态和设置 |
| `02` | `SET_ROTATION` | `u8`：0/1/2/3 对应 0/90/180/270° | 空 | Control，NVS 持久化并强制排队刷新 |
| `03` | `SET_MODE` | `u8`：0 日历、1 相框、2 日历+相框、3 状态页 | 空 | Control，NVS 持久化并刷新 |
| `04` | `REFRESH` | 空 | 空 | Control，显式强制刷新；仍由 EPD 单队列串行 |
| `05` | `NEXT_PHOTO` | 空 | `u8` 新槽位 | Control，槽位 0..3 环绕，持久化并刷新 |
| `06` | `SET_TIMEZONE` | 1..63 字节 POSIX/IANA 时区名 ASCII，例如 `Asia/Shanghai` | 空 | Control，持久化时区并刷新 |
| `07` | `SET_LOCATION` | `i32 lat_e7, i32 lon_e7` | 空 | Control，范围纬度 ±90°、经度 ±180°；保存并请求天气更新 |
| `08` | `WIFI_SET_CREDENTIALS` | `u8 ssid_len, u8 password_len, ssid[], password[]` | 空 | Control，校验后先保存到 NVS，再配置 STA 并发起连接；两个长度均为 0 时清除凭据 |
| `09` | `WEATHER_REFRESH` | 空 | 空 | Control，异步请求天气更新 |
| `0A` | `PHOTO_BEGIN` | `u32 photo_id, u8 slot, u32 total_len, u32 crc32` | `u8 slot, u32 next_offset, u32 total_len` | Control，创建临时上传；长度必须正好 105984 |
| `0B` | `PHOTO_DATA` | `u32 offset, bytes[]` | `u32 next_offset` | Image，按偏移接收，可乱序/重发 |
| `0C` | `PHOTO_END` | `u32 crc32` | `u8 slot, u32 photo_id, u32 total_len` | Control，完整性和 CRC 校验后原子提交，选中该槽并刷新 |
| `0D` | `PHOTO_ABORT` | 空 | 空 | Control，丢弃当前临时上传 |
| `0E` | `SET_TODAY_PLAN` | 0..30 字节 UTF-8，无结尾 NUL | 空 | Control，更新并持久化今日计划；空字符串恢复“无” |

`SET_TIMEZONE` 接受字符集 `[A-Za-z0-9_+./-]`，拒绝空段/首尾 `/`；格式通过后交由运行库设置时区。位置与计划校验失败返回 `01`。设置项只有 NVS 保存成功才更新内存。配置改变会调用统一回调并将屏幕刷新请求放入串行队列。

### Wi-Fi 凭据命令 `08`

Payload 按字节紧密排列：`ssid_len:u8`、`password_len:u8`、SSID 原始字节、密码原始字节。SSID 长度为 1..32，密码长度为 0..64；密码长度 0 表示开放网络。清除凭据使用 `[00, 00]`，不带后续字节。总 Payload 长度必须严格等于 `2 + ssid_len + password_len`。拒绝 NUL、ASCII 控制字符和 DEL，以免 NVS 与 ESP-IDF 的 NUL 结尾字段发生截断。SSID/密码字节按 UTF-8/ASCII 原样保存，固件不回传凭据。SSID 的 32 字节上限按字节计，固件会完整传给 Wi-Fi 驱动，不要求在 32 字节 SSID 缓冲区中额外保留 NUL。

命令成功后，固件先提交完整的新配置到 NVS，再更新运行配置并向 Wi-Fi STA 应用凭据/发起连接；NVS 写入失败不会覆盖内存中的旧配置。无线连接是异步的，最终结果由 `GET_INFO` 查询。设备启动时若有已保存 SSID 会自动连接；无凭据时等待小程序发送本命令，不启动 ESPTouch/SmartConfig。连续连接失败后保留凭据并等待新 BLE 配置，不会自动进入 SmartConfig。

### `GET_INFO` 数据格式

状态码之后的数据按顺序紧密排列：

| 字段 | 类型/说明 |
| --- | --- |
| schema | `u8`，当前为 1 |
| mode, rotation, photo_slot | 各 `u8`，枚举见命令 02/03/05 |
| flags | `u8`：bit0 Wi-Fi 已连接、bit1 NTP 已同步、bit2 有有效天气、bit3 天气过期、bit4 已保存 Wi-Fi SSID；其他位保留 |
| Wi-Fi TX、BLE TX | 各有符号 `i8`；Wi-Fi 单位 0.25 dBm，BLE 单位 dBm |
| auto_refresh_sec | `u32`，自动刷新间隔，最小 900 秒 |
| latitude_e7, longitude_e7 | 各 `i32` |
| temperature_c10 | `i16`，摄氏度乘 10；没有有效天气时为 0 |
| weather_updated_unix | `u64`，UTC Unix 秒；无缓存时为 0 |
| timezone | `u8 byte_len` + UTF-8/ASCII 字节 |
| today_plan | `u8 byte_len` + UTF-8 字节；默认 `无` |
| upload_active, upload_slot | 各 `u8`；无活动上传时 slot 为 `FF` |
| upload_photo_id, upload_total_len, upload_next_offset | 各 `u32`；无活动上传时为 0 |

TX 功率为配置/固件请求的有效等级报告，不是射频仪实测值。

## 照片上传与恢复

照片数据为客户端生成的 768×552、2bpp 原始画面，必须是完整的 105984 字节。`PHOTO_BEGIN` 中 CRC32 使用 IEEE/ZIP CRC-32（初值和最终异或均 `FFFFFFFF`，反射多项式 `EDB88320`），并携带固定目标槽位 0..3。照片先写入该槽的非活动 Flash bank；提交前不影响已显示的照片。客户端应按当前协商 MTU 选择数据段，单个 Image GATT 值中的帧不能超过 `MTU - 3`，所以本命令最大图片数据通常为 `MTU - 10 - 4` 字节（ATT 开销 3、协议帧开销 10、偏移 4）。

`PHOTO_DATA` 偏移从 0 开始。接受乱序和重复片段；每次响应给出从 0 起第一个尚未收到的字节位置，可用于滑动窗口/断点续传。上传状态和收到位图保存在 RAM，只能在本次开机进程存活期间（包括 BLE 断连后重连）续传，断电/复位会丢弃临时进度。客户端可重发相同参数的 `PHOTO_BEGIN` 查询当前 offset。`PHOTO_END` 需全部字节到齐，并且请求 CRC、上传开始 CRC 和 Flash 回读 CRC 全部一致；通过后元数据切换到新 bank，旧照片在提交前保持有效。CRC/提交失败应重新查询状态或重新开始上传。成功后当前槽更新为目标槽。

照片分区由 4 个槽、每槽双 bank 组成；NVS 只保存设置和照片槽索引，不保存整张照片。

## 今日计划与显示

主页显示 `今日计划:<内容>`，默认 `今日计划:无`。其 UTF-8 字节长度最多 30，固件保留 `today_plan` NVS 字段。设备不会按日期清空它；小程序负责按需要覆盖或发送空 Payload 恢复默认值。字库是固件内置子集，缺字可能留空。

## 实现与验收提示

- 所有 14 个命令均已接入命令分发和状态码响应；照片命令实际访问 `photos` 分区，普通配置访问 NVS。
- GATT 写入回调只解帧并把命令复制进队列；NVS、网络请求、照片提交不在 NimBLE 写回调中同步执行。
- 设备只允许一个 BLE 连接。响应帧可能分成多条通知；客户端需要一个面向 Notify 特征的协议流解析器，不能假设一条通知就是一条完整帧。
- `GET_INFO`/其它响应只有在 Notify 已订阅且存在 BLE 连接时发送。Notify 失败时固件日志会记录，但不会回滚已经执行成功的命令。
- 显式 `REFRESH` 与配置更新可越过 15 分钟自动刷新门限，但所有刷新仍串行；传输/刷新期间重复请求由刷新队列合并。
