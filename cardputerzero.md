# zeroSDR 在 Cardputer Zero 真机部署指南

本文说明如何把 zeroSDR 完整部署到 M5Stack Cardputer Zero 的 APPLaunch 中，包括：

- AArch64 构建
- APPLaunch 目录、启动脚本、桌面入口和图标
- 显示与 TCA8418 键盘适配
- PipeWire / ES8389 音频设置
- RTL-SDR 权限与 USB 稳定性
- ADSB 所需的 readsb 安装
- 实机验证和常见故障排查

> 测试环境：Cardputer Zero（CM0，AArch64，Debian 13，320×170 屏幕，ES8389，TCA8418 键盘）
>
> 项目：zeroSDR v0.3.2

---

## 1. 部署后的目录结构

最终 APPLaunch 中应包含：

```text
/usr/share/APPLaunch/
├── applications/
│   └── zerosdr.desktop
├── bin/
│   ├── zerosdr              # 实际可执行文件
│   └── run-zerosdr          # 启动 wrapper
└── share/
    └── images/
        └── zerosdr.png      # 图标
```

APPLaunch 扫描的是：

```text
/usr/share/APPLaunch/applications/*.desktop
```

`Exec` 如果是相对路径，会以 `/usr/share/APPLaunch` 为根解析。

---

## 2. 设备准备

### 2.1 SSH 登录

```bash
ssh pi@<cardputer-ip>
```

以下命令默认在 Cardputer Zero 上执行。如果在 Pi 5 上交叉编译，会在对应步骤注明。

### 2.2 安装运行依赖

```bash
sudo apt-get update
sudo apt-get install -y \
    librtlsdr0 \
    libasound2t64 \
    alsa-utils \
    pipewire-alsa
```

说明：

- Debian 13 的 ALSA 运行时包是 `libasound2t64`；旧系统可能是 `libasound2`。
- `alsa-utils` 提供 `amixer`，启动脚本会用它设置 DAC 音量。
- `pipewire-alsa` 让 ALSA 的 `default` 设备走 PipeWire，zeroSDR 才能从扬声器出声。

> **已知系统坑：`pipewire-alsa` 可能升级 libcamera**
>
> 在某一版 Cardputer Zero 系统上，直接执行 `apt-get install pipewire-alsa` 会顺带把
> `libcamera` / `pipewire` 升级到新版本，导致预装的 Camera App 出现：
>
> ```text
> undefined symbol: _ZN9libcamera10properties14PixelArraySizeE
> ```
>
> 这不是 zeroSDR 的问题，但如果还要使用 Camera App，请阅读本文最后的
> “Camera App 被 libcamera 升级破坏” 一节。

### 2.3 用户权限

APPLaunch 通常以 `pi` 用户运行外部程序。确认 `pi` 在以下组：

```bash
groups pi
```

至少需要：

```text
video    # /dev/fb0、/dev/video*
audio    # ALSA
input    # /dev/input/event*
```

如果缺少：

```bash
sudo usermod -aG video,audio,input pi
```

重新登录后生效。

### 2.4 构建依赖

在真机上直接构建：

```bash
sudo apt-get install -y \
    build-essential \
    cmake \
    librtlsdr-dev \
    libasound2-dev
```

如果在 Pi 5 上交叉编译，则安装交叉工具链：

```bash
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

---

## 3. 构建 zeroSDR

### 3.1 在 Cardputer Zero 上原生构建

```bash
cd ~/zerosdr
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
```

当前 `CMakeLists.txt` 在未指定构建类型时默认也是 `Release`，但显式指定更稳妥。

检查二进制：

```bash
file build/zerosdr
```

应看到：

```text
ELF 64-bit LSB pie executable, ARM aarch64
```

### 3.2 在 Pi 5 上构建后复制

如果 Pi 5 本身就是 AArch64，可以直接构建：

```bash
cd ~/zerosdr
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

然后复制到设备：

```bash
scp build/zerosdr pi@<cardputer-ip>:/tmp/zerosdr
```

### 3.3 必须使用 Release

zeroSDR 在 CM0 上要同时跑 FFT、解调、重采样和 UI。未优化的构建会导致音频线程跟不上，出现持续 underrun 和卡顿。

确认：

```bash
grep CMAKE_BUILD_TYPE build/CMakeCache.txt
```

应为：

```text
CMAKE_BUILD_TYPE:STRING=Release
```

---

## 4. 安装到 APPLaunch

以下操作在 Cardputer Zero 上执行。

