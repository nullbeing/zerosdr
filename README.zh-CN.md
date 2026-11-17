# zeroSDR

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%20%7C%20M5Stack-orange.svg)]()

**简体中文** | [English](README.md)

zeroSDR 是一个面向小屏幕嵌入式 Linux 的开源 SDR 接收程序。频谱和瀑布直接画进
framebuffer，音频解调（FM / NFM / AM / USB / LSB）通过 ALSA 输出。

![zeroSDR 界面](images/zerosdr.jpg)

> **在 SDR 页面按 `H` 可以随时显示屏幕按键列表。**
> 在列表页按 `→` 切到设置页（FM 去加重、音频 AGC、FFT 大小、采样率），
> 再按 `H` 关闭。

| | |
|:---:|:---:|
| ![频谱页](images/spectrum.png) | ![ADSB 雷达](images/radar.png) |
| 宽带频谱页：460–490 MHz 扫描 + 峰值保持 | ADSB 雷达视图：距离环与呼号大字 |

| | |
|:---:|:---:|
| ![ADSB 详情](images/detail.png) | ![按键说明](images/sdr.png) |
| 飞机详情页（PFD） | 实时频谱、瀑布与状态栏 |

## 功能

- 320×170 framebuffer 上的实时频谱与瀑布
- FM / NFM / AM / USB / LSB 解调，带静噪、音频 AGC、FM 去加重（50/75 µs）
- 硬件 AGC / 手动增益、显示缩放、FFT 大小、采样率预设
- 屏幕按键列表 + 设置页（`H`）
- ADSB 飞机页面（readsb），含雷达/PFD 视图
- 宽带扫描频谱页：自定义起止频率、平移缩放、峰值保持
- 频谱与瀑布六种配色主题（`M`）
- 任意页面都能用键盘调节系统音量（`Fn`+`D` / `Fn`+`S`）
- 借助 librtlsdr 的 udev 规则，以普通用户运行，**不需要 root**
- 针对 Raspberry Pi Zero 2W / Pi 5 与 M5Stack Cardputer Zero 优化

## 硬件要求

- Raspberry Pi 5 / Pi Zero 2W / M5Stack Cardputer Zero（CM0，aarch64）
- RTL-SDR 电视棒（已验证：RTL-SDR Blog V4，RTL2832U）
- 320×170 framebuffer 屏幕（`/dev/fb0`，Cardputer Zero 上还有 `/dev/fb_lcd`）
- 尽量新的 rtl-sdr 驱动（直接影响调谐准确性）

## 快速开始（桌面 / 树莓派）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake librtlsdr-dev libasound2-dev libpulse-dev libfreetype-dev nlohmann-json3-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

./build/zerosdr                    # 默认：100 MHz、1.024 MS/s、自动增益
./build/zerosdr -f 99300000        # 99.3 MHz FM
./build/zerosdr -s 2048000 -g 20   # 指定采样率和增益
```

在 CM0 上**必须用 Release 构建**：FFT、解调、重采样要在四个小核上实时跑。

## 在 Cardputer Zero 上安装

zeroSDR 已在 Cardputer Zero 应用商店（AppStore）上架：打开商店搜 **zeroSDR**
安装即可。在设备终端等价于：

```bash
sudo apt update && sudo apt install zerosdr
```

包里已经带好程序本体、APPLaunch 入口与图标，以及带 RTL-SDR 后端的
`readsb` 解码器，不需要再装别的东西。

## Cardputer Zero 手动部署

APPLaunch 以桌面用户 `pi` 启动应用，`Exec` / `Icon` 按 `/usr/share/APPLaunch`
为根解析。最终目录：

```text
/usr/share/APPLaunch/
├── applications/zerosdr.desktop
├── bin/zerosdr                 # 主程序
├── bin/run-zerosdr             # wrapper：屏幕 / 键盘 / 音频准备
└── share/images/zerosdr.png    # 图标
```

### 1. 在设备上构建（推荐）

```bash
sudo apt-get install -y build-essential cmake librtlsdr-dev libasound2-dev libpulse-dev libfreetype-dev nlohmann-json3-dev
cd ~/zerosdr
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
file build/zerosdr        # 应显示 ARM aarch64
```

### 2. 安装主程序

```bash
sudo install -d /usr/share/APPLaunch/bin
sudo install -m 755 build/zerosdr /usr/share/APPLaunch/bin/zerosdr
```

### 3. 安装启动 wrapper

APPLaunch 的 `Terminal=false` 模式不经过 shell，所以 `-d /dev/fb_lcd` 这类参数
不能直接写在 `Exec` 里。wrapper 还负责复用用户的 PipeWire 会话，并把 ES8389
DAC 固定在 -6 dB（0 dB 会周期性断音）。

```bash
sudo tee /usr/share/APPLaunch/bin/run-zerosdr >/dev/null <<'EOF'
#!/bin/sh
set -eu

