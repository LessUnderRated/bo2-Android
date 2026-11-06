<div align="center">

<picture>
  <source media="(prefers-color-scheme: light)" srcset="docs/assets/hero-public-light.svg">
  <img src="docs/assets/hero-public.svg" alt="bo2-mac：将《使命召唤：黑色行动 II》从 32 位 x86 Windows 静态重编译为原生 ARM64 macOS，使用你自己的 Steam 副本。在 Apple M1 上：战役每秒 51 至 60 帧，僵尸模式 57 至 60 帧，对战机器人的多人模式 53 至 60 帧。" width="100%">
</picture>

# bo2-mac

**《使命召唤：黑色行动 II》，从 32 位 x86 Windows 静态重编译为原生 ARM64 macOS。**<br>
战役、僵尸模式和对战机器人的多人模式，在你的 Mac 上用你自己的 Steam 副本构建。

<a href="README.md">English</a> · <b>中文</b>

<a href="#安装"><img src="docs/assets/btn-play.svg" alt="开始游戏" height="40"></a>&nbsp;
<a href="#结果"><img src="docs/assets/btn-results.svg" alt="结果" height="40"></a>&nbsp;
<a href="#流程"><img src="docs/assets/btn-pipeline.svg" alt="流程" height="40"></a>&nbsp;
<a href="#发现"><img src="docs/assets/btn-findings.svg" alt="发现" height="40"></a>&nbsp;
<a href="#路线图"><img src="docs/assets/btn-roadmap.svg" alt="路线图" height="40"></a>