### 4.1 创建目录

```bash
sudo install -d \
    /usr/share/APPLaunch/bin \
    /usr/share/APPLaunch/applications \
    /usr/share/APPLaunch/share/images
```

### 4.2 安装主程序

```bash
sudo install -m 755 build/zerosdr \
    /usr/share/APPLaunch/bin/zerosdr
```

### 4.3 安装图标

```bash
sudo install -m 644 share/images/zerosdr.png \
    /usr/share/APPLaunch/share/images/zerosdr.png
```

### 4.4 创建启动 wrapper

APPLaunch 的 `Terminal=false` 模式不会拆分参数，也不会经过 shell，所以不能直接在 `Exec` 里写 `-d /dev/fb_lcd` 之类的参数。必须使用 wrapper。

创建 `/usr/share/APPLaunch/bin/run-zerosdr`：

```bash
sudo tee /usr/share/APPLaunch/bin/run-zerosdr >/dev/null <<'EOF'
#!/bin/sh
set -eu

# APPLaunch 以用户会话启动应用；复用该用户的 PipeWire 会话。
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

# 防止 PipeWire 使用 128 帧的低延迟 quantum；该模式在 CM0 上会产生
# 周期性 xrun，听起来像短暂断音。
if command -v pw-metadata >/dev/null 2>&1; then
    pw-metadata -n settings 0 clock.force-quantum 1024 \
        >/dev/null 2>&1 || true
fi

# ES8389 扬声器功放在 0 dB DAC 音量下会触发保护性断音。
# -6 dB 是这台硬件上稳定工作的最大音量。
# 179 = -6.0 dB（0.5 dB/step，min -95.5 dB）。
amixer -c 1 cset name='DACL Playback Volume' 179 \
    >/dev/null 2>&1 || true
amixer -c 1 cset name='DACR Playback Volume' 179 \
    >/dev/null 2>&1 || true

# Cardputer Zero 的 LCD。
if [ -n "${LV_LINUX_FBDEV_DEVICE:-}" ]; then
    FB="$LV_LINUX_FBDEV_DEVICE"
elif [ -e /dev/fb_lcd ]; then
    FB=/dev/fb_lcd
else
    FB="$(awk '/fb_st7789v/ {print "/dev/fb" $1; exit}' \
        /proc/fb 2>/dev/null || true)"
    [ -n "${FB:-}" ] || FB=/dev/fb0
fi

# TCA8418 键盘。
KBD=/dev/input/by-path/platform-3f804000.i2c-event

# 保留调用者传入的参数（例如 -f 103600000）。
set -- -d "$FB" "$@"
if [ -e "$KBD" ]; then
    set -- "$@" -k "$KBD"
fi

exec /usr/share/APPLaunch/bin/zerosdr "$@"
EOF

sudo chmod 755 /usr/share/APPLaunch/bin/run-zerosdr
```

关键点：

- 屏幕优先用 `/dev/fb_lcd`，不要写死 `/dev/fb0`。
- 键盘设备是 `platform-3f804000.i2c-event`。
- `set -- -d "$FB" "$@"` 保留外部参数，方便测试 `-f`、`-g`。
- `pw-metadata` 和 `amixer` 是可选调用；失败不会阻止启动。

### 4.5 创建 APPLaunch 桌面入口

```bash
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

字段说明：

- `Exec=bin/run-zerosdr`：相对 `/usr/share/APPLaunch` 解析。
- `Terminal=false`：GUI 程序直接启动。
- `Icon=share/images/zerosdr.png`：相对 APPLaunch 图标目录。

### 4.6 重启 APPLaunch

Cardputer Zero 上 APPLaunch 通常是 **用户服务**：

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
systemctl --user restart APPLaunch.service
```

如果系统版本把 APPLaunch 做成系统服务：

```bash
sudo systemctl restart APPLaunch.service
```

然后确认：

```bash
systemctl --user is-active APPLaunch.service
```

---

## 5. 音频设置

### 5.1 PipeWire / ALSA

`pipewire-alsa` 安装后，ALSA 的 `default` 设备应指向 PipeWire：

```bash
aplay -L | head
```

应该看到类似：

```text
default
    Default ALSA Output (currently PipeWire Media Server)
```

测试：

```bash
speaker-test -D default -t sine -f 440 -c 1 -l 1
```

### 5.2 DAC 音量限制为 -6 dB

Cardputer Zero 的 ES8389 + 1 W 扬声器在接近满幅时会触发保护性断音。测试结果是：

