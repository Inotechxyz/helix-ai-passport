<p align="right">
  <strong>简体中文</strong> · <a href="flashing.md">English</a>
</p>

# 向设备烧录固件（Flash Firmware）

本指南说明如何识别串口、选择合并镜像或分段镜像、写入 ESP32-C3，并检查
启动情况。请先完成固件的构建与验证，参见[构建与验证](build-and-test.zh_CN.md)。
烧录只证明镜像已写入，不代表通过真机验收。

## 写入之前

- **不需要备份设备原有固件。** 不要要求先完整读取 Flash 再烧录。烧录会覆盖
  已安装的固件，本流程不会保留自动回滚副本。
- **固件与用户数据是两回事。** 合并镜像可能重置 NVS 和 PHY 数据。如果需要
  保留设置、档案或记录，应先导出，并遵循
  [烧录与已存数据](firmware-layout.zh_CN.md#烧录与已存数据)中的数据策略。
- **把烧录绑定到确切事实。** 写入前确认端口、镜像路径与哈希、写入偏移以及
  预期的数据影响。设备已连接、或此前批准过另一个镜像，都不构成授权。
- **停止占用该端口的其他程序。** 监视器、日志监听和终端程序会占用串口，
  导致访问错误。不要为了释放端口而结束无关进程。

目标设备为 8 MB Flash 的 ESP32-C3，通常使用乐鑫原生 USB Serial/JTAG；
带 USB-UART 桥接芯片的板卡枚举方式不同。

## 识别串口

端口名称可能随每次连接变化。应重新识别端口，不要沿用旧值。

Windows PowerShell：

```powershell
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.Name -match 'COM\d+' } |
  Select-Object Name, DeviceID
```

Linux：

```bash
python -m serial.tools.list_ports -v
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

macOS：

```bash
python -m serial.tools.list_ports -v
ls /dev/cu.usbmodem* /dev/cu.usbserial-* 2>/dev/null
```

根据 USB ID 或端口描述确认设备：

- 乐鑫 USB Serial/JTAG：厂商 ID `303A`，产品 ID `1001`。Linux 下表现为
  `/dev/ttyACM*`；Windows 分配 `COM` 号。
- USB-UART 桥接：例如 `10C4`（CP210x）或 `1A86`（CH34x）。Linux 下表现为
  `/dev/ttyUSB*`。

如果存在多个候选设备或描述不明确，请用户确认目标设备。不要假定第一个
端口就是目标。

## 选择镜像

| 目标 | 产物 | 写入范围 | 数据影响 |
| --- | --- | --- | --- |
| 空白设备或有意完整刷新 | 验证通过的 `build/FoloToy-AI-Passport-full.bin`（合并镜像） | 从 `0x0` 写入 | 可能重置 NVS 与 PHY 数据 |
| 保留设置与记录 | 同一构建产出的 bootloader、分区表和应用程序 | 实际配置的偏移 | 镜像不重叠时不会写入 NVS |

生成并验证合并镜像：

```bash
./tools/validate.sh --firmware
python3 tools/archive_firmware.py verify build/firmware/<full-bin-sha256>/
```

门禁会写出 `build/FoloToy-AI-Passport-full.bin`，并在
`build/firmware/<full-bin-sha256>/` 下归档配套 ELF 与各镜像。交付前应将镜像
哈希与归档中的 `manifest.json` 比对。

增量开发时，`idf.py build` 也会生成包含配置偏移的 `build/flash_args`。
它只是数据，不要当作 shell 脚本执行。不要在 `0x0` 写入应用单体
`build/FoloToy-AI-Passport.bin`，也不要混用不同构建的镜像。

当前 `partitions.csv` 跟踪的布局把 bootloader 写到 `0x0`、分区表写到
`0x8000`、factory 应用写到 `0x30000`。烧录前应结合 `flash_args` 与
`partitions.csv` 核对这些值；自定义布局和上游默认布局（应用位于 `0x10000`）
并不相同。

## 烧录

使用合并镜像从 `0x0` 完整刷新：

```text
esptool --chip esp32c3 -p <port> -b 460800 write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/FoloToy-AI-Passport-full.bin
```

以上命令拼写按 esptool v5。使用 esptool v4 时，改用 `esptool.py` 和下划线
形式的命令，例如 `write_flash` 与 `--flash_mode`。应以已安装工具的帮助为准，
不要假定命令拼写。

在已激活的 ESP-IDF 环境中，可分段烧录并保留 NVS：

```bash
idf.py -p <port> flash
```

`idf.py flash` 只写入 `build/flash_args` 中列出的镜像。等价的显式 esptool
命令为：

```text
esptool --chip esp32c3 -p <port> -b 460800 write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x30000 build/FoloToy-AI-Passport.bin
```

请使用与实际烧录构建一致的路径和偏移。如果 bootloader 和分区表已经正确，
仅更新应用可运行 `idf.py -p <port> app-flash`。

esptool 默认校验写入数据并输出 `Hash of data verified`。如果校验失败，应
停止并检查线缆、供电、端口和镜像，不要盲目重写。成功写入后会硬复位，
设备随后启动新固件。

## 检查启动

已激活 ESP-IDF 时：

```bash
idf.py -p <port> monitor
```

没有 ESP-IDF 时，可用 115200 波特率的普通串口终端，例如：

```text
python -m serial.tools.miniterm <port> 115200
```

打开监视器可能复位板卡，应按重启预期处理。采集有限长度的日志后，在运行
其他工具或交回端口前停止监视器。如果没有任何输出，应用可能处于空闲或
深睡眠状态；复位一次后复查，不要直接重写镜像。

确认出现 bootloader 交接与应用启动消息。随后按适用的
[硬件检查清单](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)
在设备上操作改动过的行为。启动日志正常不能证明显示、音频、按键或射频
行为正常；真机测试结果应与构建结果分开报告。

## 故障排查

| 现象 | 检查项 |
| --- | --- |
| 端口被占用或拒绝访问 | 停止监视器和日志监听；确认没有其他程序占用端口。不要结束无关进程。 |
| 连接失败 | 使用可传输数据的线缆，确认设备已供电，并复查端口。原生 USB Serial/JTAG 通常会自动进入下载模式；手动启动模式只适用于支持该方式的板卡。 |
| 哈希校验失败 | 最多重试一次后停止。检查线缆、供电、端口与镜像身份；未经新的授权不要改变已批准的目标或写入范围。 |
| 无启动日志 | 复位一次，确认 115200 波特率和正确端口。应用可能空闲或处于深睡眠。 |
| 怀疑 Flash 损坏 | 不要用整片擦除作为例行修复。先验证镜像与偏移；任何擦除前先保存用户数据。 |

`idf.py erase-flash` 和整片擦除会删除 NVS、设置与记录。只有明确需要完整
擦除、且必须保留的数据已保存时才可使用。

## 记录烧录结果

记录端口、镜像路径与 SHA-256、写入偏移、esptool 结果、有限长度的启动日志
以及观察到的行为。原始日志保存在本地，分享前清除凭据、令牌和个人数据。
出现崩溃时，使用同一构建对应的 ELF 转入调试流程。

相关文档：[构建与验证](build-and-test.zh_CN.md)、
[固件布局与数据策略](firmware-layout.zh_CN.md#烧录与已存数据)、
[环境引导](environment-setup.zh_CN.md)、
[硬件指南](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)和
[AI 开发指南](../ai-guide.zh_CN.md)。