![source: x86-32 Windows](https://img.shields.io/badge/source-x86--32%20Windows-ff6a00?style=flat-square&labelColor=0b0b0c)
![target: ARM64 macOS](https://img.shields.io/badge/target-ARM64%20macOS-ff6a00?style=flat-square&labelColor=0b0b0c)
![lifter: Remill, LLVM 17](https://img.shields.io/badge/lifter-Remill%20%E2%86%92%20LLVM%2017-ff6a00?style=flat-square&labelColor=0b0b0c)
![renderer: D3D11 to Metal](https://img.shields.io/badge/renderer-D3D11%20%E2%86%92%20Metal-ff6a00?style=flat-square&labelColor=0b0b0c)
![Wine: none](https://img.shields.io/badge/Wine-none-ff6a00?style=flat-square&labelColor=0b0b0c)
![game files in this repository: none](https://img.shields.io/badge/game%20files-none-f4f1ea?style=flat-square&labelColor=0b0b0c)
![status: beta](https://img.shields.io/badge/status-beta-f4f1ea?style=flat-square&labelColor=0b0b0c)

<sub>[概要](#概要) · [方法](#方法) · [限制](#限制) · [仓库结构](#仓库结构) · [法律声明](#法律声明)</sub><br>
<sub>2026-11-03 测试版 · 在一台 Apple M1 上测得（4 个性能核心 + 4 个能效核心，16 GB）</sub>

</div>

<p align="center"><a href="https://github.com/adrbn/bo2-mac/raw/main/docs/assets/gameplay.mp4"><img src="docs/assets/preview.gif" alt="战役、僵尸模式和多人模式在 macOS 上原生运行" width="560"></a><br>
<sub>Apple M1 上实际游玩时录制的战役（Angola）、僵尸模式（Town）和多人模式（Hijacked，对战机器人）；右上角的数字是每秒帧数；录屏本身会占用一些帧数，战役中最明显。<a href="https://github.com/adrbn/bo2-mac/raw/main/docs/assets/gameplay.mp4">80 秒视频（MP4，20 MB）</a></sub></p>

> [!IMPORTANT]
> **本仓库不包含任何游戏文件，也不包含任何翻译后的游戏代码。** 它在你的 Mac 上，把你自己的《黑色行动 II》Steam
> 副本变成一个 Mac 应用。游戏的代码会进入这个应用，所以没有人能给你一个现成的：每位玩家自己构建，且不得分享。
> 非官方粉丝项目，与 Activision、Treyarch 或 Microsoft 无关。想玩，请购买正版游戏。测试版：请阅读
> [已证实与未证实的内容](docs/PRERELEASE.md)。

---

<img src="docs/assets/rule-01.svg" alt="" width="100%">

## 概要

bo2-mac 读取《黑色行动 II》的三个可执行文件：战役（`t6sp.exe`）、僵尸模式（`t6zm.exe`）和多人模式（`t6mp.exe`），
提前把它们的 x86 机器码转换成 ARM64 机器码。[Remill](https://github.com/lifting-bits/remill) 把每个函数提升为
LLVM IR，我们自己的一系列 pass 改写这些 IR，结果再与一个手写的 Win32 运行时链接，其中的 Direct3D 11 由 Metal 绘制。
没有 Wine，也没有 JIT：游戏以原生代码在单个进程中运行。三个可执行文件共约 150,000 个函数被翻译。只有复制保护
隐藏的那些短小代码段在一个小型 x86 解释器中运行，翻译遗漏的代码也是如此：漏洞的设计结果是损失速度，而不是让游戏崩溃。

在 Apple M1 上，战役以 **每秒 51–60 帧** 运行，所有任务都能加载并衔接到下一个，直到片尾字幕。僵尸模式可以通过
游戏自己的菜单单人游玩 **六张地图**，帧率 **57–60**。多人模式通过真实菜单对战机器人，十一种游戏模式，帧率
**53–60**。一个应用 `Black Ops II.app` 打开战役，每个菜单中的 CAMPAIGN / ZOMBIES / MULTIPLAYER 入口会切换到另一个游戏。

除了你自己，不会从任何人那里下载任何东西：转换器读取你购买的副本中的可执行文件，逐字节校验，然后在你的机器上构建应用。

<img src="docs/assets/rule-02.svg" alt="" width="100%">

## 结果

| | 测量值 | 条件 |
|---|---|---|
| 战役 | **51–60 帧/秒** | 所有任务都能加载并衔接到片尾字幕；并非全部完整通关 |
| 僵尸模式 | **57–60 帧/秒**（Die Rise 43–58） | TranZit、Mob of the Dead、Nuketown、Origins、Buried、Die Rise，单人，通过真实菜单 |
| 多人模式对战机器人 | **53–60 帧/秒** | 通过真实菜单的十一种游戏模式：完整一局、计分板、返回大厅 |
| 游戏世界相对真实时间 | **≈ 1.0×** | 游戏自己的服务器时钟对比墙上时间 |
| 在玩家 Mac 上转换 | M1 上 **约 1 小时 45 分**（仅战役：约 45 分钟） | 战役一转换完成即可游玩；僵尸模式和多人模式随后完成 |
| 磁盘 | 峰值约 **10 GB**，保留约 **4 GB** | 转换后只保留链接好的二进制文件 |

帧率统计的是真正呈现到屏幕上的 drawable，已对照屏幕和 Metal HUD 核实。

**你可以玩什么**

<table>
<tr>
<td width="33%" valign="top"><img src="docs/assets/card-public-campaign.svg" alt="战役" width="100%"><br>
真实的主菜单：NEW STORY、RESUME STORY、REPLAY MISSION。从 <code>angola</code> 到片尾字幕，各任务依次加载并衔接。
Strike Force 任务点击一下即可开始。</td>
<td width="33%" valign="top"><img src="docs/assets/card-public-zombies.svg" alt="僵尸模式" width="100%"><br>
六张地图，单人：主菜单、大厅、对局、Game Over、计分板，然后在同一进程中开始下一局。
如果你的 Steam 账户拥有 DLC 地图，就可以游玩。</td>
<td width="33%" valign="top"><img src="docs/assets/card-public-multi.svg" alt="多人模式" width="100%"><br>
从真实菜单进入 Custom Games 对战机器人：CHOOSE CLASS、HUD、完整一局、计分板，然后返回大厅。
对局规则是本项目自己的脚本。</td>
</tr>
</table>

三种模式都支持：键盘和鼠标，或通过 GameController.framework 以 XInput 方式使用 DualSense；默认开启声音；移动键按
你键盘上的位置对应（AZERTY 上是 ZQSD，QWERTY 上是 WASD），除非你在游戏选项里自己绑定过。

<img src="docs/assets/rule-03.svg" alt="" width="100%">

## 流程

五个步骤，全部在玩家的 Mac 上完成：

1. **校验。** 转换器只接受每个可执行文件的确切 Steam 版本，用 SHA-256 校验。启动器会询问你 Mac 上的 Steam
   客户端，当前登录的账户是否拥有这款游戏，否则拒绝启动。
2. **编目与提升。** Ghidra 列出你自己的可执行文件中的函数；Remill v6.0.1 把每个函数提升为 LLVM IR。
   [tools/remill-patches/](tools/remill-patches/) 中带有五个针对提升器的补丁，其中包括 x87 舍入模式，以及一个
   让正在运行的 Remill 为每个函数 fork 一次、而不是每次重新加载其 x86 语义的补丁。
3. **折叠。** Remill 的 IR 是正确的，但很慢：每次客户机内存访问、每个标志位、每次 x87 压栈都是一次调用或一次
   写入 `State` 结构。我们的 pass 在 `clang -O2` 之前改写它：客户机读写被内联，`lock` 序列变成硬件原子操作，
   客户机的 `ret` 留在提升后的代码内部，x87 栈被提升为局部变量。
4. **运行时。** [runtime/](runtime/) 实现了游戏真正调用的 Windows 导入函数，带有精确的 stdcall 参数数量（Steam API
   的参数数量取自 Valve 公开的头文件）。客户机内存是一个 4 GiB 的映射。Direct3D 11 是 Metal 之上的伪 COM 层；DXBC
   着色器通过 [HLSLcc](third_party/HLSLcc) 转换。XAudio2 输出到 CoreAudio，视频用 libvpx 原生解码。
5. **线程。** 客户机线程最初作为协程运行在一个主机线程上，消除了大部分不确定性来源，使一次运行可以重放。快速配置把服务器、渲染后端、
   混音与解码以及任务工作线程移到真正的主机线程上。ARM 不是 TSO，所以这需要真正的内存屏障。

**复制保护是被执行的，而不是被绕过。** 游戏把完整性检查藏在硬件异常、单步陷阱以及它解密到堆中的代码之后。一个
基于 Unicorn 的小型 x86 机器（[x86_guest_vm.cpp](runtime/win32/x86_guest_vm.cpp)、
[heap_code.cpp](runtime/win32/heap_code.cpp)）正好执行这些代码段，然后把控制权交还给重编译后的代码。没有强制任何
检查结果，也没有手工放置任何密钥。

**翻译中的漏洞按设计只损失速度，不会崩溃。** 当游戏执行到从未被提升的 x86 代码时，同一台机器运行它的原始字节
（[interp.cpp](runtime/win32/interp.cpp)），并在下一个已翻译的块、调用或返回处把控制权交还给重编译代码。
每个被解释执行的地址都会记录在日志中（`[INTERP]`）。

**在线服务在本地应答。** 僵尸模式和多人模式需要 Demonware；一个根据游戏自身请求编写的应答程序
（[dw_local.cpp](runtime/win32/dw_local.cpp)）在你的 Mac 上应答大厅、存储和票据请求。没有任何数据离开这台机器。

<details>
<summary>为什么不用 Wine、CrossOver、DXVK 或 DXMT？</summary>

它们在运行时翻译 Windows 调用，x86 代码仍然交给 Rosetta。这里 x86 代码本身已经不存在：游戏启动之前它就已经是
ARM64 机器码。DXVK 和 DXMT 是 PE DLL，通过 Wine 的 unixlib 机制访问原生后端；本移植没有 Wine，它的 Direct3D 11
是自己在 32 位客户机内存之上实现的一层。只有着色器翻译需要外部组件，而 HLSLcc 这个普通的 C++ 库就够了。
</details>

<img src="docs/assets/rule-04.svg" alt="" width="100%">

## 发现

每一条都是：屏幕上看到的症状，测得的原因，以及修复的效果。

<details>
<summary><b>十三项发现</b>，从 x87 舍入模式到一条指令内部的错误起点</summary>


**F1 · 提升器忽略了 x87 舍入模式。** 在僵尸模式中，趴下时视角会抖动。在视角高度的网络编解码器中，游戏要求截断，
`fistp` 却按最近值舍入。客户端收到的高度 8 个样本错了 8 个；打上 Remill 补丁并重新提升编解码器后，8 个中错 0 个。

**F2 · x87 控制字中的一个保留位。** 一个战役任务在特效循环中冻结，而且只在真实线程下出现。提升器保存控制字第 6 位
的方式与真实硬件不同。一行 Remill 补丁加上四个重新提升的运行时函数就修好了，没有变慢。

**F3 · 一个 CPU 特性标志改变了算法。** 我们的 `IsProcessorFeaturePresent` 回答“没有 SSE2”，于是 C 运行时选用了
它的 x87 `ceil`，而在我们的运行时下它按最近值舍入。随后一个特效永远采样索引 −1：两个任务冻结。

**F4 · 卡顿来自我们自己的日志。** 170–430 毫秒的卡顿，是渲染线程阻塞在 `write()` 中：我们的日志被重定向到一个
几乎写满的磁盘上的文件。改为通过管道发送给子进程后，每次运行的卡顿次数从 12 和 8 降到 0 和 0。

**F5 · 着色器翻译把常量移动了 4 个字节。** HLSLcc 把 `float` 之后的 `float2` 放在偏移 24，而 Direct3D 从偏移 20
读取。2,664 个着色器中有 31 个受影响，包括眼睛和准星。

**F6 · 保护必须运行。** 选项菜单 8 次中冻结 8 次。一个完整性检查会改写自己的返回地址，让它指向解密到堆中的代码，
而运行时忽略了这一点。像 Windows 那样执行这段代码后，3 次中 3 次没有冻结。

**F7 · 我们自己的安全防护就是那堵墙。** 我们的一个防护在游戏进行到 28 秒时制造了一堵墙。本项目最大的收益来自移除
我们自己的防护和消除伪共享，而不是来自巧妙的代码。

**F8 · 一个空的 API 桩看起来像服务故障。** 两个什么都不返回的桩（`CryptGenRandom`、`StringFromGUID2`）在僵尸模式中
看起来像网络故障。

**F9 · 线程优先级是程序的一部分。** `SetThreadPriority` 原本只是一个桩。把混音器的请求传给 macOS 之后，一次运行中
过短的音频缓冲从 908–1872 个降到 7–71 个，声音断续也随之消失。

**F10 · 原生蒙皮，逐位一致。** 用 NEON 重写的 CPU 蒙皮与原始 x86 结果逐位相同，让一个繁重场景从 17.8 帧提升到 30.3 帧。

**F11 · 贴花需要游戏的深度偏移。** 焦痕和血迹会随着镜头轻微转动而出现或消失。游戏要求对贴花使用深度偏移，而我们的
层把它设为零。把游戏的数值传给 Metal 后问题解决。

**F12 · 渲染线程每帧都在复制关卡几何体。** 在多人模式和僵尸模式中，它每帧向 GPU 复制 23–63 MB 顶点数据，大部分是
静态的。把静态缓冲常驻在 Metal 中后，每帧复制量降到 3.5–5 MB：多人模式 51.8 → 60 帧，僵尸模式 43.1 → 58.2 帧。
同样的改动对战役毫无作用，因为那里的节奏由调度线程决定。一个优化只对触及其瓶颈的模式有效。

**F13 · 一条指令内部的错误起点让战斗中的帧率减半。** 在最初几次从全新 Steam 副本进行的转换中，只要玩家开火，僵尸
模式就跌到 25 帧。热点函数在解释器中运行：编目把下一个函数的起点放在了某个根函数的第一条指令内部，于是它的有界
扫描解码出零字节，这个根被悄无声息地丢弃。像逐个提升工具早已做的那样，对这类根进行无界扫描后，它们回到了翻译代码中。

</details>

<img src="docs/assets/rule-05.svg" alt="" width="100%">

## 方法

**先预测，再测量。**

- 在读取结果之前写下预测：到本测试版为止，约有 1,100 条编号的预测。
- 每组至少运行三次。运行间噪声为 30–64%，所以在 n = 3 时，低于约 40% 的差异看不出来。
- 一个优化必须证明自己已生效：日志里必须显示它被触发。
- 没有进入关卡的运行是失败的运行，不是数据点。
- 屏幕胜过日志。当屏幕与探针矛盾时，错的是探针。

<details>
<summary><b>录制与重放、撤回、净室开发、分工</b></summary>

**确定性录制与重放。** 每个客户机线程都是同一主机线程上的协程，所以大部分不确定性来源都被消除；重放会与录制结果核对，而不是默认一致。三个边界捕获了所有外部
输入：导入分发器、窗口消息队列，以及被消费的音频缓冲。日志约 16 MB。它可以让同一次运行恰好只改变一个开关再跑一遍。

**出错的往往是测量工具本身。** 一路上撤回了近 200 条结论。占比最大的（31%）是工具回答了另一个问题；其次（17%）是
工具根本无法回答。所以这里的每个判定工具都会写明它的输入，并在输入缺失时拒绝回答。

**对局规则采用净室开发。** 多人模式和僵尸模式的对局规则来自一个 Steam 已不再提供的每日补丁文件。
[runtime/match_rules/](runtime/match_rules/) 中的替代品由一个从未看过原始脚本的团队编写：一个团队根据游戏写出行为
规范，另一个团队只依据这份规范写代码，两个团队之间只通过书面问答沟通，问答连同日期一并保存。

**分工。** AI 代理提供吞吐量：编写兼容层、在每次运行的数百万次调用中二分定位、阅读数以万计的代码行。人提供目标、
根据屏幕上真实所见做出的纠正，以及对讨好式结论的拒绝。这是一款游戏、数月的工作和大量的人工干预。

</details>

<img src="docs/assets/rule-07.svg" alt="" width="100%">

## 限制

- **仅限本地。** 多人模式和僵尸模式对接的是你 Mac 上的应答程序。目前没有在线游戏、没有局域网，也没有合作模式。
  League Play 会被拒绝。
- **对局规则是我们自己的。** 采用净室开发（见 [方法](#方法)）；某些地方的行为可能与原版不同。
- **发热。** 长时间游玩时 Mac 会非常热（M1 上报告 111–116 °C）。请插电游玩：使用电池时，macOS 会大幅降低游戏速度。插电能避免降速，但不能避免发热。
- **每张地图的第一局。** 在僵尸模式和多人模式中，着色器在地图加载时转换为 Metal，所以第一次加载更久，第一分钟仍可能轻微卡顿。在战役中，着色器在第一次出现时才转换，所以加载过场动画保持流畅，第一次经过某个地方时可能卡顿。之后全部缓存：下一次加载快，运行流畅。
- **窗口。** 游戏在 1280×720 的窗口中运行。
- **其他 Mac。** 只在一台 M1 上测量过。M2、M3 和 M4 应该更快，但这里不声称任何数字。
- **战役。** 所有任务都能加载并衔接；并非每个任务都完整通关过。
- **僵尸模式。** Grief 和 Turned 需要第二名玩家，目前还没有合作模式。
- **macOS 音频。** 当系统的 `coreaudiod` 状态异常时，战役会卡在第一个视频。检查
  `afplay -t 1 /System/Library/Sounds/Tink.aiff` 是否约 1 秒完成；重启 Mac 即可修复。

<img src="docs/assets/rule-08.svg" alt="" width="100%">

## 安装

你需要一台 Apple 芯片的 Mac、macOS 14 或更高版本、至少 **20 GB 可用空间**（转换峰值约 10 GB，最终保留约 4 GB）、
Homebrew，以及登录了拥有该游戏账户的 Steam for Mac。

```bash
git clone https://github.com/adrbn/bo2-mac.git && cd bo2-mac
brew install llvm@17 cmake ninja libvpx unicorn
pip3 install capstone
scripts/make_app.sh --from-source
```

打开 `Black Ops II.app`，指向你的游戏文件夹，然后让它完成转换：M1 上约 1 小时 45 分，战役最先完成，约 45 分钟。战役一转换完成即可游玩：
此时开始游玩的话，僵尸模式和多人模式会在你第一次从游戏菜单中选择它们时再转换；否则转换会把三个模式全部完成。如果磁盘可用空间低于 8 GB，转换会干净地停止，并从原处继续。

**更新，或转换停止时。** 在仓库文件夹中运行 `git pull && scripts/make_app.sh --from-source`：已经转换的部分会保留，转换从原处继续。更新只重做其改动涉及的部分：运行时的修复在 M1 上约 3 分钟重新链接三种模式，而不必重新转换。如果仍然失败，请附上它提示的日志开一个 issue。

转换器只接受确切的 Steam 版本，用 SHA-256 校验。Steam 可执行文件由 Valve 的 SteamStub 外壳加密，**本仓库不提供
任何移除它的方法**：转换器需要 `~/.bo2-native/steam/` 中三个已解密的映像（`t6sp.exe.unpacked.exe`、
`t6mp.exe.unpacked.exe`、`t6zm.exe.unpacked.exe`）。你需要用自己选择的独立工具，从你购买的副本中自行制作它们。
只需去掉外壳：上面的文件名和 SHA-256 校验会立即告诉你文件是否正确。切勿分享这些文件。

**DLC。** 启动器会询问你的 Steam 客户端你的账户拥有哪些 DLC，与游戏在 Windows 上的做法完全相同。当你拥有某张 DLC
地图且其文件位于你的游戏文件夹中时，就可以游玩。

<img src="docs/assets/rule-09.svg" alt="" width="100%">

## 仓库结构

| 路径 | 内容 |
|---|---|
| [runtime/win32/](runtime/win32/) | Win32 运行时：kernel32、调度器、Direct3D 11、声音、输入、本地服务应答程序 |
| [runtime/gfx/](runtime/gfx/) | Metal 后端：呈现、着色器缓存、渲染目标 |
| [runtime/match_rules/](runtime/match_rules/) | 多人模式和僵尸模式的净室对局规则 |
| [tools/](tools/) | 提升驱动、编目、IR pass、反汇编与分析工具 |
| [tools/remill-patches/](tools/remill-patches/) | 在 Remill v6.0.1 之上携带的补丁 |
| [scripts/](scripts/) | `make_app.sh`、Steam 安装与转换脚本、启动器 |
| [third_party/HLSLcc](third_party/HLSLcc) | DXBC 到 Metal 的着色器翻译 |
| [docs/](docs/) | 预发布状态、工具链 |

<img src="docs/assets/rule-10.svg" alt="" width="100%">

## 路线图

1. **本测试版**：从全新的 Steam 副本运行三种模式，并在构建它们的那台 Mac 之外的其他 Mac 上运行。你的报告（Mac 型号、
   帧率、哪里坏了）决定下一步做什么。
   第一份外部报告，2026-11-05：在一台 M2 Pro（16 GB）上，转换完成并且游戏启动，途中修复了三个问题
   （[#4](https://github.com/adrbn/bo2-mac/issues/4)、[#5](https://github.com/adrbn/bo2-mac/issues/5)、[#6](https://github.com/adrbn/bo2-mac/issues/6)）。
2. **无需终端**：下载并打开一个应用，然后把游戏文件夹拖到它的窗口上。它自带预先构建好的工具，所以在你的 Mac 上
   只编译游戏本身（需要 macOS 26 或更高版本；macOS 14 和 15 继续使用终端安装）。
3. **完全翻译**：仍通过 Unicorn 解释器运行的 46 个代码块会被提前翻译，每条翻译后的指令都与解释器对照检查；
   任何翻译后的函数都可以送回解释器执行，用二分法找出错误的那个。
4. **无需等待着色器**：着色器在第一局之前构建，而不是在启动时（多人模式约 3 分钟）和游戏中（出现新效果时卡顿）。
   已开始：着色器现在分批编译，之前会话用过的着色器会在启动时于后台编译。在一台没有 Xcode 的 M1 上的多人模式中
   （几次尝试，不是基准测试），一局前两分钟内的着色器编译时间，首次启动从 18–25 秒降到 10–14 秒，下一次启动约 5 秒；
   超过 300 毫秒的卡顿从 15–22 次降到 13–15 次，然后是 7 次。首次启动的卡顿次数仍然差不多。
5. **更短的转换**：M1 上从约 2.5 小时缩短到 1 小时 45 分（2026-11-06，三种模式实测：准备阶段只读取一次可执行文件，
   lift 和 fold 按内存允许使用全部核心）。更新现在只重做改变的部分（运行时修复：约 3 分钟）。下一步：很少运行的代码减少优化。三种模式只有 4% 的
   函数相同，所以共用代码只转换一次收益很小。
6. **更快，且不发热**：战役受限于调度线程而不是渲染；11 个机器人的多人模式受限于服务器线程。下一步是把最热的函数
   改写为原生代码，然后为 120 Hz 屏幕提供高于 60 的帧率。
7. **手柄**：振动和辅助瞄准。
8. **一起玩**：真正的 UDP 套接字，用于僵尸模式合作、Grief 和 Turned，以及局域网多人模式。之后是 PC 版从未有过的本地分屏：
   两份游戏画在同一个窗口里，每人一个手柄，通过这个局域网连接。
9. **以后**：MetalFX、呈现时的后处理，以及从玩家自己的副本加载的替换纹理。

<img src="docs/assets/rule-11.svg" alt="" width="100%">

## 法律声明

适用于用户自己拥有的副本，运行在用户自己拥有的硬件上。

- 未使用任何引擎源代码或泄露代码。
- 既不分发输入的二进制文件，也不分发重编译的输出。不包含任何游戏文件：玩家需要自己的《使命召唤：黑色行动 II》
  Steam 副本。转换器只接受确切的 Steam 版本（SHA-256），并且每次启动游戏时都由玩家自己的 Steam 客户端检查所有权：
  Steam 未登录或该账号不拥有游戏时，游戏拒绝运行。
- 复制保护按游戏原本的写法执行；其中没有任何部分被修补掉。本仓库不提供任何移除 Valve SteamStub 外壳的方法。
- 游戏所需的在线服务由一个根据游戏自身请求编写的本地应答程序应答：不连接任何 Activision 服务器，没有在线游戏，
  游戏所有权仍由玩家的 Steam 检查。对局规则采用净室开发。
- 本项目编写的代码采用 GPL-3.0 许可（[LICENSE](LICENSE)）。
- Unicorn（GPL-2.0）在构建时从 Homebrew 静态链接（[tools/boot.py](tools/boot.py)）。GPL-2.0 与 GPL-3.0 的代码不能
  合在同一个二进制文件中发布，因此路线图第 2 步的预构建工具无法按目前的链接方式内嵌 Unicorn。在你自己的 Mac 上
  从源码构建不受影响。

这是一个非官方粉丝项目。Call of Duty 和 Black Ops 是 Activision Publishing, Inc. 的商标。Steam 是 Valve Corporation
的商标。本项目与 Activision、Treyarch、Microsoft 或 Valve 无关，未获得其认可或赞助。想玩，请购买正版游戏。

<div align="center"><img src="docs/assets/footer.svg" alt="" width="100%"></div>
