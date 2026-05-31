# new-awtrix3 — AWTRIX3 的 ESP-IDF 6.0 移植版

> 面向 **ESP32-S3 / Ulanzi TC001 32×8 点阵时钟硬件** 的 AWTRIX3 固件移植项目。  
> 本项目将原 AWTRIX3 的 Arduino / PlatformIO 生态实现，重组为标准 **ESP-IDF 6.0.1 + CMake 组件化工程**，目标是在更接近
> ESP-IDF 原生开发方式的基础上保留 AWTRIX3 的核心显示、HTTP、MQTT、Home Assistant、外设与自定义 App 能力。

---

## 目录

- [1. 项目简介](#1-项目简介)
- [2. 当前状态概览](#2-当前状态概览)
- [3. 硬件目标](#3-硬件目标)
- [4. 技术栈与依赖](#4-技术栈与依赖)
- [5. 项目结构](#5-项目结构)
- [6. 启动流程](#6-启动流程)
- [7. 构建、刷写与监视](#7-构建刷写与监视)
- [8. 分区与文件系统](#8-分区与文件系统)
- [9. 运行时配置](#9-运行时配置)
- [10. HTTP API](#10-http-api)
- [11. MQTT 与 Home Assistant](#11-mqtt-与-home-assistant)
- [12. 自定义 App 与通知](#12-自定义-app-与通知)
- [13. 显示、特效与菜单](#13-显示特效与菜单)
- [14. 外设支持](#14-外设支持)
- [15. 已知限制与风险](#15-已知限制与风险)
- [16. 与优秀开源固件的对比和改进建议](#16-与优秀开源固件的对比和改进建议)
- [17. 推荐路线图](#17-推荐路线图)
- [18. 开发规范建议](#18-开发规范建议)
- [19. 常见问题](#19-常见问题)
- [20. 版权与许可说明](#20-版权与许可说明)

---

## 1. 项目简介

`new-awtrix3` 是一个 AWTRIX3 固件移植工程，主要目标是：

1. **脱离 Arduino 运行时**  
   使用 ESP-IDF 原生 API 替代 Arduino `WiFi.h`、`Preferences`、`LittleFS`、`WebServer`、`PubSubClient`、`FastLED` 等依赖。

2. **组件化重构**  
   将原项目按职责拆分为 HAL、存储、显示、核心逻辑、网络、HTTP、外设、配置、事件总线等 ESP-IDF component。

3. **保留 AWTRIX3 的核心用户体验**  
   包括 32×8 点阵显示、时间/日期/温湿度/电量/天气 App、通知、Moodlight、自定义 App、按钮导航、MQTT 控制、Home Assistant 自动发现等。

4. **面向后续工程化维护**  
   通过 CMake、组件依赖、ESP-IDF managed components、NVS/SPIFFS、FreeRTOS 任务模型，为后续加入测试、CI、Web
   UI、OTA、更多硬件板卡和更强并发安全打基础。

本仓库不是简单的“编译脚本移植”，而是一次较完整的运行时迁移：显示驱动、JSON 解析、HTTP 服务、MQTT、mDNS、SPIFFS、按钮、蜂鸣器、配置持久化都已改为
ESP-IDF 体系。

---

## 2. 当前状态概览

根据当前源码和项目内报告，项目已具备以下能力：

| 模块                       |    状态 | 说明                                                 |
|--------------------------|------:|----------------------------------------------------|
| ESP-IDF 工程结构             |     ✅ | 顶层 `CMakeLists.txt` + `main/` + `components/` 标准结构 |
| 目标芯片                     |     ✅ | `CONFIG_IDF_TARGET="esp32s3"`                      |
| Ulanzi TC001 板级定义        |     ✅ | `components/awtrix_hal/boards/ulanzi_s3.h`         |
| LED 点阵显示                 |     ✅ | 32×8 WS2812，基于 `espressif/led_strip` / RMT         |
| NVS 配置                   |     ✅ | `AwtrixConfig` 单例封装持久化配置                           |
| SPIFFS                   |     ✅ | 用于自定义 App、开发配置等文件                                  |
| HTTP 服务                  |     ✅ | `esp_http_server` 封装，提供 Web/API 路由                 |
| MQTT                     |     ✅ | 基于 `espressif/mqtt`，支持控制主题和统计上报                    |
| Home Assistant Discovery |     ✅ | 发布多个 light/sensor/binary_sensor/number 实体          |
| mDNS / UDP Discovery     |     ✅ | 支持局域网发现                                            |
| 按钮                       |     ✅ | 左/右/选择/复位按钮，支持菜单和导航                                |
| 通知                       |     ✅ | 支持 JSON 通知、滚动、颜色、RTTTL 声音触发                        |
| Moodlight                |     ✅ | 支持纯色/色温模式                                          |
| 自定义 App                  |     ✅ | 支持 JSON 自定义页面、绘图 DSL、持久化加载                         |
| 天气 Overlay               |     ✅ | drizzle/rain/snow/storm/thunder/frost 等前景特效        |
| GIF/JPEG                 |    ⚠️ | GIF 解码占位/简化；JPEG 不完整，见已知限制                         |
| Web 配网页面                 |    ⚠️ | 当前为最小内嵌页面，完整可视化配置 UI 仍需完善                          |
| 并发安全                     |    ⚠️ | 部分 DisplayManager 状态已加锁，但全局配置/跨任务状态仍建议继续治理         |
| 自动 OTA                   | ⚠️/待定 | 当前源码入口未看到完整 OTA 路由与启动流程，建议后续明确是否恢复                 |

> 说明：项目内 `PORTING_REPORT.md` 和 `REVIEW_REPORT.md` 记录了移植过程、架构审查和历史状态。README 以当前源码为主，报告可作为深度开发参考。

---

## 3. 硬件目标

当前默认目标硬件是 **Ulanzi TC001 / ESP32-S3 版本**。

板级定义位置：

```text
components/awtrix_hal/boards/ulanzi_s3.h
```

关键参数：

| 项目       | 配置                     |
|----------|------------------------|
| MCU      | ESP32-S3               |
| Flash    | 16 MB                  |
| PSRAM    | Octal PSRAM，80 MHz     |
| 点阵尺寸     | 32 × 8                 |
| LED 数据引脚 | GPIO5                  |
| 矩阵布局     | 4 个 8×8 面板，progressive |
| 左键       | GPIO7                  |
| 右键       | GPIO8                  |
| 选择键      | GPIO10                 |
| 复位键      | GPIO13                 |
| 蜂鸣器      | GPIO15                 |
| 电池 ADC   | GPIO4 / ADC1_CH4       |
| 光敏 ADC   | GPIO6 / ADC1_CH6       |
| I2C SCL  | GPIO10                 |
| I2C SDA  | GPIO11                 |

> 注意：当前 `BUTTON_SELECT_PIN` 和 `I2C_SCL_PIN` 都使用 GPIO10。若后续实际接入 I2C 传感器，应重新核对硬件原理图和引脚复用，避免按钮与
> I2C 冲突。

---

## 4. 技术栈与依赖

### 4.1 ESP-IDF 版本

项目面向：

```text
ESP-IDF >= 6.0
当前开发环境：ESP-IDF 6.0.1
```

本机技能配置中使用的 IDF 路径为：

```text
C:/esp/v6.0.1/esp-idf/
```

ESP-IDF 工具路径：

```text
C:/Espressif/tools/
```

### 4.2 managed components

`main/idf_component.yml` 中声明的依赖：

```yaml
dependencies:
  idf: ">=6.0"
  espressif/button: ">=3.0"
  espressif/led_strip: ">=2.0"
  espressif/cjson: ">=1.0"
  espressif/mdns: ">=1.0"
  espressif/mqtt: ">=1.0"
```

用途概览：

| 依赖                                | 用途                   |
|-----------------------------------|----------------------|
| `espressif/button`                | 物理按键事件抽象             |
| `espressif/led_strip`             | WS2812 LED 点阵 RMT 驱动 |
| `espressif/cjson`                 | HTTP/MQTT/配置 JSON 解析 |
| `espressif/mdns`                  | 局域网服务发现              |
| `espressif/mqtt`                  | MQTT 客户端             |
| ESP-IDF 内建 `spiffs`               | 文件系统                 |
| ESP-IDF 内建 `nvs_flash`            | 配置持久化                |
| ESP-IDF 内建 `esp_http_server`      | HTTP API 服务          |
| ESP-IDF 内建 `esp_wifi`/`esp_netif` | Wi-Fi 和网络栈           |

### 4.3 Arduino 依赖替换关系

| 原 Arduino/PlatformIO 生态     | ESP-IDF 移植实现                                     |
|-----------------------------|--------------------------------------------------|
| FastLED / FastLED_NeoMatrix | 自研 `Matrix` C++ 类 + `led_matrix.c` + `led_strip` |
| ArduinoJson                 | `cJSON`                                          |
| PubSubClient                | `esp-mqtt`                                       |
| WebServer / FSWebServer     | `esp_http_server` + `AwtrixHttpServer` 封装        |
| WiFi.h                      | `esp_wifi` + `esp_netif` + `esp_event`           |
| LittleFS                    | SPIFFS                                           |
| Preferences                 | NVS                                              |
| Bounce2                     | `espressif/button`                               |
| mDNS                        | `espressif/mdns`                                 |
| Ticker                      | FreeRTOS tick / ESP timer                        |
| Arduino `String` 生态         | C/C++ 标准库与 ESP-IDF C API 混用                      |

---

## 5. 项目结构

```text
new-awtrix3/
├── CMakeLists.txt                 # 顶层 ESP-IDF 工程定义
├── partitions.csv                 # 自定义分区表
├── sdkconfig.defaults             # 默认 sdkconfig
├── dependencies.lock              # managed components 锁定信息
├── PORTING_REPORT.md              # Arduino → ESP-IDF 移植报告
├── REVIEW_REPORT.md               # 架构与代码审查报告
├── data/                          # SPIFFS 镜像数据目录，当前可放 dev.json 等资源
├── main/
│   ├── app_main.cpp               # 固件入口、初始化顺序、主循环
│   ├── CMakeLists.txt
│   └── idf_component.yml          # ESP-IDF 组件依赖声明
└── components/
    ├── awtrix_config/             # 全局配置 AwtrixConfig、NVS/SPIFFS 配置加载
    ├── awtrix_core/               # DisplayManager、UI、特效、菜单、字典、图标、Apps
    ├── awtrix_display/            # Matrix、字体、LED HAL、GIF 播放器占位/简化实现
    ├── awtrix_events/             # 轻量事件总线，解耦 core/periphery
    ├── awtrix_hal/                # 板级 HAL、GPIO、工具函数、Ulanzi S3 定义
    ├── awtrix_http/               # HTTP server 封装与 REST API 路由
    ├── awtrix_network/            # Wi-Fi、SNTP、mDNS、UDP Discovery、MQTT、ArtNet
    ├── awtrix_periphery/          # 按键、蜂鸣器、DFPlayer、RTTTL、Power、IO
    └── awtrix_storage/            # NVS/SPIFFS 包装
```

### 5.1 组件职责说明

#### `awtrix_hal`

底层硬件抽象层，负责：

- 板卡类型定义；
- 矩阵尺寸、GPIO、ADC、I2C、蜂鸣器引脚；
- 通用日志 tag；
- 基础初始化函数。

#### `awtrix_storage`

负责存储抽象：

- NVS 初始化；
- SPIFFS 挂载；
- 配置读写辅助。

#### `awtrix_display`

负责显示底层：

- WS2812 点阵驱动；
- `Matrix` C++ 绘图类；
- AWTRIX 字体数据；
- GIF 播放器接口。

#### `awtrix_core`

核心业务层：

- `DisplayManager`；
- App 列表管理；
- 自定义 App 解析和渲染；
- 通知队列；
- Moodlight；
- 特效和 Overlay；
- 菜单管理；
- 绘图 DSL；
- 内置 App：Time / Date / Temp / Hum / Bat / Weather。

#### `awtrix_network`

网络能力：

- Wi-Fi STA/AP；
- SNTP 时间同步；
- mDNS；
- UDP Discovery；
- MQTT 控制和状态上报；
- ArtNet。

#### `awtrix_http`

HTTP 服务：

- `AwtrixHttpServer` 封装；
- REST API 注册；
- Basic Auth 检查；
- 文件上传/保存/删除；
- 简化 Web 页面。

#### `awtrix_periphery`

外设：

- 物理按键；
- 蜂鸣器；
- RTTTL；
- DFPlayer Mini 驱动；
- 电源/亮度/外设 tick。

#### `awtrix_events`

事件总线组件，用于降低 `awtrix_core` 与 `awtrix_periphery` 的直接耦合。当前通过 `std::function` 槽位连接
RTTTL、声音、R2D2、亮度、通知等动作。

---

## 6. 启动流程

固件入口为：

```text
main/app_main.cpp
```

当前启动顺序概览：

1. **Boot delay**  
   设置蜂鸣器/相关 GPIO，等待硬件稳定。

2. **HAL 初始化**  
   调用 `awtrix_init()`。

3. **NVS + 配置加载**  
   初始化设置存储，调用 `AwtrixConfig::get().load()`。

4. **SPIFFS 挂载**  
   调用 `awtrix_fs_mount()`。

5. **Matrix 初始化**  
   创建 `Matrix` 对象，设置亮度、旋转、清屏。

6. **DisplayManager 初始化**  
   绑定 Matrix，加载内置 Apps。

7. **Boot 画面**  
   在 32×8 点阵上显示 `AWTRIX` 和版本号。

8. **Wi-Fi 启动**  
   非阻塞启动 Wi-Fi。

9. **等待 Wi-Fi ready**  
   显示进度动画；连接失败时进入 AP 模式相关状态。

10. **SNTP 初始化**  
    使用 `CONFIG.ntp_server` 与 `CONFIG.ntp_tz`。

11. **mDNS / UDP Discovery**  
    发布 HTTP 与 AWTRIX 服务。

12. **外设初始化**  
    注册按钮回调，初始化外设，播放启动音。

13. **事件总线接线**  
    将 core 与 periphery 的动作通过 `AwtrixEventBus` 连接。

14. **加载自定义 Apps**  
    从 `/spiffs/CUSTOMAPPS/*.json` 恢复用户自定义页面。

15. **HTTP Server 启动**  
    创建 `AwtrixHttpServer`，注册 REST 路由。

16. **MQTT 启动**  
    调用 `awtrix_mqtt_init()`。

17. **状态显示**  
    若联网成功显示 IP:Port，否则显示 AP 信息。

18. **主循环**  
    每约 16 ms tick 一次，包含：
    - `DisplayManager::tick()`；
    - `PeripheryManager::tick()`；
    - RTTTL tick；
    - MQTT tick；
    - UDP discovery 周期响应；
    - 新年提示检查；
    - 当前 App 周期发布。

---

## 7. 构建、刷写与监视

### 7.1 准备环境

在 Windows PowerShell 中加载 ESP-IDF 6.0.1 环境：

```powershell
. 'C:/Espressif/tools/Microsoft.v6.0.1.PowerShell_profile.ps1'
```

进入项目目录：

```powershell
cd F:/new-awtrix3
```

### 7.2 设置目标芯片

通常 `sdkconfig.defaults` 已指定：

```text
CONFIG_IDF_TARGET="esp32s3"
```

如果需要手动设置：

```powershell
idf.py set-target esp32s3
```

### 7.3 编译

```powershell
idf.py build
```

成功后主要产物位于：

```text
build/awtrix3.bin
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
```

### 7.4 刷写

替换 `COM3` 为实际串口：

```powershell
idf.py -p COM3 flash
```

### 7.5 监视串口日志

```powershell
idf.py -p COM3 monitor
```

或刷写后直接监视：

```powershell
idf.py -p COM3 flash monitor
```

### 7.6 构建配置菜单

```powershell
idf.py menuconfig
```

常用关注项：

- Flash size；
- PSRAM；
- SPIFFS；
- Wi-Fi buffer；
- HTTP server header/URI 长度；
- 日志等级；
- FreeRTOS tick rate；
- main task stack。

---

## 8. 分区与文件系统

当前 `partitions.csv`：

```csv
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x300000,
fs,       data, spiffs,  0x310000,0xA0000,
```

| 分区         | 类型          | 用途        |     大小 |
|------------|-------------|-----------|-------:|
| `nvs`      | data/nvs    | 配置持久化     |  24 KB |
| `phy_init` | data/phy    | PHY 初始化数据 |   4 KB |
| `factory`  | app/factory | 固件主体      |   3 MB |
| `fs`       | data/spiffs | 文件系统      | 640 KB |

### 8.1 SPIFFS 用途

SPIFFS 挂载路径通常为：

```text
/spiffs
```

当前主要用途：

- `/spiffs/dev.json`：一次性开发/初始化配置；
- `/spiffs/dev.json.applied`：`dev.json` 已应用后的重命名文件；
- `/spiffs/CUSTOMAPPS/*.json`：用户自定义 App；
- Web 文件上传/保存相关资源。

### 8.2 烧录 SPIFFS 镜像

如果 `data/` 目录中准备了 SPIFFS 内容，可使用 ESP-IDF 常见命令：

```powershell
idf.py build
idf.py -p COM3 flash
```

若项目配置了 SPIFFS 镜像生成，也可使用：

```powershell
idf.py -p COM3 spiffs-flash
```

> 如果 `spiffs-flash` 不可用，需要检查是否在 CMake 中声明了 SPIFFS 镜像生成规则。

---

## 9. 运行时配置

配置核心类型：

```cpp
AwtrixConfig
```

位置：

```text
components/awtrix_config/awtrix_globals.h
components/awtrix_config/awtrix_globals.cpp
```

便捷宏：

```cpp
#define CONFIG (AwtrixConfig::get())
```

### 9.1 配置类别

| 类别             | 典型字段                                                                        |
|----------------|-----------------------------------------------------------------------------|
| 身份信息           | `uniqueID`, `hostname`, `version`                                           |
| 网络             | `net_ip`, `net_gw`, `net_sn`, `net_static`, `ap_mode`                       |
| MQTT           | `mqtt_host`, `mqtt_user`, `mqtt_pass`, `mqtt_prefix`, `mqtt_port`           |
| Home Assistant | `ha_discovery`, `ha_prefix`                                                 |
| HTTP           | `auth_user`, `auth_pass`, `web_port`                                        |
| NTP            | `ntp_server`, `ntp_tz`                                                      |
| 显示             | `brightness`, `auto_brightness`, `rotate_screen`, `mirror_display`, `gamma` |
| App 可见性        | `showTime`, `showDate`, `showTemp`, `showHum`, `showBat`, `showWeather`     |
| 格式化            | `timeFormat`, `dateFormat`, `showSeconds`, `showWeekday`                    |
| 传感器            | `currentTemp`, `currentHum`, `currentLux`, `batteryPercent`                 |
| 声音             | `soundActive`, `soundVolume`, `bootSound`                                   |
| 特效             | `bgEffect`, `globalOverlay`                                                 |
| 运行状态           | `currentApp`, `artnetMode`, `moodlightMode`, `receivedMessages`             |

### 9.2 `dev.json` 一次性初始化

启动时 `AwtrixConfig::loadDevSettings()` 会尝试读取：

```text
/spiffs/dev.json
```

如果存在，会解析部分字段并写入 NVS，然后将文件重命名为：

```text
/spiffs/dev.json.applied
```

示例：

```json
{
  "HOSTNAME": "awtrix-bedroom",
  "NTP_SERVER": "pool.ntp.org",
  "NTP_TZ": "CST-8",
  "MQTT_HOST": "192.168.1.10",
  "MQTT_PORT": 1883,
  "MQTT_USER": "awtrix",
  "MQTT_PASS": "请勿提交真实密码",
  "HA_DISCOVERY": true,
  "HA_PREFIX": "homeassistant",
  "BOOT_SOUND": ""
}
```

> 安全提醒：不要把包含真实 Wi-Fi、MQTT、HTTP Auth 密码的 `dev.json` 提交到仓库。

### 9.3 默认值举例

当前默认配置包括：

| 字段                                              | 默认值             |
|-------------------------------------------------|-----------------|
| `hostname`                                      | `awtrix`        |
| `web_port`                                      | `80`            |
| `mqtt_port`                                     | `1883`          |
| `ha_prefix`                                     | `homeassistant` |
| `ntp_server`                                    | `pool.ntp.org`  |
| `brightness`                                    | `120`           |
| `min_brightness`                                | `10`            |
| `max_brightness`                                | `255`           |
| `matrix_fps`                                    | `42`            |
| `showTime`                                      | `true`          |
| `showDate/showTemp/showHum/showBat/showWeather` | `false`         |
| `timeFormat`                                    | `%H:%M:%S`      |
| `dateFormat`                                    | `%d.%m.%y`      |
| `soundActive`                                   | `true`          |
| `soundVolume`                                   | `25`            |

---

## 10. HTTP API

HTTP 服务由 `awtrix_http` 提供：

```text
components/awtrix_http/awtrix_http.cpp
components/awtrix_http/awtrix_api.cpp
```

### 10.1 GET 路由

当前注册的 GET 路由：

| 路由                 | 用途             |
|--------------------|----------------|
| `/`                | 简化首页           |
| `/version`         | 固件版本           |
| `/api/loop`        | 当前 App loop 信息 |
| `/api/effects`     | 可用背景特效列表       |
| `/api/transitions` | 可用转场列表         |
| `/api/apps`        | App 列表         |
| `/api/settings`    | 当前配置 JSON      |
| `/api/stats`       | 当前统计信息         |
| `/api/screen`      | 当前屏幕像素 JSON    |
| `/fullscreen`      | 全屏页面           |
| `/backup`          | 备份页面/接口入口      |
| `/list`            | 文件列表           |
| `/edit`            | 文件编辑入口         |

### 10.2 POST 路由

当前注册的 POST 路由：

| 路由                    | 用途             |
|-----------------------|----------------|
| `/api/power`          | 开关屏/电源状态       |
| `/api/sleep`          | 睡眠/休眠相关控制      |
| `/api/notify`         | 发送通知           |
| `/api/notify/dismiss` | 关闭通知           |
| `/api/apps`           | 更新 App 列表      |
| `/api/switch`         | 切换到指定 App      |
| `/api/nextapp`        | 下一个 App        |
| `/api/previousapp`    | 上一个 App        |
| `/api/settings`       | 更新配置           |
| `/api/reboot`         | 重启设备           |
| `/api/rtttl`          | 播放 RTTTL       |
| `/api/sound`          | 声音/DFPlayer 动作 |
| `/api/moodlight`      | Moodlight 模式   |
| `/api/erase`          | 擦除相关数据         |
| `/api/resetSettings`  | 重置设置           |
| `/api/reorder`        | App 重排序        |
| `/api/custom`         | 创建/更新自定义 App   |
| `/api/indicator1`     | 指示灯 1          |
| `/api/indicator2`     | 指示灯 2          |
| `/api/indicator3`     | 指示灯 3          |
| `/api/r2d2`           | R2D2 声音效果      |
| `/upload`             | 文件上传           |
| `/delete`             | 文件删除           |
| `/save`               | 文件保存           |

### 10.3 Basic Auth

项目中实现了 HTTP Basic Auth 网关。建议所有写操作、重启、擦除、文件写入、声音播放、通知等敏感动作都启用鉴权。

配置字段：

```text
auth_user
auth_pass
```

安全建议：

- 不要使用空密码暴露到不可信网络；
- 不要将 HTTP 直接暴露到公网；
- 如需远程访问，建议通过 VPN、反向代理鉴权或 Home Assistant 内网集成；
- 后续可考虑加入 token auth 或 session auth，替代裸 Basic Auth。

### 10.4 常见调用示例

发送通知：

```bash
curl -X POST http://awtrix.local/api/notify -H "Content-Type: application/json" -d '{"text":"你好 AWTRIX","color":"#00ff88","duration":5}'
```

切换 Moodlight：

```bash
curl -X POST http://awtrix.local/api/moodlight -H "Content-Type: application/json" -d '{"color":"#3366ff","brightness":120}'
```

设置亮度：

```bash
curl -X POST http://awtrix.local/api/settings -H "Content-Type: application/json" -d '{"BRI":80}'
```

切换下一个 App：

```bash
curl -X POST http://awtrix.local/api/nextapp
```

获取统计：

```bash
curl http://awtrix.local/api/stats
```

---

## 11. MQTT 与 Home Assistant

MQTT 实现位于：

```text
components/awtrix_network/awtrix_mqtt.cpp
```

### 11.1 MQTT 前缀

运行时前缀由配置决定：

```text
CONFIG.mqtt_prefix
```

若未显式配置，通常基于 `uniqueID` 或默认 `awtrix`。

以下示例假设前缀为：

```text
awtrix
```

### 11.2 订阅主题

代码注释与实现中列出的控制主题包括：

```text
awtrix/notify
awtrix/notify/dismiss
awtrix/apps
awtrix/switch
awtrix/sendscreen
awtrix/settings
awtrix/r2d2
awtrix/nextapp
awtrix/previousapp
awtrix/rtttl
awtrix/power
awtrix/sleep
awtrix/indicator1
awtrix/indicator2
awtrix/indicator3
awtrix/moodlight
awtrix/reboot
awtrix/sound
awtrix/custom/+
```

### 11.3 发布主题

周期统计主题包括：

```text
awtrix/stats/version
awtrix/stats/uptime
awtrix/stats/temperature
awtrix/stats/humidity
awtrix/stats/battery
awtrix/stats/lux
awtrix/stats/effects
awtrix/stats/transitions
awtrix/stats/ram
awtrix/stats/brightness
awtrix/stats/rssi
awtrix/stats/ip
awtrix/stats/currentApp
```

按钮状态：

```text
awtrix/stats/btn_left
awtrix/stats/btn_select
awtrix/stats/btn_right
```

屏幕 JSON：

```text
awtrix/screen
```

### 11.4 MQTT 示例

发送通知：

```bash
mosquitto_pub -h 192.168.1.10 -t awtrix/notify -m '{"text":"MQTT Hello","color":"#ff8800"}'
```

切换 App：

```bash
mosquitto_pub -h 192.168.1.10 -t awtrix/switch -m '{"name":"Time"}'
```

更新设置：

```bash
mosquitto_pub -h 192.168.1.10 -t awtrix/settings -m '{"BRI":100}'
```

创建自定义 App：

```bash
mosquitto_pub -h 192.168.1.10 -t awtrix/custom/demo -m '{"text":"Demo","color":"#00ff00","duration":30}'
```

### 11.5 Home Assistant Discovery

当配置：

```text
ha_discovery = true
```

设备会向 `ha_prefix` 下发布 Home Assistant discovery payload。默认前缀：

```text
homeassistant
```

当前 Discovery 覆盖的实体类型包括：

- 主点阵 light；
- 指示灯 light；
- 温度 sensor；
- 湿度 sensor；
- 电量 sensor；
- 光照 sensor；
- uptime sensor；
- RSSI sensor；
- IP sensor；
- version sensor；
- free RAM sensor；
- current app sensor；
- 左/中/右按钮 binary sensor；
- brightness number。

改进建议见后文 Home Assistant 章节。

---

## 12. 自定义 App 与通知

### 12.1 内置 App

当前 `DisplayManager` 支持的内置 App：

| App       | 说明        |
|-----------|-----------|
| `Time`    | 时间显示      |
| `Date`    | 日期显示      |
| `Temp`    | 温度显示      |
| `Hum`     | 湿度显示      |
| `Bat`     | 电池百分比显示   |
| `Weather` | 天气/温度文本显示 |

是否显示由配置字段控制：

```text
showTime
showDate
showTemp
showHum
showBat
showWeather
```

### 12.2 通知 JSON

通知由：

```cpp
DisplayManager::generateNotification(uint8_t source, const char *json)
```

处理。

常见字段示例：

```json
{
  "text": "门已打开",
  "color": "#ff0000",
  "duration": 8,
  "repeat": 1,
  "noScroll": false,
  "rtttl": "notify:d=4,o=5,b=120:c6,g6"
}
```

### 12.3 自定义 App JSON

自定义 App 可通过 HTTP `/api/custom` 或 MQTT `custom/<name>` 创建，也可持久化到：

```text
/spiffs/CUSTOMAPPS/<name>.json
```

典型示例：

```json
{
  "text": "HELLO",
  "color": "#00ffcc",
  "center": true,
  "duration": 30,
  "effect": 0,
  "rainbow": false,
  "blink": 0,
  "progress": 75,
  "progressC": "#00ff00",
  "progressBC": "#333333"
}
```

### 12.4 绘图 DSL

项目支持 `drawInstructions`，用于以 JSON 描述点、线、矩形、圆、文本和调色板像素。

示例：

```json
{
  "draw": [
    {"db": [0, 0, 32, 8, "#000020"]},
    {"dl": [0, 7, 31, 0, "#00ffff"]},
    {"dt": [2, 1, "#ffffff", "Hi"]},
    {"dp": [31, 7, "#ff0000"]}
  ]
}
```

常见 opcode：

| opcode | 作用                    |
|--------|-----------------------|
| `dp`   | draw pixel            |
| `dl`   | draw line             |
| `db`   | draw filled rectangle |
| `dr`   | draw rectangle        |
| `df`   | draw filled circle    |
| `dc`   | draw circle           |
| `dt`   | draw text             |
| `dlp`  | 定义调色板                 |
| `dpa`  | 使用调色板画点               |

当前实现还包含编译缓存路径：解析自定义 App 时将 drawInstructions 编译为 POD `DrawOp`，避免每帧 `cJSON_Parse`
带来的堆分配压力。这是比原始“每帧解析 JSON”更适合 MCU 的做法。

---

## 13. 显示、特效与菜单

### 13.1 Matrix

`Matrix` 类提供类似 Adafruit GFX 的接口：

- `drawPixel`；
- `drawLine`；
- `drawRect` / `fillRect`；
- `drawCircle` / `fillCircle`；
- `setCursor`；
- `print`；
- `setTextColor`；
- `clear`；
- `show`；
- `setBrightness`；
- `setRotation`。

### 13.2 背景特效

当前配置枚举中至少包含：

```cpp
BG_NONE
BG_RAINBOW
BG_STARS
```

实际可用特效列表以 `/api/effects` 和 `effects_core.cpp` 为准。

### 13.3 Overlay

支持的前景 Overlay：

```cpp
OVERLAY_NONE
OVERLAY_TIME
OVERLAY_DRIZZLE
OVERLAY_RAIN
OVERLAY_SNOW
OVERLAY_STORM
OVERLAY_THUNDER
OVERLAY_FROST
```

天气 Overlay 会叠加在通知或自定义 App 上，例如雨、雪、风暴、雷电、冰霜等。

### 13.4 按钮交互

默认交互：

| 按钮     | 非菜单状态        | 菜单状态   |
|--------|--------------|--------|
| 左键     | 上一个 App      | 左移     |
| 右键     | 下一个 App      | 右移     |
| 选择键短按  | 选择/关闭通知      | 确认     |
| 选择键长按  | 打开菜单         | 菜单长按动作 |
| 复位键超长按 | 恢复出厂/擦除配置后重启 | 同左     |

支持 `swap_buttons`：设备倒装时可以逻辑交换左右键。

支持 `rotate_screen`：可将屏幕旋转 180°。

---

## 14. 外设支持

外设模块位于：

```text
components/awtrix_periphery/
```

当前涉及：

- 按键；
- 蜂鸣器；
- RTTTL 播放；
- DFPlayer Mini 驱动；
- 电源控制；
- 亮度/传感器 tick；
- R2D2 风格音效。

### 14.1 声音

声音配置：

```text
soundActive
soundVolume
bootSound
buzzerVolume
```

RTTTL 可通过 HTTP 或 MQTT 触发。

### 14.2 传感器

当前配置保留了温度、湿度、光照、电池等字段：

```text
currentTemp
currentHum
currentLux
batteryPercent
ldrRaw
batteryRaw
```

但真实温湿度传感器驱动仍需要结合硬件进一步完善。

---

## 15. 已知限制与风险

### 15.1 GIF 解码仍不完整

`GifPlayer.cpp` 当前为占位/简化实现，未完整实现 GIF89a LZW 解码。

影响：

- 用户上传 `.gif` 自定义 App 时，动画能力不足；
- 静态图标和基础绘制不受影响。

建议：

- 引入轻量 GIF 解码器；
- 或在上传阶段将 GIF 预转换为帧序列/RGB565；
- 或设计 AWTRIX 专用帧格式，减少 MCU 端解码负担。

### 15.2 JPEG 解码不完整

当前 `drawJPG` 更偏向消费已有 RGB565 buffer，未完整接入 JPEG 解码。

建议：

- 接入 ESP-IDF 可用的轻量 JPEG 解码库；
- 或限制 API 输入为 BMP/RGB565/自定义 raw frame；
- 对大图片做尺寸、格式和内存限制。

### 15.3 Web UI 仍偏最小实现

当前 HTTP 首页与配置页面更像调试入口，不是完整产品级 Web UI。

建议：

- 使用 `data/` 目录存放压缩后的静态 Web 前端；
- 提供 Wi-Fi、MQTT、HA、显示、App 排序、自定义 App 编辑器；
- 增加上传资源管理；
- 前端资源 gzip + cache；
- 后端 API 保持稳定。

### 15.4 Captive Portal 缺失

首次配网体验不完整。

建议：

- AP 模式下启动 DNS server；
- 将所有域名解析到设备 IP；
- 自动跳转配置页；
- 配网成功后自动重启或切换 STA。

### 15.5 并发安全仍需继续审计

ESP-IDF 中 HTTP server、MQTT task、按钮回调、主循环、ArtNet 等可能并发访问共享状态。

当前代码已经在 `DisplayManager` 的部分路径中引入锁，但仍建议重点审计：

- `CONFIG` 全局单例的多任务读写；
- `m_apps` / `m_customApps` / `m_notifications` 生命周期；
- MQTT pending subscription vector；
- ArtNet 与主显示循环同时写 Matrix；
- 声音/外设事件与 HTTP/MQTT 同时触发；
- Basic Auth 和文件上传路径的边界处理。

### 15.6 OTA 状态需要重新明确

项目报告中提到过 OTA 方案或 OTA 移除历史，但当前 `app_main.cpp` 和 HTTP 路由表中未体现完整 OTA 启动/更新闭环。

建议项目维护者明确：

1. 如果不支持 OTA：在 README、API 和 Web UI 中明确删除更新入口；
2. 如果要支持 OTA：使用双 OTA 分区表、HTTPS 校验、版本回滚和签名校验；
3. 不建议在仅 factory 分区布局下贸然加入 OTA。

当前分区表只有 `factory` app 分区，不适合标准 A/B OTA。

### 15.7 安全边界仍较弱

当前 Basic Auth 可用，但如果设备暴露在不可信网络，仍存在风险。

建议：

- 写接口默认强制鉴权；
- 禁止空密码暴露敏感动作；
- 限制文件上传大小和路径；
- 对 JSON body 设置最大长度；
- 对 MQTT 控制主题增加可选鉴权/ACL 说明；
- 禁止日志打印密码、token、Wi-Fi 密码和 MQTT 密码。

---

## 16. 与优秀开源固件的对比和改进建议

这里结合 AWTRIX3、WLED、ESPHome、Tasmota、OpenMQTTGateway、ESP-IDF 官方示例等优秀开源项目的常见实践，给出本项目后续改进方向。

### 16.1 借鉴 WLED：更成熟的显示效果与 Web 配置体验

WLED 的优秀点：

- 丰富的灯效系统；
- Web UI 易用；
- Preset / Playlist 机制成熟；
- JSON API 稳定；
- UDP/实时控制生态丰富。

本项目可借鉴：

1. **Preset 机制**  
   将 Moodlight、自定义 App、亮度、Overlay、颜色、滚动速度组合成 preset。

2. **Playlist / App Playlist**  
   当前 App loop 可进一步升级为可配置播放列表：
    - 每个 App 独立 duration；
    - 条件显示，如仅电池低时显示 Bat；
    - 时间段规则，如夜间自动低亮。

3. **效果参数化**  
   每个背景特效和 Overlay 支持速度、密度、颜色、方向等参数，而不是仅枚举。

4. **WebSocket 实时预览**  
   Web UI 修改设置后无需刷新，通过 WebSocket 同步当前屏幕状态。

### 16.2 借鉴 ESPHome：声明式配置与 Home Assistant 深度集成

ESPHome 的优秀点：

- YAML 声明式配置；
- 与 Home Assistant 集成极佳；
- 实体模型清晰；
- OTA、日志、诊断体验好。

本项目可借鉴：

1. **实体模型标准化**  
   Home Assistant discovery payload 中明确 device class、state class、availability topic。

2. **Availability Topic**  
   增加：
   ```text
   awtrix/status = online/offline
   ```
   并设置 MQTT Last Will。

3. **诊断实体**  
   上报：
    - free heap；
    - minimum free heap；
    - uptime；
    - Wi-Fi RSSI；
    - IP；
    - firmware version；
    - reset reason；
    - PSRAM 使用量。

4. **配置导入导出**  
   支持完整 JSON backup/restore，便于多设备复制配置。

### 16.3 借鉴 Tasmota：健壮的命令体系和设备管理

Tasmota 的优秀点：

- MQTT 命令体系稳定；
- 控制、状态、遥测主题分层清晰；
- Web 配网和恢复模式成熟；
- 串口日志对排障友好。

本项目可借鉴：

1. **主题分层**

建议长期演进为：

```text
awtrix/cmnd/...
awtrix/stat/...
awtrix/tele/...
```

或保持 AWTRIX 兼容主题，同时新增标准化主题。

2. **统一命令响应**

控制命令执行后发布响应：

```text
awtrix/stat/result
```

例如：

```json
{"command":"brightness","ok":true,"value":120}
```

3. **故障恢复模式**

当配置损坏、Wi-Fi 连续失败、HTTP 初始化失败时进入安全 AP 模式。

### 16.4 借鉴 ESP-IDF 官方示例：更规范的任务与资源生命周期

ESP-IDF 官方示例通常强调：

- 明确 event loop；
- 资源创建/销毁成对；
- 错误码检查；
- 组件边界清晰；
- FreeRTOS 同步原语合理使用。

本项目可改进：

1. **统一事件队列**  
   HTTP/MQTT/按钮/ArtNet 不直接改 DisplayManager，而是投递事件到主显示任务。

2. **单写者模型**  
   让 DisplayManager 的状态只由一个任务写，其他任务只发消息，避免大量锁。

3. **错误处理统一化**  
   所有 `esp_err_t` 统一检查并记录。

4. **组件依赖瘦身**  
   减少横向依赖，例如 HTTP 不直接调用 MQTT，改为事件总线。

### 16.5 借鉴原 AWTRIX3：保持 API 兼容与生态兼容

AWTRIX3 的价值在于已有生态：

- 用户已有 MQTT/HTTP 自动化；
- Home Assistant 蓝图/自动化；
- 自定义 App JSON；
- 通知格式；
- 社区使用经验。

本项目应尽量：

1. 保持 HTTP/MQTT payload 兼容；
2. 记录与原版不兼容之处；
3. 提供迁移表；
4. 给出原版配置导入方式；
5. 避免随意更改字段名。

---

## 17. 推荐路线图

### P0：稳定性与安全性

1. **全局并发模型收敛**
    - DisplayManager 状态单写者；
    - HTTP/MQTT 只投递事件；
    - CONFIG 写入采用快照或 mutex；
    - Matrix 写入集中到显示任务。

2. **敏感 API 鉴权全覆盖**
    - `/api/reboot`；
    - `/api/erase`；
    - `/api/resetSettings`；
    - `/upload`、`/delete`、`/save`；
    - `/api/settings`；
    - `/api/custom`。

3. **文件上传安全**
    - 限制路径穿越；
    - 限制大小；
    - 限制扩展名；
    - 写入临时文件后原子替换。

4. **崩溃恢复**
    - 记录 reset reason；
    - 连续启动失败进入安全模式；
    - 提供恢复出厂入口。

### P1：用户体验

1. **完整 Web UI**
    - Wi-Fi 配置；
    - MQTT 配置；
    - Home Assistant 开关；
    - App 排序；
    - 自定义 App 编辑器；
    - 实时屏幕预览；
    - 备份/恢复。

2. **Captive Portal**
    - 首次启动自动 AP；
    - 手机自动弹出配网页面；
    - 配网后保存并重启。

3. **更完整的 Home Assistant Discovery**
    - availability；
    - device identifiers；
    - suggested area；
    - diagnostics；
    - 可配置 entity category。

4. **WebSocket / SSE**
    - 实时日志；
    - 实时屏幕；
    - 实时状态。

### P2：显示能力

1. **完整 GIF 解码**；
2. **JPEG/PNG 支持或上传端预转换**；
3. **更多背景特效**；
4. **特效参数化**；
5. **字体/图标管理**；
6. **更好的中文显示策略**。

> 32×8 点阵显示中文天然受限。若要支持中文，建议采用超小点阵字库、拼音/图标替代、滚动显示或服务端预渲染 bitmap。

### P3：工程化

1. **单元测试**
    - JSON 配置解析；
    - drawInstructions 编译；
    - MQTT topic 解析；
    - HTTP body 边界。

2. **Host-based tests**
   使用 ESP-IDF host test 或普通 CMake 测试分离纯逻辑。

3. **CI**
    - `idf.py build`；
    - 格式检查；
    - 静态分析；
    - size report。

4. **文档自动生成**
    - API schema；
    - MQTT topic 表；
    - 配置字段表。

---

## 18. 开发规范建议

### 18.1 组件依赖原则

建议长期保持依赖方向：

```text
main
 ├── awtrix_http
 ├── awtrix_network
 ├── awtrix_core
 ├── awtrix_periphery
 ├── awtrix_config
 ├── awtrix_storage
 ├── awtrix_display
 └── awtrix_hal
```

更理想的层级：

```text
应用层:     main / HTTP / MQTT
业务层:     core / menu / app / notification
服务层:     config / storage / events
硬件层:     display / periphery / hal
外部依赖:   esp_http_server / esp_wifi / mqtt / led_strip / cjson
```

原则：

- 底层不要 include 高层头文件；
- HTTP 不直接调用 MQTT；
- MQTT 不直接操作 Matrix；
- 外设只产生事件，不直接改 UI；
- DisplayManager 的状态修改集中处理。

### 18.2 JSON 解析规范

建议：

- 所有 JSON body 限制最大长度；
- 所有字段做类型检查；
- 对字符串长度做限制；
- 颜色解析统一函数；
- 对未知字段保持兼容但记录 debug 日志；
- 避免在 60 Hz 渲染路径中解析 JSON。

当前 drawInstructions 编译缓存是正确方向，应继续扩大到其他热路径。

### 18.3 日志规范

建议使用统一 tag：

```text
system
wifi
mqtt
http
display
storage
periphery
```

不要打印：

- Wi-Fi 密码；
- MQTT 密码；
- HTTP Basic Auth 密码；
- token；
- 用户隐私 payload。

### 18.4 内存规范

ESP32-S3 虽然有 PSRAM，但实时显示路径仍应避免频繁分配。

建议：

- 主循环不分配；
- 渲染路径不 `cJSON_Parse`；
- 大 buffer 放 PSRAM 时评估 DMA/速度限制；
- HTTP 上传使用流式处理；
- MQTT payload 限制长度；
- 记录 heap watermark。

### 18.5 C/C++ 混用规范

当前项目 C 与 C++ 混用较多，建议：

- C API 暴露给跨组件调用；
- C++ 类用于内部实现；
- 头文件明确 `extern "C"`；
- 避免在 C 头文件中暴露 C++ 类型；
- STL 容器不要跨 C ABI 边界。

---

## 19. 常见问题

### Q1：为什么使用 ESP-IDF 而不是继续 Arduino？

ESP-IDF 更适合长期固件工程维护，优势包括：

- 原生 FreeRTOS；
- 更清晰的组件依赖；
- 更强的网络、TLS、OTA、NVS、分区管理；
- 更容易做 CI 和量产配置；
- 更接近 Espressif 官方支持路径。

代价是：

- 代码复杂度更高；
- Arduino 生态库不能直接复用；
- 需要自己处理更多底层细节。

### Q2：可以直接刷到普通 ESP32 吗？

不建议。当前默认目标是 ESP32-S3，并启用了 S3、16 MB Flash、Octal PSRAM 等配置。普通 ESP32 需要：

- 修改 target；
- 修改板级 GPIO；
- 修改 PSRAM/Flash 配置；
- 重新验证 RMT/LED 驱动；
- 调整分区大小。

### Q3：可以支持其他 32×8 点阵硬件吗？

可以，但需要新增 board header，例如：

```text
components/awtrix_hal/boards/my_board.h
```

然后抽象选择机制。目前 `awtrix_hal.h` 直接 include `boards/ulanzi_s3.h`，后续建议改为 Kconfig 选择板卡。

### Q4：为什么自定义 App 推荐 JSON？

JSON 与原 AWTRIX3 生态兼容，HTTP/MQTT 都易于传输，同时便于 Home Assistant、Node-RED、脚本自动化生成。

### Q5：为什么 32×8 显示中文困难？

32×8 像素高度太低，标准 12×12、16×16 汉字无法完整显示。可选方案：

- 使用 8×8 极简中文字库，但可读性有限；
- 服务端预渲染 bitmap 后滚动；
- 使用拼音、英文缩写、图标替代；
- 支持更高分辨率硬件。

### Q6：Home Assistant 中没有自动出现设备怎么办？

检查：

1. MQTT broker 是否连接；
2. `ha_discovery` 是否启用；
3. `ha_prefix` 是否与 Home Assistant MQTT discovery prefix 一致；
4. 设备是否发布 discovery topic；
5. MQTT 用户是否有 publish 权限；
6. Home Assistant MQTT 集成是否启用 discovery。

### Q7：设备连接不上 Wi-Fi 怎么办？

建议：

1. 通过串口日志查看 Wi-Fi 错误；
2. 检查 NVS 中 SSID/PASS；
3. 使用 `dev.json` 初始化网络配置；
4. 擦除 NVS 后重新配置；
5. 后续实现 Captive Portal 提升首次配网体验。

---

## 20. 版权与许可说明

本项目基于 AWTRIX3 思路和代码迁移而来。原 AWTRIX3 版权信息见 `main/app_main.cpp` 文件头：

```text
Original AWTRIX3 copyright (C) 2024 Stephan Mühl (Blueforcer)
License: Creative Commons Attribution-NonCommercial-ShareAlike 4.0
```

请在分发、修改、商用或二次发布前仔细确认原项目许可证与本仓库实际许可证文件。若计划公开发布，建议补充：

- `LICENSE`；
- 原项目 NOTICE；
- 第三方组件许可证汇总；
- managed components 许可证说明；
- 修改说明和移植声明。

---

## 附录 A：推荐的下一步最小任务清单

如果要把当前工程推进到更可用的公开版本，建议优先做：

1. 增加 `LICENSE` 和第三方许可证说明；
2. 明确 OTA 是否支持，并同步分区表/API/Web UI；
3. 为 `CONFIG` 增加线程安全策略；
4. 将 HTTP/MQTT 控制改为事件队列投递；
5. 实现 Captive Portal；
6. 完善 Web UI；
7. 增加 Host 单元测试；
8. 增加 GitHub Actions / 本地 CI 构建脚本；
9. 增加 API/MQTT 兼容性文档；
10. 真机长时间 soak test：Wi-Fi 重连、MQTT 断线重连、频繁通知、自定义 App 更新、按钮连击、文件上传。

---

## 附录 B：项目定位一句话

**new-awtrix3 是一个面向 Ulanzi TC001 / ESP32-S3 的 AWTRIX3 ESP-IDF 原生移植工程，当前已具备核心显示、HTTP、MQTT、Home
Assistant 和自定义 App 能力，但仍建议优先完善并发安全、Web 配网、媒体解码、OTA 策略和工程化测试。**