| DAC 音量 | 现象 |
|---|---|
| 0 dB | 满幅音频会间歇断一下 |
| -3 dB | 仍可能临界断续 |
| -6 dB | 稳定 |

设置：

```bash
amixer -c 1 cset name='DACL Playback Volume' 179
amixer -c 1 cset name='DACR Playback Volume' 179
sudo alsactl store
```

wrapper 已包含这两条命令，所以每次从 APPLaunch 启动 zeroSDR 都会重新设置。

### 5.3 PipeWire quantum

在 CM0 上，PipeWire 掉到 128 帧（约 2.7 ms）时会产生周期性 xrun。建议固定到 1024 帧：

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cat > ~/.config/pipewire/pipewire.conf.d/99-cardputer-latency.conf <<'EOF'
context.properties = {
    default.clock.quantum = 1024
    default.clock.min-quantum = 1024
    default.clock.max-quantum = 2048
}
EOF
```

立即生效也可以：

```bash
pw-metadata -n settings 0 clock.force-quantum 1024
```

检查：

```bash
pw-top
```

播放时 sink 的 `QUANT` 应为 1024。

### 5.4 可选：CPU governor

如果仍有偶发卡顿，可把 CPU governor 设为 `performance`：

```bash
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$f"
done
```

代价是耗电和发热增加。重启会恢复默认 governor。

---

## 6. 显示和键盘

### 6.1 显示

Cardputer Zero 的 LCD 通常是：

```text
/dev/fb_lcd -> /dev/fb0
```

确认：

```bash
ls -l /dev/fb_lcd
cat /proc/fb
```

zeroSDR 支持 `-d <device>`，wrapper 已自动传入。

### 6.2 键盘

Cardputer Zero 使用 TCA8418：

```text
/dev/input/by-path/platform-3f804000.i2c-event
```

zeroSDR 会自动识别设备名中的 `tca8418`，并使用 Cardputer Zero 专用键位映射。

**按 `H` 可以随时在屏幕上打开按键列表**；在列表页按 `→` 切到设置页
（FM 去加重、音频 AGC、FFT 大小、采样率），`H` 关闭。

| 按键 | 功能 |
|---|---|
| `Z` / `←` | 频率 -step |
| `C` / `→` | 频率 +step |
| `F` / `↑` | 增益 + |
| `X` / `↓` | 增益 - |
| `Shift` + `Z`/`C` | 粗调（step × 10） |
| `Fn` + `L`（PgUp） | 频率 +1 MHz |
| `Fn` + `M`（PgDn） | 频率 -1 MHz |
| `D` | 切换解调模式（FM/NFM/AM/USB/LSB） |
| `B` / `Fn` + `B` | 切换解调带宽 |
| `Fn` + `D` / `Fn` + `S` | 音量加 / 减 |
| `S` | 静噪（短按 +5%，长按 10%/25%） |
| `T` | 切换调谐步进 |
| `Space` / `Fn` + `A` | 音频开关 |
| `G` | 硬件 AGC 开关 |
| `M` | 显示模式 |
| `Y` | 显示缩放 |
| `Fn` + `J`（PrintScreen） | 截图（`/tmp/zerosdr_*.png`） |
| `H` | 按键列表（`→` 切设置页） |
| `Tab` | SDR / ADSB 页面切换 |
| `Q` / `Esc` | 退出 |

> 直接输入频率时的小数点就是键盘上的 `.` 键（内部由 TCA8418 的符号层键码
> `KEY_HIRAGANA` 映射）——如果这个键失效，检查 `input_handler.cpp` 里的
> `KEY_HIRAGANA → APPKEY_DOT` 映射是否还在。

---

## 7. RTL-SDR 设置

### 7.1 检查设备

```bash
lsusb | grep -i 0bda
```

应看到类似：

```text
0bda:2832 Realtek Semiconductor Corp. RTL2832U
```

测试：

```bash
rtl_test -t
```

如果 `rtl_test` 不存在：

```bash
sudo apt-get install -y rtl-sdr
```

### 7.2 内核驱动

librtlsdr 会在打开设备时自动 detach `dvb_usb_rtl28xxu`。如果出现设备忙：

```bash
sudo pkill readsb
sudo pkill -f rtl_
```

不建议让 readsb 的 systemd 服务常驻；zeroSDR 会自己管理 readsb。

### 7.3 USB 稳定性

如果频谱正常但音频偶发断一下，先检查：

```bash
journalctl --user -u APPLaunch.service -n 100 --no-pager
```

重点看：

```text
SDR ring dropped ... bytes
Audio buffer underrun
```

当前代码已经把 RTL-SDR USB 缓冲提高到 32 × 32 KB，ring buffer 为 2 MB。

---

## 8. ADSB 部署

zeroSDR 的 ADSB 页面本身只负责解析 `aircraft.json`，真正解码 1090 MHz 的是外部程序 **readsb**。

### 8.1 不要直接使用 Debian 的 readsb

Debian 13 的 `readsb` 包没有 RTL-SDR 后端，会报：

```text
SDR type 'rtlsdr' not recognized
```

必须使用带 `RTLSDR=yes` 编译的版本。

### 8.2 编译 readsb

```bash
sudo apt-get install -y \
    build-essential git \
    libusb-1.0-0-dev librtlsdr-dev \
    libncurses-dev zlib1g-dev libzstd-dev pkg-config