# APPLaunch 在用户会话里启动应用；复用其 PipeWire runtime。
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    for d in /run/user/*; do
        if [ -S "$d/pipewire-0" ]; then
            XDG_RUNTIME_DIR="$d"
            break
        fi
    done
fi
if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    export XDG_RUNTIME_DIR
    export PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR"
fi

# 128 帧 quantum 在 CM0 上会产生周期性 xrun。
if command -v pw-metadata >/dev/null 2>&1; then
    pw-metadata -n settings 0 clock.force-quantum 1024 >/dev/null 2>&1 || true
fi

# ES8389 功放在 0 dB 会断音；179 = -6 dB。
amixer -c 1 cset name='DACL Playback Volume' 179 >/dev/null 2>&1 || true
amixer -c 1 cset name='DACR Playback Volume' 179 >/dev/null 2>&1 || true

# LCD framebuffer。
if [ -n "${LV_LINUX_FBDEV_DEVICE:-}" ]; then
    FB="$LV_LINUX_FBDEV_DEVICE"
elif [ -e /dev/fb_lcd ]; then
    FB=/dev/fb_lcd
else
    FB=/dev/fb0
fi

# TCA8418 键盘。
KBD=/dev/input/by-path/platform-3f804000.i2c-event

set -- -d "$FB" "$@"
if [ -e "$KBD" ]; then
    set -- "$@" -k "$KBD"
fi

exec /usr/share/APPLaunch/bin/zerosdr "$@"
EOF
sudo chmod 755 /usr/share/APPLaunch/bin/run-zerosdr
```

### 4. 安装桌面入口

```bash
sudo install -d /usr/share/APPLaunch/applications
sudo tee /usr/share/APPLaunch/applications/zerosdr.desktop >/dev/null <<'EOF'
[Desktop Entry]
Name=zeroSDR
Comment=Lightweight SDR receiver
Exec=bin/run-zerosdr
Terminal=false
Icon=share/images/zerosdr.png
Type=Application
EOF
```

### 5. 安装图标

```bash
sudo install -d /usr/share/APPLaunch/share/images
sudo install -m 644 share/images/zerosdr.png \
    /usr/share/APPLaunch/share/images/zerosdr.png
```

### 6. 权限（不需要 root）

librtlsdr 自带 `/lib/udev/rules.d/60-librtlsdr0.rules`，会把电视棒设成
`root:plugdev 0660`。确认 `pi` 在 `plugdev`（以及 `video` / `audio` / `input`），
然后重新插拔电视棒：

```bash
groups pi
ls -l /dev/bus/usb/*/*        # RTL-SDR 节点应为 root plugdev
lsusb | grep -i 0bda
```

### 7. 重启 APPLaunch 并验证

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
systemctl --user restart APPLaunch.service
```

从启动器打开 **zeroSDR**，依次确认：

1. 频谱 / 瀑布在动
2. **按 `H` 出现按键列表，按 `→` 进入设置页**
3. 有声音（调到已知电台）
4. `Tab` 打开页面菜单，用方向键选择、`Enter` 进入；`Q` / 长按 `Esc` 回到启动器

日志：

```bash
journalctl --user -u APPLaunch.service -f
```

正常应包含：

```text
Framebuffer initialized: 320x170
Keyboard map: Cardputer Zero (tca8418c)
RTL-SDR device initialized
Audio output initialized: 48000 Hz, 1 channel(s)
```

