<p align="right">
  <a href="langstudy-todo.md">English</a> · <strong>简体中文</strong>
</p>

# 待办 — langstudy 伙伴分支

`feature/langstudy-buddy` 分支的工作笔记。分支上有两个本地提交（`a1f019e`、
`87f5141`）；当前工作区改动尚未提交，分支也尚未推送。

状态：`[ ]` 待办 · `[~]` 进行中 · `[x]` 完成 · `[!]` 阻塞

---

## P0 — Edge TTS 偶尔只返回一个短词（"prosperity is not completely played"）

**现象。** 在真机上，有些单词只播放前约 0.6 秒；同一个单词稍后再按一次通常能完整播放。已报告于
`prosperity`、`conservation`、`deforestation`、`inflation`、`budget`、`capital`、`vaccine`。

**目前证据（串口日志 `build/wsl-flash/audio-debug7.log`）。**

| 尝试 | 峰值下载量 | 解码样本数 | `完整` |
| --- | --- | --- | --- |
| prosperity #1 | 3600 B | 14400 samples | 1 |
| prosperity #2 | 3600 B | 14400 samples | 1 |
| prosperity #3 | 8064 B | 32256 samples | 1 |

即使响应偏短，服务器也发送 `turn.end`（`完整=1`），因此不是设备主动取消，也不是 TLS 丢记录。

**实测健康响应**（主机探测，线上字节完全一致）：

```
payloads [850 ×11, 274, 105]
headers  [128 ×12, 103]
audio    [720 ×11, 144, 0] = 8064 B
```

所以每帧音频都是 `720 B` 的 MP3（`2 B 长度 + 128 B Edge 头 + 720 B`）。
设备下载的 3600 字节正好等于 **5 × 720**，即记录的六帧中有一帧没有贡献音频——这个账目对不上"句子更短"，
而是对得上"丢了一帧"。

**已排除。**
- WebSocket 分片：每帧日志都显示 `fin=1 首帧=1`，没有 continuation。
- 请求形状：主机客户端发送与设备完全相同的 URL 参数、请求头、config JSON
  （不带末尾 CRLF）和 SSML，12 次全部返回 8064 B。
- 后端 IP：`150.171.27.10` 和 `150.171.28.10` 各自 10 次全部返回 8064 B。
- 服务器偶发波动：主机端 `edge-tts` 7.3.1 对每个单词返回固定大小
  （prosperity 8064、conservation 8352、inflation 7776），各自 8/8 一致。
- 堆内存：store-and-forward 加提前预留 16 KiB 暂存区之后，所有分配失败都已消失
  （`MP3 解码缓冲 分配失败` 不再出现）。

**尚未测试。** 第 6 帧到底是不是设备丢弃的非音频二进制帧（例如
`header_len > EDGE_HEADER_MAX (256)` 把 `Path:audio` 挤出扫描前缀），
还是服务器在慢客户端上真的提前结束了回合。

**当前状态。** 已烧录诊断版固件（audio-debug8），它会为每个二进制帧记录：
`头=<header_len> 读到=<want> 音频=<is_audio> 负载=<remaining>`，
另有 `追加=<bytes> 累计=<total>`、每个文本帧（`文本帧: ...`）、解析出的
`服务端 IP`，以及我们发送的 `时间戳`/`SSML` 长度。抓取文件：
`build/wsl-flash/audio-debug8.log`。**尚未在真机测试**——点按流程还没有跑过，日志是空的。

**恢复方法。** 启动 `build/wsl-flash/watch.py > build/wsl-flash/audio-debug8.log`，
对 prosperity / conservation / deforestation / inflation 各按约 4 次 UP，然后读日志：
- 如果服务器发了 6 帧，最后一帧 `音频=0` 且 `头=` 很长，就提高 `EDGE_HEADER_MAX`
  （或扫描头部尾部）——这是解析器 bug，属于真正的修复。
- 如果服务器发了 6 个音频帧和 `turn.end`，那就是服务端问题。加客户端保护：
  一个 6 个字母以上的英文单词不可能是 0.6 秒，所以当 `phrase.len` 短得不合理
  （例如 `< 4800 B`）时重试一次，只在这之后才回退到本地合成器。
- 考虑对连续单词复用同一个 WebSocket：可以去掉 1.5–2.8 秒的握手+升级时间，
  这正是让学习者再次按 UP 的原因，也能告诉我们截断是否与新建连接相关。

**收尾。** 提交前删除临时的 `音频帧`、`头=`、`追加=`、`文本帧`、`服务端 IP`
和 `时间戳` 诊断；保留堆内存和 `语音数据/完整` 汇总行。

---

## P1 — 其他尚未在真机验证的工作

- [ ] **导航不得重播上一个单词。** `ls_voice_stop()` 现在会在 `langstudy_ui.c`
      中每个导航/菜单/配网入口设置 `s_cancel`、递增 `s_request_seq`、清空队列并停止音频。
      尚未在硬件上确认（与 P0 相同的点按测试）。
- [ ] **重复按键。** `langstudy_voice.c` 中已有重复点按吸收和失败后重播一次；
      在 P0 修复之前表现一般。
- [ ] **Azure 同步重试。** `langstudy_sync.c` 现在先打开/握手再分配内存，
      并把 GET 流式写入可增长缓冲区。运行 `PROGRESS → SYNC NOW`，确认
      `profiles/<uuid>/progress.lsprog` 出现在 blob 容器中。
- [ ] **伙伴流程。** CREATE PROFILE、精灵图、XP/进化、喂食/状态、跨重启持久化
      ——都尚未在真机验证。
- [~] **休眠/唤醒状态栏。** Wi-Fi/`D` 图标不再持久化：每次开机和唤醒都会启动静默的
      `LS_ONLINE_PROBE` 重新检查两条链路，因此图标会在界面出现几秒后点亮，在此之前保持隐藏。
      只有电池 SoC 跨深度休眠保留（RTC 缓存）。仍需在真机检查：重启 → 图标、
      按键唤醒 → 图标，以及探测不干扰开机后第一个单词的 Edge TTS 路径。
- [ ] **启动画面。** 冷启动/重启显示 5 秒启动画面（按键唤醒不显示），
      画面由 `tools/gen_splash.py` 生成；检查题献文字、交接时无黑帧，
      以及启动画面期间的按键无效。

---

## 给接手者的说明

- 分段烧录，绝不做全片擦除（NVS 保存 Wi-Fi 和档案数据）：
  ```
  esptool --chip esp32c3 -p COM11 -b 460800 write_flash --flash_mode dio \
    --flash_size 8MB --flash_freq 80m \
    0x0 build/wsl-flash/bootloader.bin \
    0x8000 build/wsl-flash/partition-table.bin \
    0x30000 build/wsl-flash/FoloToy-AI-Passport.bin
  ```
- `build/wsl-flash/watch.py` 会占用 COM11，烧录前先停止。
- WSL 构建：挂载 `/mnt/d`，rsync 到 `/home/jason/helix-build`（排除
  `managed_components`，或删除后让组件管理器重新下载），
  去掉 `tools/*.sh` 的 CR，然后运行
  `su - jason -c /home/jason/run_build.sh` → `FIRMWARE_COMPILE_PASS`。
- 主机测试：`ACTIONLINT_BIN=/tmp/actionlint-stub tools/validate.sh --static`
  （每个 WSL 会话重新创建 stub；actionlint 下载返回 504）。