git clone --depth 1 https://github.com/wiedehopf/readsb.git ~/readsb-src
cd ~/readsb-src
make -j"$(nproc)" RTLSDR=yes
sudo install -m 755 readsb /usr/bin/readsb
```

验证：

```bash
/usr/bin/readsb --version
/usr/bin/readsb --device-type rtlsdr \
    --gain auto --write-json /tmp/readsb-test --quiet
```

应能看到：

```text
Found Rafael Micro R820T tuner
```

并在 `/tmp/readsb-test/aircraft.json` 中出现飞机数据。

### 8.3 禁用系统 readsb 服务

如果系统安装了 `readsb` 包：

```bash
sudo systemctl disable --now readsb.service
```

否则它会抢占 RTL-SDR。

防止 apt 覆盖自定义二进制：

```bash
sudo apt-mark hold readsb
```

### 8.4 zeroSDR 中的配置

当前源码使用：

```text
/usr/bin/readsb
/tmp/readsb/
```

> 如果你使用的是旧版源码，需要先把 `src/pages/adsb_page.cpp` 里的
> `/home/edwin/readsb/readsb` 改成 `/usr/bin/readsb`，并把配置路径改成
> `/home/pi/.config/zerosdr/adsb_config.json`。

以及默认：

```text
--device-type rtlsdr
--gain auto
--write-json /tmp/readsb/
--quiet
```

观察者位置配置保存在：

```text
/home/pi/.config/zerosdr/adsb_config.json
```

启动 ADSB 页面后：

- 按 `Tab` 切到 ADSB
- 按 `L` 设置经纬度（距离/雷达视图需要）
- `G` 切换增益模式，`+/-` 调整
- 按 `Tab` 切回 SDR；zeroSDR 会杀掉 readsb 并重新初始化 RTL-SDR

如果 ADSB 页面没有飞机：

```bash
cat /tmp/readsb/readsb.log
ls -l /tmp/readsb/aircraft.json
```

---

## 9. 启动与验证

1. 从 APPLaunch 打开 **zeroSDR**
2. 确认屏幕显示频谱/瀑布
3. 确认 RTL-SDR 被识别
4. 确认音频有输出
5. 按 `H` 确认按键列表/设置页可以打开
6. 按 `Tab` 测试 ADSB 页面
7. 按 `Q` 或 `Esc` 退出，APPLaunch 应返回桌面

日志：

```bash
journalctl --user -u APPLaunch.service -f
```

正常启动应包含：

```text
Framebuffer initialized: 320x170
Keyboard map: Cardputer Zero (tca8418c)
RTL-SDR device initialized
Audio output initialized: 48000 Hz, 1 channel(s)
PageManager: Starting main loop with 2 page(s)
```

---

## 10. 常见故障

### 10.1 黑屏或画到 HDMI

确认 `/proc/fb` 和 `/dev/fb_lcd`，不要假设 `/dev/fb0` 一定是 LCD。

```bash
cat /proc/fb
ls -l /dev/fb*
```

手动测试：

```bash
/usr/share/APPLaunch/bin/zerosdr -d /dev/fb_lcd
```

### 10.2 键盘无反应

确认：

```bash
ls -l /dev/input/by-path/platform-3f804000.i2c-event
```

手动指定：

```bash
/usr/share/APPLaunch/bin/zerosdr \
    -d /dev/fb_lcd \
    -k /dev/input/by-path/platform-3f804000.i2c-event