### 卸载

```bash
sudo rm -f /usr/share/APPLaunch/bin/zerosdr \
           /usr/share/APPLaunch/bin/run-zerosdr \
           /usr/share/APPLaunch/applications/zerosdr.desktop \
           /usr/share/APPLaunch/share/images/zerosdr.png
systemctl --user restart APPLaunch.service
```

### `.deb` 方式

```bash
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../zerosdr_*.deb
```

包内安装 `usr/bin/zerosdr` 和 `/usr/share/APPLaunch/` 下的入口、主程序与图标；
**不会**安装系统级 `/usr/share/applications` 入口（AppStore 的路径规则只允许
`usr/share/<包名>/**` 和 `usr/share/APPLaunch/**`），也不会生成 `-dbgsym` 包。

更详细的 Cardputer Zero 说明（音频、ADSB/readsb、依赖包风险、恢复步骤）见
[cardputerzero.md](cardputerzero.md)。

## 首次启动

第一次运行会显示一个简短引导（之后可从 `H` → 设置 → `SHOW GUIDE` 重新打开）：

- `H` 打开按键列表与设置
- `Tab` 打开页面菜单，选择 SDR、频谱、ADSB
- `Enter` 在 ADSB 的 列表 → 详情 → 雷达 之间循环
- 观测点位置：可以手动输入，也可以在检测到 GPS 时自动读取（gpsd 或串口
  NMEA）。手动输入会一直覆盖 GPS，直到在设置里清除位置。编辑器支持负数
  （南纬/西经）：按 `SYM`+`R` 输入减号，再按一次则去掉负号。

## 按键

### 先按 `H`

SDR 页面内置了参考页：

```text
H          打开 / 关闭浮层
← / →      在按键列表和设置页之间切换
↑ / ↓      选择设置项
Enter      修改选中的设置
```

### Cardputer Zero 键位

TCA8418 键盘会自动识别（`tca8418c`）。`F`/`Z`/`X`/`C` 直接作为方向键，`Fn`
组合键发出标准功能码，普通字母键用于 SDR 功能。

| 按键 | 功能 |
|---|---|
| `Z` / `←` | 频率 -step |
| `C` / `→` | 频率 +step |
| `F` / `↑` | 增益 + |
| `X` / `↓` | 增益 - |
| `Alt`+`Z`/`C` | 粗调（step × 10） |
| `0-9` `.` | 直接输入频率（MHz） |
| `Enter` | 应用频率 |
| `Backspace` | 删除一位 |
| `Esc` | 取消输入 |
| `D` | 切换解调模式（FM/NFM/AM/USB/LSB） |
| `B` / `Fn`+`B` | 切换解调带宽 |
| `Fn`+`D` | 音量 +（所有页面可用） |
| `Fn`+`S` | 音量 -（所有页面可用） |
| `S` | 静噪（+5%；长按 >0.5 s = 10%，>1.5 s = 25%） |
| `T` | 切换调谐步进（Auto/100/1k/5k/10k/100k/1M） |
| `Space` / `Fn`+`A` | 音频开关 |
| `G` | 硬件 AGC 开关 |
| `M` | 频谱与瀑布配色主题（DEFAULT/CLASSIC/TURBO/INFERNO/VIRIDIS/PLASMA） |
| `Y` | 显示缩放（只在采样率允许的档位内循环） |
| `Fn`+`P`（F12） | 截图（`~/zerosdr_screenshot_*.png`） |
| `H` | 按键列表，`→` 进设置页 |
| `Tab` | 页面菜单（方向键选择，`Enter` 进入） |
| `Q` | 立即退出 |
| `Esc`（长按 2 秒） | 退出，屏幕中央有进度提示 |

`Fn`+`D` / `Fn`+`S` 在任何页面都能调节系统音量。launcher 的 labwc 绑定通常
是 5% 一档；如果 300 ms 内系统音量没有变化，zeroSDR 会自己按 10% 调整，
并用 SDR 状态栏或频谱/ADSB 页的 `VOL n%` 提示反馈。SDR 页按音量 + 还会
同时取消静音。

### PC 键盘

| 按键 | 功能 |
|---|---|
| `←` `→` | 调谐（Alt = 粗调） |
| `Alt`+`Z`/`C` | 粗调（step × 10） |
| `↑` `↓` | 增益 |
| `D` | 解调模式 |
| `B` / `Shift+B` | 解调带宽 |
| `Space` | 音频开关 |
| `S` | 静噪 |
| `T` | 调谐步进 |
| `M` | 配色主题 |
| `Shift+Z` | 显示缩放 |
| `Shift+R` | FFT 大小 |
| `Shift+S` | 截图 |
| `H` | 按键列表 / 设置 |
| `Tab` | 页面菜单 |
| `Q` / `Esc` | 退出 |

## 设置页（`H` → `→`）

不常改的选项放在设置页里，不占用快捷键：

| 项目 | 取值 | 默认 |
|---|---|---|
| FM 去加重 | 50 µs（中国/欧洲/亚洲）/ 75 µs（美洲） | 50 µs |
| 音频 AGC | 开 / 关 | 开 |
| FFT 大小 | 256 / 512 / 1024 / 2048 | 512 |
| 采样率 | 1.024 / 1.2 / 1.8 / 2.048 / 2.4 MHz | 1.024 MHz |
| 首次启动引导 | `SHOW GUIDE`：立即重新打开 | — |

修改采样率时，显示跨度会自动切到"新带宽能填满的最大档"（1.2 M → 1.0 M，
2.048 M → 2.0 M），避免出现"跨度大于采样率、按 `Y` 看起来没反应"的情况。

所有设置统一保存在：

```text
~/.config/zerosdr/config.json
```

```json
{
  "system":   { "guide_show": false },
  "sdr":      { "deemphasis_us": 50, "audio_agc": true,
                "fft_size": 512, "sample_rate": 1024000, "theme": 0 },
  "spectrum": { "configured": true,
                "start_hz": 88000000, "stop_hz": 108000000 },
  "adsb":     { "observer_latitude": 23.1211,
                "observer_longitude": 113.236, "manual_override": true }
}
```

旧的 `sdr_settings.json`、`spectrum_range.txt`、`adsb_config.json` 和
`guide.json` 会在首次启动时自动导入一次，之后不再更新。

## 解调带宽预设

**FM（广播）**：200 / 250 / **300** / 400 / 500 kHz
**NFM**：6.25 / **12.5** / 25 kHz
**AM**：5 / **10** / 15 kHz
**SSB（USB/LSB）**：2.4 / 2.7 / 3.0 kHz

## 显示缩放档位

`Y` 循环 50k / 100k / 200k / 500k / 1M / 2M / 2.8M / 3.2M，并且限制在当前
采样率能覆盖的范围内。

## ADSB 页面

按 `Tab` 打开页面菜单，可切换到 SDR 页面、宽带频谱页和 ADSB 飞机列表。列表页按键：

| 按键 | 功能 |
|---|---|
| `↑` / `↓` | 选择飞机 |
| `←` / `→` | 上下翻 5 行 |
| `Enter` | 顺序切换视图：列表 → 详情 → 雷达 → 列表 |
| `R` | 直接跳到雷达视图（需要先设置观测点位置） |
| `I` | 切换雷达的 航向朝上 / 北朝上（需要 IMU 罗盘） |
| `0` | 罗盘归零：先把机身上边对准正北，再按一下 |
| `H` | 按键列表 + 设置（经度/纬度、清除位置） |
| `L` | 观测点位置编辑 |
| `G` / `+` / `-` | 雷达增益模式 / 增益 |
| `Tab` | 页面菜单 |

雷达左侧列表显示 `ICAO` / 高度 / 距离，选中飞机的呼号以大字显示在左侧栏底部的
暗色通栏条带上（没有呼号时显示 ICAO）。呼号以及 ADSB 的状态栏/详情文字使用抗锯齿
TrueType 字体渲染。

### 罗盘（BMI270 + BMM150）

部分 Cardputer Zero 主板带有 BMI270 加速度计和 BMM150 磁力计（磁力计挂在 BMI270
的辅助 I²C 总线上）。zeroSDR 通过内核 IIO 的 sysfs 设备读取两个传感器，不需要额外
驱动、守护进程或权限。检测到之后，雷达可以切换为**航向朝上**：罗盘刻度随设备转动，
让机身上边所指的方向始终在圆盘顶部，飞机光点和扫描线也一起转。