```

### 10.3 没有音频

确认：

```bash
aplay -L | head
wpctl status
amixer -c 1 sget Speaker
```

确认 `DACL/DACR Playback Volume` 为 179。

### 10.4 音频偶发“突然停一下”

按顺序检查：

1. `journalctl` 是否有 `Audio buffer underrun`
2. 是否有 `SDR ring dropped`
3. `pw-top` 的 sink `QUANT` 是否变成 128
4. DAC 是否被其他 App 改回 0 dB
5. 是否使用了 0 dB 或 -3 dB 的 DAC 音量

### 10.5 频谱中心指针与信号不对齐

确认：

```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

并检查当前 `span` 是否大于采样率。当前版本会把显示 span 限制在 `min(span, sample_rate)`。

如果窄带下仍有固定偏移，是 RTL-SDR 的 PPM 频偏，需要 `rtlsdr_set_freq_correction()` 校准。

### 10.6 ADSB 无法启动

```bash
ls -l /usr/bin/readsb
/usr/bin/readsb --device-type rtlsdr --gain auto \
    --write-json /tmp/readsb-test --quiet
```

常见原因：

- 使用了 Debian 不带 RTL-SDR 的 readsb
- `readsb.service` 正在运行并占用设备
- zeroSDR 还没有释放 RTL-SDR
- 天线不是 1090 MHz

### 10.7 Camera App 被 libcamera 升级破坏

如果安装 `pipewire-alsa` 后 Camera App 报：

```text
undefined symbol: _ZN9libcamera10properties14PixelArraySizeE
```

说明 apt 把 libcamera/pipewire 升级到了与预装 Camera App 不匹配的版本。

恢复思路：

1. 把 `libcamera0.7` 等恢复到原来的 `0.7.2+rpt20260817-1`
2. 把 `pipewire` 系列恢复到 `1.4.2-1+rpt3`
3. 安装同版本的 `pipewire-alsa`
4. 使用 `apt-mark hold` 锁定这些包

旧包仍可从 Raspberry Pi archive 获取：

```text
http://archive.raspberrypi.com/debian/pool/main/libc/libcamera/
http://archive.raspberrypi.com/debian/pool/main/p/pipewire/
```

锁定示例：

```bash
sudo apt-mark hold \
    libcamera0.7 libcamera-ipa libcamera-tools \
    libcamera-v4l2 python3-libcamera \
    pipewire pipewire-bin pipewire-pulse pipewire-alsa \
    libpipewire-0.3-0t64 libpipewire-0.3-modules \
    libspa-0.2-modules libspa-0.2-bluetooth \
    libspa-0.2-libcamera pipewire-libcamera
```

---

## 11. 卸载

删除 APPLaunch 文件：

```bash
sudo rm -f \
    /usr/share/APPLaunch/bin/zerosdr \
    /usr/share/APPLaunch/bin/run-zerosdr \
    /usr/share/APPLaunch/applications/zerosdr.desktop \
    /usr/share/APPLaunch/share/images/zerosdr.png
```

重启 APPLaunch：

```bash
systemctl --user restart APPLaunch.service
```

如果需要删除 ADSB：

```bash
sudo apt-mark unhold readsb
sudo apt-get remove readsb
sudo rm -f /usr/bin/readsb
```

---

## 12. 快速部署命令汇总

在可信赖的构建机上：

```bash
cd ~/zerosdr
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

复制到设备：

```bash
scp build/zerosdr share/images/zerosdr.png \
    pi@<cardputer-ip>:/tmp/
```

在 Cardputer Zero 上：

```bash
sudo install -m 755 /tmp/zerosdr \
    /usr/share/APPLaunch/bin/zerosdr
sudo install -m 644 /tmp/zerosdr.png \
    /usr/share/APPLaunch/share/images/zerosdr.png
# 再按第 4.4、4.5 节创建 wrapper 和 desktop
systemctl --user restart APPLaunch.service
```

---

## 13. 版本与已知限制

- 屏幕：320×170，`/dev/fb_lcd`
- 键盘：TCA8418，自动识别 `tca8418c`
- 音频：48 kHz 单声道，PipeWire；DAC 固定 -6 dB
- RTL-SDR：1.024 MHz 默认采样率最稳；2.4 MHz 会显著增加 CPU
- ADSB：需要外置兼容 RTL-SDR 的 readsb
- Camera App：与本项目无直接关系，但 libcamera 包升级会影响它

如果后续升级系统包，建议先确认：

```bash
dpkg -l | grep -E 'libcamera|pipewire|readsb'
```

避免再次出现 libcamera/相机、readsb/RTL-SDR 或 PipeWire/音频的版本不匹配。