- `I` 在 航向朝上 / 北朝上 之间切换。状态栏显示 `HDG 123`：航向朝上时高亮，北朝上
  时变暗（罗盘仍然在跟踪）。
- `0` 把当前方向记为罗盘零点：先把机身上边对准正北（或对准真北，把当地磁偏角一并
  吃掉），再按一下。偏移会写入配置文件。
- 航向由加速度计做倾斜补偿，并按单位向量平滑，跨过 0°/360° 时不会跳变。
- 没有 IMU 的机型，雷达保持北朝上，按 `I` 只会响一声提示。

设置保存在 `~/.config/zerosdr/config.json`：

```json
"imu": {
  "enabled": true,
  "heading_up": true,
  "offset_deg": 0.0,
  "invert": false
}
```

`offset_deg` 用来补偿芯片在机身里的安装角度（也可以吸收磁偏角）；`invert` 用于
镜像安装时翻转旋转方向。

观测点位置可以来自 GPS（gpsd 的 2947 端口，或 `/dev/serial0`、`/dev/ttyAMA0`、
`/dev/ttyS0`、`/dev/ttyUSB0`、`/dev/ttyACM0` 上的 NMEA 数据）。会记住最后一次
定位；手动输入的位置优先，直到清除为止；ADSB 设置页会显示当前来源是
GPS / MANUAL / SAVED / NOT SET。

ADSB 解码使用随包安装的 `readsb`（带 RTL-SDR 后端，位于
`/usr/lib/zerosdr/readsb`），不需要另外安装解码器；如果系统里有
`/usr/bin/readsb` 则作为后备。

按 `H` 打开全屏浮层，两个标签页：按键列表和设置页。设置页可以查看/修改观测点
经纬度（回车进入原来的位置编辑界面），也可以清除已保存的位置。`←`/`→` 切换
标签页，`↑`/`↓` 选择，`Enter` 编辑，`H` 关闭。编辑器里按 `SYM`+`R` 输入减号
（再按一次去掉负号），南纬/西经和正坐标一样输入。

音效反馈：

- 列表光标移动时短促“嘀”一声
- 雷达每转一圈（60°/s，6 秒）一次声纳 ping
- 扫描线每经过 90° 一个轻微 tick
- 出现新飞机时两音提示（最多每 3 秒一次）

## 宽带频谱页

`Tab` 打开页面菜单（SDR / 频谱 / ADSB）。频谱页是一个 qspectrumanalyzer 风格的
扫描式频谱仪：把 RTL-SDR 在用户指定的起止频率之间反复调谐，把每一片拼成一条宽
频谱，下面配瀑布。RTL-SDR V4 的范围是 24–1766 MHz。

第一次进入时会要求输入 START / STOP（MHz）。用数字和 `.` 输入，`↑`/`↓` 切换字段，
`Enter` 逐项确认；范围保存在 `~/.config/zerosdr/config.json` 的 `spectrum` 段里，
之后直接复用。

| 按键 | 功能 |
|---|---|
| `←` / `→` | 平移可见窗口 |
| `↑` / `↓` 或 `+` / `-` | 放大 / 缩小 |
| `Enter` | 回到完整范围 |
| `M` | 峰值保持开关（红色包络，按 1.5 dB/s 回落） |
| `Space` | 显示平滑开关（约 1 MHz 中值滤波） |
| `R` | 修改起止频率 |
| `H` | 半透明帮助浮层 |
| `Tab` | 页面菜单（方向键选择，`Enter` 进入） |
| `Esc`（长按 2 秒） | 退出 |

说明：

- 每一片用 1.024 MHz 采样率、取中间约 0.78 MHz，调谐稳定 15 ms，一步约 20 ms；
  20 MHz 范围不到一秒扫完，100 MHz 几秒钟。
- 宽频时一个 bin 约 2 kHz，放大后一个 bin 一列像素。
- 宽带频谱页固定使用 tuner 自动增益（AGC）。手动增益会放大 RTL-SDR 的
  28.8 MHz 时钟泄漏，所以这里不提供；需要手动增益请到 SDR 页调。
- `Space` 切换显示平滑：对画出来的轨迹做约 1 MHz 的中值滤波，再和原始轨迹
  按 50/50 混合（半强度）。它能压掉窄的时钟 spur、让宽信号显出来，同时保留
  一部分真实的窄信号。采集的 bin、峰值保持和回落都不受影响，只过滤显示轨迹和
  新写入的瀑布行；开启时状态栏显示 `SM`。
- 峰值保持默认开启，`M` 关闭时会清空已记录的峰值。红色包络按 1.5 dB/s 回落
  （`spectrum.max_hold_decay_db_s`，设为 0 则永久保持），已经消失的峰会自动退掉，
  持续存在的信号仍会每轮刷新自己的电平。
- 平移或缩放时会清空瀑布窗口，避免显示别处残留的历史。
- 离开页面时会释放 SDR，交还给 SDR 页和 ADSB 页。

## 命令行参数

```text
Usage: zerosdr [options]
  -f <freq>     中心频率（Hz，默认 100000000）
  -s <rate>     采样率（Hz，默认 1024000）
  -g <gain>     增益（dB，默认自动）
  -d <device>   framebuffer 设备（默认 /dev/fb0）
  -k <device>   键盘输入设备（默认自动检测）
  -h            显示帮助
```

## 常见问题

### 瀑布看起来是空的，但声音很清晰

FM 广播对轻微偏调不敏感：电台偏离显示中心 100 kHz 时声音依然清楚，只是峰值会
落在红色中心线的左/右一点。想让它居中，就调到峰上（FM AUTO 步进刚好 100 kHz）。

正中那条很窄的亮线是 RTL-SDR 的 **DC / 本振泄漏**，不是电台；真正的广播电台是
约 200 kHz 宽的平台。

### 空频率上声音很响

静噪关闭（`SQ:OFF`）+ 音频 AGC 打开时，解调噪声会被放大到接近满幅。按 `S`
把静噪加上，噪声就会被门限挡掉。

### 找不到 RTL-SDR / 权限不足

```bash
lsusb | grep -i 0bda
ls -l /dev/bus/usb/*/*      # 应为 root:plugdev 0660
groups pi                   # 应含 plugdev、video、audio、input
```

安装/升级 `librtlsdr0` 后重新插拔电视棒。

### 设备忙

```bash
sudo pkill readsb
sudo pkill -f rtl_
```

### 没有声音

```bash
aplay -L | head             # "default" 应指向 PipeWire
amixer -c 1 sget Speaker
```

精简系统里可能没有 ALSA 的 `default` 路由（日志会显示
`Cannot open audio device default`）。程序会自动回退到 `pulse` / `pipewire` /
声卡设备；如果都没有，安装 `libasound2-plugins`：

```bash
sudo apt-get install -y libasound2-plugins
```

wrapper 会把 `DACL/DACR Playback Volume` 设为 179（-6 dB）；0 dB 时 ES8389
功放会断音。

### 频偏

```bash
rtl_test -p                 # 测量 PPM
```

几 ppm 属正常（例如 -27 ppm ≈ 100 MHz 上 2.7 kHz）。偏得很多通常是 rtl-sdr
驱动太旧。

### 黑屏 / 画到别的屏幕

```bash
cat /proc/fb
ls -l /dev/fb*
```

手动运行排除 APPLaunch 因素：

```bash
/usr/share/APPLaunch/bin/zerosdr -d /dev/fb_lcd \
    -k /dev/input/by-path/platform-3f804000.i2c-event
```

## 交叉编译

```bash
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libfreetype-dev

cmake -S . -B build-aarch64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
cmake --build build-aarch64 -j"$(nproc)"

scp build-aarch64/zerosdr pi@<cardputer-ip>:/tmp/
```

## 许可证

本项目采用 GNU General Public License v3.0，详见 [LICENSE](LICENSE)。

## 致谢

- 基于 [rtl-sdr](https://github.com/osmocom/rtl-sdr)
- 面向 M5Stack Cardputer Zero 设计
