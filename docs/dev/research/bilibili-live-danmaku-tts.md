# B站直播弹幕 TTS 方案调研（2026-09）

> **状态**：调研主体完成，音频路由和队列溢出策略已确认；已根据用户约束转为“远程优先”路线，待执行 PoC
>
> **范围**：仅调研与方案建议，本次未修改任何代码、构建配置或打包脚本。
>
> **结论状态说明**：`CONFIRMED`=官方资料、源码或多源证据已支持；`INFERRED`=由已确认事实推导；`UNVERIFIED`=需要 PoC 或法律/部署核验；`CONFLICTED`=可靠来源之间存在未消除的差异。

## 研究结论摘要

- **暂定建议（已根据用户偏好调整）**：优先用 Azure Speech 官方接口做远程 TTS PoC，先验证 Qt Network + REST raw PCM；只有 REST 首包延迟或流式控制不达标时，才评估 C++ Speech SDK。Azure F0 适合限额内评估，不应在商用发布前提下直接承诺；正式商用应使用付费层并完成条款核验。
- **本地路线定位**：`sherpa-onnx` 降为可选本地 fallback。若随 DEB/AUR 打包模型，必须在逐项完成权重、词典、转换产物和第三方依赖许可证审计后执行；不能因为框架是 Apache-2.0 就默认再分发。
- **已确认的音频路由**：主播本机播放 TTS，OBS 捕获承载 TTS 的桌面音频设备，观众即可听到；不把 OBS audio source 作为第一阶段必需组件。若需要隔离其他桌面声音或不依赖用户捕获配置，再增加显式 OBS audio source。
- **核心依据**：项目已有 `danmaku_received` 信号可作为独立 sink 的入口；Azure Speech 支持 `zh-CN`、异步合成、流式音频输出和 raw PCM；远程路线较少占用本机 CPU，但有网络、密钥、配额和服务条款约束。
- **暂定模型候选**：先比较 `vits-melo-tts-zh_en` 与 `vits-piper-zh_CN-chaowen-medium-int8`。前者模型元数据标注 MIT，后者数据集卡标注 CC0，但两者的完整再分发权仍需逐项确认，不能仅凭框架许可证或数据集许可证直接发布。
- **主要代价**：Azure F0 的 20 次/60 秒事务上限与弹幕峰值不匹配；需要合并/批量请求或付费层。C++ SDK 还带有再分发条款；REST 可减少 SDK 二进制依赖，但仍不能消除云服务条款和密钥管理成本。
- **最大风险**：桌面音频捕获没有捕获正确的播放设备、F0 限流导致队列持续增长，或 F0 音频输出商用权不足；本地模型授权与 OBS 内 native 稳定性仍需验证。
- **下一步**：先验证 Azure 远程合成与限流，再验证“本机播放 → OBS 桌面音频捕获 → 观众”链路；随后验证本地 fallback、合并和持久化恢复，PoC 通过后创建正式 ADR 并进入代码实现。

## 背景与问题定义

### 1. 项目现状

本次以当前仓库代码为事实基线：

| 项目 | 当前事实 | 证据 |
|---|---|---|
| 语言与标准 | C++17 | `CMakeLists.txt:7-9` |
| Qt | Qt 6，当前构建缓存显示 6.11；已使用 Core、Gui、Widgets、Network、WebSockets | `CMakeLists.txt:20-21` |
| 插件形态 | OBS `MODULE` 插件，运行在 OBS 进程内 | `CMakeLists.txt:56-57` |
| 分发 | Ubuntu 24.04 DEB 与 Arch/AUR 流程 | `.github/workflows/release.yml`、`aur/PKGBUILD` |
| 许可证信息 | `aur/PKGBUILD` 声明 MIT；仓库根目录当前未发现 `LICENSE*` 文件 | `aur/PKGBUILD:9` 与仓库扫描 |
| 当前 TTS | 没有 TTS、音频播放或音频源注入实现 | 当前源文件与依赖清单 |
| 弹幕入口 | `DanmakuWebSocket::danmaku_received` | `src/danmaku-ws.h:66-70` |
| 当前接收端 | `BiliDock::set_danmaku_ws()` 将弹幕连接到 `DanmakuDisplay::append_danmaku()` | `src/bili-dock.cpp:74-84` |
| 生命周期 | 开始/停止直播和 OBS 退出事件已集中在 `plugin-main.cpp`/`BiliDock` | `src/plugin-main.cpp:119-132` |

### 2. 决策问题

需要判断以下方案是否适合当前项目：

1. Linux 系统 TTS：`QTextToSpeech` + `speech-dispatcher` + eSpeak NG。
2. 本地离线神经 TTS：`sherpa-onnx` 与 `piper1-gpl`。
3. 云端流式 TTS：腾讯云与阿里云。
4. Azure Speech 官方 REST 与 C++ SDK 如何接入弹幕队列、播放设备和 OBS 音轨。
5. 用户要求“全部朗读、队列不丢弃、最多 1000 条”时，如何在远程限流和有限延迟之间取得可执行平衡。

### 3. 默认场景与假设

以下是已确认约束、暂定工程建议和仍待确认事项：

- **`UNVERIFIED`**：目标是中文短弹幕，单条大约 1～50 字，直播高峰可能每秒数条至数十条；需用真实直播数据复核。
- **已确认**：继续保持 MIT，后续补充正式 `LICENSE` 相关文件；插件只支持 Linux x86_64。
- **已确认**：弹幕、礼物、SC 等接收内容原则上全部朗读；朗读前过滤敏感词、重复弹幕、URL 和表情。过滤后的文本视为可朗读内容。
- **已确认**：若启用本地模型，模型随 DEB/AUR 打包分发，不采用运行时随机下载；正式打包仍以许可证审计为前置条件。
- **已确认**：队列最大长度为 1000，已接受消息不允许静默丢弃；普通弹幕合并为批次并保留全部文本，礼物/SC 保留边界；合并后仍无法消化时落盘持久化。
- **已确认**：优先使用免费远程 TTS，尽量降低本机 CPU 占用；本地 TTS 仅作为必要 fallback 或离线方案。
- **`UNVERIFIED`**：TTS 失败必须不影响弹幕接收、展示、开播和推流。
- **已确认的工程约束**：TTS 运行在 OBS 进程上下文，不能阻塞 Qt/OBS 关键线程；必须可停止、可清空队列、可降级。
- **已确认需求**：本机和直播观众都要听到，第一阶段通过“本机播放 + OBS 桌面音频捕获”满足。
- **`UNVERIFIED` 前置条件**：OBS 已捕获 TTS 实际输出的系统播放设备，并且用户接受该设备上的其他系统声音可能一并进入直播。
- **`PROPOSED`**：以单条消息从入队到开始播放不超过 30 秒作为初始直播体验预算；这是工程门槛，不是 Azure 或本机性能保证。
- **`PENDING`**：磁盘不可写/持久化失败时的最后保护动作，以及“停播/退出时不丢弃”是否也适用于已入队消息。

### 4. 非目标

- 不研究训练新模型、GPU 推理服务、ASR、语音克隆和服务端多租户架构。
- 不在本轮修改 `src/`、`CMakeLists.txt`、CI、AUR 或任何构建/打包文件。
- 不把厂商宣传的音质、延迟或免费额度直接视为本机可复现结果。

## 研究方法

### 1. 迭代轮次

| 轮次 | 工作 | 结果 |
|---|---|---|
| Round 1 | 阅读仓库结构、CMake、弹幕 WebSocket、生命周期和发行脚本 | 确认接入点、运行上下文、依赖与分发约束 |
| Round 2 | 查阅 Qt、Speech Dispatcher、eSpeak NG、sherpa-onnx、Piper 官方文档/源码/Release | 建立功能、许可证、模型、部署证据 |
| Round 3 | 查阅腾讯云/阿里云官方协议与价格、OBS 官方音频 API、社区 TTS/OBS 案例 | 补齐云成本、音频路由、队列和失败路径 |
| Round 4 | 主动搜索反方证据并交叉检查 | 发现 speechd 能力缺失、Piper GPL、模型授权/OOV、OBS 中文崩溃案例与版本冲突 |
| Round 5 | 核实 Azure Speech 免费层、配额、中文能力、Linux C++ SDK、REST raw PCM、低延迟建议和服务条款；搜索 edge-tts 反方证据 | 确认 Azure 具备远程低 CPU 接入条件，但 F0 事务上限、商用输出权和 SDK 再分发条款阻止无条件作为生产默认 |
| 当前 | 综合、标注未知项、设计 PoC | 给出暂定方案，不进入实现 |

### 2. 来源策略与评分

核心事实优先使用官方文档、官方源码和 Release；性能与工程风险再使用官方 benchmark、Issue 和实际 OBS 插件案例交叉验证。来源评分采用五维制：权威性、一手性、时效性、独立性、可验证性，各 1～5 分；总分不超过 12 的来源只作辅助依据。

本次 `web_fetch_exa` 直接抓取官方页面曾返回 `401 Invalid API key`，因此部分官方页面依据搜索结果摘要、GitHub 源码页和 Release 元数据整理；没有把无法直接复核的摘要当成已完成 PoC。许多官方文档不提供独立发布日期，报告将标注“未标注，访问日期为 2026-09-01”。

### 3. 证据等级限制

- 官方文档可以确认 API、协议和许可证声明，但不能替代目标硬件上的延迟/音质测试。
- Raspberry Pi 或第三方机器上的 RTF 不能直接外推到本项目用户的 x86_64 直播机。
- 模型卡中的“dataset license”不等于模型权重、转换产物和再分发包的完整许可证。
- 社区 Issue 能发现失败模式，但原因未定位时只能标为风险线索，不能当作必现结论。

## 关键发现

### 1. 接入点适合增加并行 TTS sink（`CONFIRMED`，置信度：高）

**发现内容**：当前弹幕展示已经通过 Qt 信号/槽接收 `DanmakuMessage`。TTS 可以作为第二个独立消费者挂到 `danmaku_received`，不需要改动 WebSocket 协议解析主链路；礼物和 SC 也有独立信号，可映射为更高优先级。

**依据来源**：

- 本地 `src/danmaku-ws.h:18-47, 66-70` 定义弹幕、礼物、SC 消息和信号。
- 本地 `src/bili-dock.cpp:74-84` 展示了当前 signal → display 的连接方式。
- 本地 `docs/adr/2026-07-25-danmaku-websocket.md:91-107` 已确立“弹幕故障不影响核心开播/推流”的隔离原则。

**来源评分摘要**：本地代码是一手事实，不适用外部来源评分；现有 ADR 为项目内部一手设计记录，可验证性高。

**推断**：TTS 应是 pure sink。提交、清洗、排队、合成和播放异常均不能同步抛回 `DanmakuWebSocket` 或阻塞 `BiliDock`。

### 2. `QTextToSpeech` 的 speechd 后端只适合“本机播放”低成本尝试（`CONFIRMED`，置信度：高）

**发现内容**：Qt 6.11 的 `QTextToSpeech` 支持 `say()`、`enqueue()`、暂停/恢复、语速/音量/音调和能力查询；但 Linux `speechd` 引擎只声明 `Speak` 与 `PauseResume`，没有 `WordByWordProgress` 和 `Synthesize`，也不支持引擎专有参数。

**依据来源**：

- Qt《Qt TextToSpeech Engines》，Qt，Qt 6.11.1 文档版，页面未标独立发布日期，访问 2026-09-01：明确 speechd 需要 `libspeechd >= 0.9`，且无逐词进度和 PCM `Synthesize` 能力。[S1]
- Qt《QTextToSpeech Class》，Qt，Qt 6.11.1 文档版，页面未标独立发布日期，访问 2026-09-01：定义 `Capabilities`、`synthesize()`、状态机和异步 API；同时说明具体引擎能力不同。[S2]
- Qt `qtexttospeech_speechd.cpp` 与 `speechd_plugin.json`，Qt 官方源码，v6.11.1，版本标签日期未在页面独立标注，访问 2026-09-01：源码显示能力列表为 `Speak`、`PauseResume`，并通过 Speech Dispatcher daemon 工作。[S3]

**来源评分摘要**：S1/S2/S3 均为 Qt 官方文档或源码，权威性 5、一手性 5、时效性 4～5、独立性 4～5、可验证性 5，总分 23～25/25。

**工程影响**：

- 可以让主播本机听到合成结果，但不能用 `QTextToSpeech::synthesize()` 取得 PCM，再稳定地喂给 OBS 音频源。
- `speechd` 还依赖用户级 daemon、输出模块和 eSpeak NG 配置，安装不完整时应进入 `Error` 并静默降级。
- Qt 队列 API 不能替代产品级有界队列；高峰期仍需在 TTS 层做丢弃、优先级和停止策略。

### 3. speechd/eSpeak NG 的主要风险是部署和中文质量，而不是接入 API（`CONFIRMED` + `UNVERIFIED`，置信度：中高）

**发现内容**：Speech Dispatcher 是客户端库 + daemon + 输出模块的系统级链路；eSpeak NG 支持 `cmn` 普通话，但官方项目定位是轻量 formant synthesizer，中文混排、多音字和数字/拼音处理存在历史 Issue。实际 B站弹幕包含网络词、数字、英文、表情和不完整标点，端到端质量必须实测。

**依据来源**：

- Speech Dispatcher 官方文档，版本 0.12.0，页面未标独立发布日期，访问 2026-09-01：描述 daemon、输出模块和 eSpeak NG 集成方式。[S4]
- eSpeak NG 官方 `docs/languages.md`，持续维护，发布日期未标，访问 2026-09-01：列出 `cmn` Mandarin；官方仓库声明 GPLv3。[S5]
- eSpeak NG Issue #1044，eSpeak NG 社区/维护者讨论，发布时间页面未稳定显示，访问 2026-09-01：记录 Mandarin 与英文/拼音规则处理问题。[S6]
- Ubuntu/Debian 包页面与 Qt 文档搜索结果：`qt6-speech-speechd-plugin`、`libspeechd2` 和 daemon 为拆分包，具体发行版包版本会变化。[S1][S4]

**来源评分摘要**：S4/S5 为官方文档/仓库，权威性 5、一手性 5、时效性 4、独立性 4、可验证性 5，总分约 23/25；S6 为维护者 Issue，权威性 4、一手性 4、时效性 3、独立性 4、可验证性 4，总分 19/25。

**结论边界**：不能仅凭 eSpeak NG 支持 `cmn` 就承诺可接受的中文直播音质。必须使用真实弹幕语料测试“多音字、数字、英文、表情占位符、颜文字、网络缩写和生僻字”。

### 4. sherpa-onnx 的 Apache-2.0 框架层更适合离线 PCM/C++ 集成（`CONFIRMED`，框架置信度：高；模型置信度：中）

**发现内容**：sherpa-onnx 提供 C API/C++ API、离线 TTS 配置和 PCM 音频生成；官方文档支持构建 shared library，GitHub Release 提供 Linux x64 共享库资产。框架仓库许可证为 Apache-2.0。中文模型路线多于 Piper 原生接入，可比较 Melo、Matcha、VITS-zh-ll、AISHELL3 与 Piper 转制模型。

**依据来源**：

- sherpa-onnx 官方 C API 文档，**文档站点版本 1.3（不是软件 Release 版本）**，发布日期未标，访问 2026-09-01：展示 `SherpaOnnxCreateOfflineTts`、生成配置、回调和 shared/static 构建方式。[S7]
- sherpa-onnx 官方 GitHub 仓库与 LICENSE，持续维护，访问 2026-09-01：仓库声明 Apache-2.0，并包含 C/C++ 实现。[S8]
- sherpa-onnx v1.13.6 Release，k2-fsa，**2026-08-18**：提供 `sherpa-onnx-v1.13.6-linux-x64-shared-lib.tar.bz2`（约 9.1MB）及完整 shared 资产，且包含 onnxruntime 版本变更信息。[S9]
- sherpa-onnx 中文模型页面，k2-fsa，页面未标独立发布日期，访问 2026-09-01：列出 `vits-piper-zh_CN-chaowen-medium` 采样率 22050Hz、单说话人和 C API/C++ API 示例入口。[S10]

**来源评分摘要**：S7/S8/S9/S10 主要为官方文档、仓库、Release 和模型页面，权威性 5、一手性 5、时效性 4～5、独立性 4、可验证性 5，总分约 23～24/25。

S31/S32 对 Melo 的模型元数据和转换路径提供了一手线索，权威性 5、一手性 5、时效性 3～4、独立性 4、可验证性 4；由于它们不能单独证明所有权重与转换产物的再分发权，模型部分仍保持中等置信度。

**重要限制**：

- 框架许可证 Apache-2.0 不自动覆盖所有模型、词典、FST、音素数据和第三方库；每个发行模型都要单独保留许可证和校验哈希。
- 官方 RTF 表包含不同硬件、不同模型和不同线程数，不能直接作为本项目直播机承诺。
- sherpa-onnx 的 shared library 解决构建入口问题，但不等于解决 OBS 进程内 ABI/符号冲突、库搜索路径和打包问题。

### 5. piper1-gpl 的技术能力有吸引力，但当前许可证使其不适合默认嵌入（`CONFIRMED` + `INFERRED`，置信度：高）

**发现内容**：OHF-Voice 的 `piper1-gpl` 代码为 GPLv3，`libpiper` 提供 C 风格 API 和分块合成；但项目的 GPLv3、内嵌/构建 eSpeak NG、缺少稳定 Linux 预编译 C 库以及中文管线仍在演进，使它不适合当前 MIT/许可证意图未完全明确的插件作为默认依赖。

**依据来源**：

- `OHF-Voice/piper1-gpl` 官方仓库，创建 2025-03-28，持续更新至 2026-09-01：仓库声明 GPLv3；README 与 `libpiper` 目录说明 C API、onnxruntime 和 eSpeak NG data 路径。[S11]
- piper1-gpl v1.7.0 Release，OHF-Voice，**2026-08-15**：Release 资产主要为 Python wheel 和 sdist，未提供可直接用于本项目的预编译 Linux `libpiper` 资产。[S12]
- `libpiper/include/piper.h`，OHF-Voice 官方源码，访问 2026-09-01：展示 `piper_create`、`piper_synthesize_start` 等 C API。[S13]
- Rhasspy Piper 维护者 Issue/Discussion，2023 年，访问 2026-09-01：记录 Piper 从 MIT 项目迁移以及 eSpeak NG GPL 相关许可证讨论。[S14]

**来源评分摘要**：S11/S12/S13 为官方仓库、Release、源码，权威性 5、一手性 5、时效性 4～5、独立性 4、可验证性 5，总分约 23～24/25；S14 为维护者讨论，约 19/25。

**法律边界**：将 GPLv3 库链接进当前插件是否要求整个组合程序按 GPLv3 分发，取决于链接方式、发布方式和适用法律；本报告不是法律意见。由于当前仓库声明 MIT、根目录又缺少正式许可证文件，**工程决策上应将 piper1-gpl 视为需要法律确认的阻断项**，而不是用性能优势抵消。

### 6. 音频路由：本机播放加桌面捕获可满足两者（OBS 路径 `CONFIRMED`，项目选择 `INFERRED`）

**发现内容**：用户已确认采用“主播本机听到，同时由 OBS 桌面音频捕获传给观众”的路径。该路径只需要 TTS 输出到指定系统播放设备，但成立的前提是 OBS 捕获同一个设备。OBS 官方 `obs_source_output_audio()` 仍可将原始音频提交到带 `OBS_SOURCE_AUDIO` 的 source，作为未来不依赖桌面捕获或需要隔离系统声音的增强路径。

**依据来源**：

- OBS《Source API Reference》，OBS Project，文档页面版本显示 32.1.0，发布日期未标，访问 2026-09-01：定义 `OBS_SOURCE_AUDIO` 和 `obs_source_output_audio()`，说明原始音频会被转换并上传到 OBS 管线。[S15]
- OBS《Backend Design》，OBS Project，发布日期未标，访问 2026-09-01：说明专用音频线程、环形缓冲、重采样、混音和编码输出流程。[S16]
- Qt `QAudioSink` API，Qt 6 文档，页面未标独立发布日期，访问 2026-09-01：本机音频输出接口；它不是 OBS source 注入接口。[S17]

**来源评分摘要**：S15/S16/S17 为官方 API/架构文档，权威性 5、一手性 5、时效性 4、独立性 4、可验证性 5，总分约 23/25。

**结论**：

- **首选路径**：使用 `QAudioSink`/平台音频输出到指定设备，由 OBS Desktop Audio 捕获；这同时满足主播和观众，但插件不能保证用户的 OBS 捕获设置正确。
- **显式 OBS 路径**：使用 sherpa/云 API 获取 PCM，再通过 OBS source 注入；需要 source 注册、激活/释放、时间戳、停止清空、音量与场景配置 PoC，作为隔离桌面声音的后续选项。
- `speechd` 无 PCM `Synthesize`，因此不能作为观众可听路径的主后端。

### 7. 弹幕 TTS 必须采用“有界队列 + 单消费者 + 清洗 + 降级”，用户已确认“不丢弃”下的溢出策略（`INFERRED`，置信度：高）

**发现内容**：公开的 B站 TTS 实现普遍使用独立 worker、串行播放、有界队列、普通消息丢弃、礼物/SC 优先和停播清空。该结构与当前项目的 signal/slot 和“弹幕功能故障隔离”原则一致，但与用户已确认的“已接受消息不允许丢弃、队列最多 1000”不同，不能直接照搬。

**依据来源**：

- `xfgryujk/blivechat-tts`，2025-12-08，维护者源码：优先级队列、队列上限、单 worker 和播放串行化。[S18]
- `MerlinCN/kinoko7danmaku`，2026-08-01，项目源码：音频 worker、礼物合并、取消 worker 和队列清理。[S19]
- `rany2/edge-tts`，仓库持续更新至 2026-09-01：连接/响应失败类型、文本清洗和服务限制实现。[S20]
- 本地 `DanmakuMessage`/`GiftMessage`/`SuperChatMessage` 三类信号与现有 ADR。[S21]

**来源评分摘要**：S18/S19 为真实项目源码，权威性 4、一手性 5、时效性 4、独立性 4、可验证性 4，总分约 21/25；S20 为成熟开源实现，约 22/25；S21 是本仓库一手代码。

**已确认与建议的行为契约**：

1. 入队前过滤空文本、纯表情、黑名单、过长文本和明显 URL；表情占位符应移除或替换为可读词。
2. “全部朗读”指过滤后的弹幕、礼物和 SC 内容均保留语义并最终进入朗读路径；礼物/SC 是否抢占普通弹幕属于待定优先级策略。
3. 合成与播放只能由一个消费者控制同一音频 sink，避免重叠和竞态。
4. 队列上限固定为 1000；“不丢弃 + 不阻塞接收线程 + 有界内存”可以通过合并和落盘实现，但“不丢弃 + 有界延迟”无法在突发流量下保证。当前报告不再把“队列满时丢弃普通消息”作为默认行为。
5. **暂定体验预算**：单条从入队到开始播放的延迟目标为 10 秒内、硬性告警阈值为 30 秒；超过阈值仍不能静默丢弃，按已确认策略先合并、再持久化。
6. **已确认溢出策略**：普通弹幕合并为批次但保留全部文本；礼物/SC 不与普通弹幕合并并保留事件边界；合并后仍达到内存队列上限时写入持久化队列，恢复后按顺序继续朗读。
7. 停播、断开房间和 OBS 退出时取消当前任务、清空队列并释放设备/source；如果“不丢弃”也覆盖停播场景，则必须改为持久化后续恢复，不能默认清空。该生命周期语义仍待确认。
8. 后端错误只丢弃当前语音结果或切换 fallback，弹幕展示、WebSocket 和推流继续运行；这不等同于丢弃原始弹幕内容。

### 8. 腾讯云与阿里云的接口能力较强，但仍有账号、成本和运营约束（`CONFIRMED` + `INFERRED`，置信度：中高）

**发现内容**：腾讯云与阿里云均提供 WSS 流式 PCM/MP3 返回；阿里官方 C++ SDK 明确提供流式文本合成对象，腾讯 `stream_wsv2` 的官方文档描述了流式输入，但公开 C++ SDK 示例主要是一次性 `SetText → Start`，C++ 流式输入支持需要再验证。

**依据来源**：

- 腾讯云《流式文本语音合成》，腾讯云，**2026-03-27**：WSS、PCM/MP3、16/24kHz、默认并发、单会话 10000 字，以及依赖标点断句的限制。[S22]
- 腾讯云《语音合成计费概述（在线版）》，腾讯云，**2026-08-10**：基础/精品音色免费资源 800 万字符、大模型 10 万、超自然大模型 2 万；精品音色按字符计费的当前价格表。[S23]
- `TencentCloud/tencentcloud-speech-sdk-cpp`，腾讯云官方 GitHub SDK，页面未标独立发布日期，访问 2026-09-01：示例主要体现一次性合成模式。[S24]
- 阿里云《使用 WebSocket 协议实现流式文本语音合成》，阿里云，页面未标独立发布日期，访问 2026-09-01：`StartSynthesis`、多次 `RunSynthesis`、`StopSynthesis`、PCM 数据帧和 Stop 冲刷缓存。[S25]
- 阿里云《使用 C++ SDK 实现流式文本语音合成》，阿里云，文档页面显示 2026 年更新，访问 2026-09-01：`createFlowingSynthesizerRequest`、`sendText`、二进制回调和停止/超时处理。[S26]
- 阿里云《计费项》，阿里云，**2026-07-22**：普通 TTS 按调用次数计费，100 个有效字符以内计一次、单请求最多 300 个字符；计费规则与长文本按字符计费不同。[S27]

**来源评分摘要**：S22/S23/S25/S26/S27 为厂商一手文档，权威性 5、一手性 5、时效性 4～5、独立性 3～4、可验证性 5，总分约 22～24/25；S24 为官方 SDK 源码，约 22/25。

**工程判断**：

- 云服务可以得到较好的音色和 PCM 流，但每个主播都要处理账号、密钥、隐私、地区网络、配额和成本。
- 腾讯短文本按字符计费通常更匹配弹幕成本，但无标点缓存与 C++ 流式支持存疑；阿里 C++ 流式路径更明确，但普通 TTS 按调用次数会放大高频短请求成本。
- 对当前“免费开源 Linux OBS 插件”形态，腾讯/阿里方案应是可选后端，不应成为默认安装后才能使用的硬依赖。这个结论是基于项目分发形态的推断，不是对云服务能力的否定。

### 9. Azure Speech 符合远程低 CPU 方向，但 F0 不能无条件承担生产流量（`CONFIRMED` + `CONFLICTED`，置信度：中高）

**发现内容**：Azure Speech 官方服务支持 `zh-CN` 普通话、C++ Linux SDK、异步文本合成、Pull/Push 音频流和 raw PCM 输出；REST API 也能直接请求 16/24/44.1/48kHz 的 16-bit mono PCM。它符合“远程优先、减少本机 CPU”的方向，但 F0 的配额和输出使用条款不适合被当作无条件的生产承诺。

**依据来源**：

- Azure《Language and Voice Support》，Microsoft，**2026-08-13**：列出 `zh-CN` Chinese (Mandarin, Simplified) 及可用语音能力。[S35]
- Azure《About the Speech SDK》与《Install the Speech SDK》，Microsoft，**2026-01-30**（安装页）：C++ 支持 Linux；列出 Ubuntu 20.04/22.04/24.04、Debian 11/12 及 glibc、OpenSSL、CA certificates、ALSA 依赖。[S36][S37]
- Azure《Text to speech REST API》，Microsoft，日期未标，访问 2026-09-01：列出 raw PCM 输出格式，包括 `raw-16khz-16bit-mono-pcm`、`raw-24khz-16bit-mono-pcm`、`raw-44100hz-16bit-mono-pcm` 和 `raw-48khz-16bit-mono-pcm`。[S38]
- C++ `SpeechSynthesizer`、`AudioOutputStream` API，Microsoft，日期未标，访问 2026-09-01：提供 `StartSpeakingTextAsync`、`CreatePullStream`/`CreatePushStream` 等接口。[S40]
- Azure《Speech pricing》，Microsoft，日期未标，访问 2026-09-01：Neural TTS F0 每月 50 万字符免费。[S33]
- Azure《Speech service quotas and limits》，Microsoft，**2026-06-26**：F0 对标准/自定义语音为 60 秒 20 次事务且不可调；S0 默认 30 TPS，最高可申请 1000 TPS。[S34]
- Azure《How to lower speech synthesis latency》，Microsoft，日期未标，访问 2026-09-01：建议复用 `SpeechSynthesizer`/连接并使用 Pull/Push stream 或 `Synthesizing` 事件降低首包延迟。[S39]
- Microsoft Cognitive Services Speech SDK license terms，Microsoft，`license_202311`，页面日期未标：允许再分发的代码有“加入主要功能、向分发者/终端用户传递同等保护条款”等要求，不能把 SDK 当作无条件 MIT 依赖。[S41]

**来源评分摘要**：S33-S40 为 Microsoft 官方价格、文档、API 和配额资料，权威性 5、一手性 5、时效性 4～5、独立性 3～4、可验证性 5，总分约 22～24/25；S41/S42 为官方许可证/产品条款，权威性 5、一手性 5、可验证性 5，但独立性按 3 分处理；S43 为 Microsoft Q&A 答复，权威性 3～4、一手性 3、可验证性 3～4，只用于呈现条款冲突，不单独支撑商用结论。

**关键推断**：F0 的 20 次/60 秒约等于平均每秒 0.33 次请求。若一条弹幕对应一次请求，它无法覆盖“每秒数条至数十条”的峰值；必须合并多条文本、接受明显排队延迟，或改用付费 S0。50 万字符额度解决的是月度字符计费，不解决事务速率和排队问题。

**商用与分发边界**：Microsoft Product Terms 的 TTS 条款写明，预置神经语音输出的商用使用权面向付费层 TTS 客户；Microsoft Q&A 的 **2026-03-03** 答复也建议商用使用付费层。此前存在相反的社区/版主答复，因此该结论标记为 `CONFLICTED`；对 MIT 插件的正式发布不能把 F0 视为已获商用授权，至少应按“F0 仅评估、生产使用用户自己的付费资源”设计，并在发布前取得书面法律确认。[S42][S43]

**工程判断**：

- **PoC 首选**：先用 Qt Network 调 REST raw PCM，避免第一步引入 Microsoft SDK 二进制、额外 ABI 和 SDK 再分发条款；若首包延迟/流式控制不达标，再验证 C++ SDK。
- **发布策略**：不内置 Azure 密钥，不把 F0 额度写成产品保证；让用户配置自己的 endpoint/key 或受限 token，并提供关闭远程 TTS 的选项。
- **本地 fallback**：只在网络、429、配额或服务条款不满足时使用已审计的本地模型；模型随 DEB/AUR 打包，但不能在许可证未确认前发布。
- `edge-tts` 是非官方 Edge Read Aloud 协议实现，使用服务端点的稳定性和商用许可均无保障，且项目为 GPLv3、返回 MP3；不能作为当前 MIT 插件的正式 Azure 替代后端。[S20][S44]

## 对比分析

### 1. 候选方案总览

先按硬性约束筛选，再使用成本和体验做权衡：

| 方案 | 定位 | 本机离线 | 可获得 PCM | 观众可听路径 | 中文质量预期 | 分发/许可证 | 初筛结论 |
|---|---|---:|---:|---|---|---|---|
| Qt `speechd` + eSpeak NG | 保守、系统集成 | 是 | 否 | 可通过桌面捕获间接满足；不适合作为直接注入 | 低到中，必须实测 | 系统依赖多；GPL 组件在进程外，但仍需打包/法律核对 | 可作本机 fallback/PoC |
| Azure Speech F0/S0 | 远程、低本机 CPU | 否 | 是，官方支持 raw PCM | 本机播放或 OBS source 均可 | 高，`zh-CN` 神经语音 | F0 50 万字符/月但 20 次/60 秒；商用输出权需付费层/条款确认；SDK 另有再分发条款 | **远程优先 PoC；F0 仅评估，生产按 S0/付费资源设计** |
| sherpa-onnx + 许可明确的中文模型 | 中间、离线神经 fallback | 是 | 是 | 本机或 OBS source 均可 | 中到高，取决于模型 | 框架 Apache-2.0；模型/依赖需逐项审计；模型随包分发 | 可作 fallback/离线路线 |
| piper1-gpl/libpiper | 性能优先 | 是 | 是 | 本机或 OBS source 均可 | 中到高，中文生态演进中 | GPLv3；C 库集成和发行资产不够直接 | 当前许可证意图下暂不选 |
| 腾讯云流式 TTS | 云端、成本优先 | 否 | 是 | 可转 OBS source | 高，音色多 | 密钥、网络、按字符计费；C++ 流式支持待验证 | 备选 |
| 阿里云流式 TTS | 云端、C++ 集成优先 | 否 | 是 | 可转 OBS source | 高，音色多 | 官方 C++ 流式 SDK；密钥、网络、按次计费 | 必须上云时优先验证 |
| `edge-tts` | 非官方 Edge Read Aloud 协议 | 否 | 主要为 MP3 | 需解码后播放 | 高但服务行为不可控 | GPLv3；无官方商用保证；协议可能失效 | 不纳入正式候选 |

### 2. 四种实施路线

| 路线 | 适用场景 | 时间成本 | 金钱成本 | 复杂度/维护 | 迁移与回滚 | 最坏情况 |
|---|---|---|---|---|---|---|
| **A 保守：speechd 本机播放** | 主播和观众都听到；前提是 OBS 捕获同一播放设备，且接受系统 TTS 音质 | 低到中：Qt 模块、运行时探测、清洗/队列 | 低；主要是系统包 | 低代码复杂度，但依赖 daemon、插件路径和发行版差异 | 最容易回滚；禁用 TTS 即恢复现状 | 用户缺少 daemon/voice，完全无声；或其他桌面声音被带入直播 |
| **B 中间：sherpa-onnx PCM** | 需要离线、较好中文质量、可控队列；本机播放或显式 OBS source 均可 | 中到高：模型、线程、音频 sink、打包与 PoC | 无云调用费；增加约数十 MB 库加模型存储 | 中高；需要处理 ABI、模型、音频时钟与版本 | 后端接口可抽象；模型可替换；失败可回退 A/静默 | in-process native crash 可能带崩 OBS；音频时间戳错误会丢音/延迟漂移 |
| **C 当前优先（激进）：Azure Speech 远程 TTS** | 追求低本机 CPU 和自然音色，接受用户配置 Azure 资源 | 中：REST/SDK、鉴权、流式播放、限流和隐私 | F0 可用于评估；生产商用按 S0/付费资源估算 | 网络、微软 SDK/服务条款、密钥和配额增加维护面 | 可保留 B 作为本地 fallback；REST/SDK 后端可替换 | F0 20 次/60 秒导致弹幕风暴积压；断网/429/欠费导致无声；商用权判断错误 |
| **D 其他云流式 TTS** | 追求高拟人音色，愿意管理账号和持续费用 | 中到高：WSS、鉴权、流式播放、限流和隐私 | 按字/按调用；成本随弹幕量增长 | 网络、厂商协议、密钥和配额增加维护面 | 可保留 B 作为本地 fallback；退出云服务相对容易 | 断网/429/欠费导致无声；密钥泄漏或账单失控 |

**推荐排序（按用户已确认的资源与成本偏好）**：Azure 官方后端 C > sherpa-onnx 本地 fallback B > speechd A；其他云 D 作为替代。这里的“优先”只表示 PoC 和产品路线优先级，不表示 F0 能无条件满足生产或商用要求。音频输出第一阶段仍采用本机播放 + OBS Desktop Audio 捕获；只有 PoC 证明桌面捕获无法满足隔离或稳定性要求时，才切换到显式 OBS source 路径。

### 3. 中文模型候选比较

| 模型 | 路线 | 已知体积/采样率 | 许可证证据 | 优点 | 风险/待验证 |
|---|---|---|---|---|---|
| `vits-melo-tts-zh_en` | sherpa-onnx VITS | 约 163MB，44.1kHz（模型元数据/官方转换脚本）[S31][S32] | 元数据标 MIT；需核对转换产物和依赖 | 中英混读、模型质量候选 | 体积较大；桌面 x86_64 RTF、冷启动、弹幕 OOV 未测 |
| `vits-piper-zh_CN-chaowen-medium-int8` | sherpa-onnx 转制 Piper | 约 13MB 级 int8 资产；原模型 22.05kHz | Piper 模型卡的 dataset 标 CC0；权重/转换包再分发权需核对 | 体积小、适合离线、中文专用 | dataset license 不等于权重 license；中文真实弹幕质量未测 |
| `vits-piper-zh_CN-xiao_ya-medium-int8` | sherpa-onnx 转制 Piper | 约 13MB 级 int8 资产 | 资料显示数据集/模型存在非商用限制线索 | 体积小、G2PW/词典路线 | 当前开源再分发场景不宜默认采用，需法律确认 |
| `matcha-icefall-zh-baker` | sherpa-onnx Matcha | 约 73MB 模型 + 51MB vocoder（调研资料） | 官方模型资料标注 non-commercial | 资源/速度可能较好 | 商用/再分发不适合当前默认场景 |
| `vits-zh-ll` | sherpa-onnx VITS | 约 116MB | 许可证声明不完整 | 多说话人/中文路线 | 模型许可证与 RTF 均需核实，不作为首发包 |

**模型结论**：先用 Melo 与 Chaowen-int8 做同语料 A/B；若没有可保存的权重、数据集、转换脚本和第三方依赖许可清单，就不能把模型随 DEB/AUR 发布。

### 4. 音频输出路径比较

| 路径 | 输出对象 | 观众是否确定听到 | 首要依赖 | 主要风险 | 推荐用途 |
|---|---|---:|---|---|---|
| `QAudioSink`/系统音频 | 主播默认音频设备，并由 OBS Desktop Audio 捕获 | 是，前提是 OBS 捕获同一设备 | Qt Multimedia 或平台音频 | 设备变更、独占、其他桌面声音混入或未捕获 | **第一阶段路径** |
| OBS audio source + `obs_source_output_audio` | OBS source/混音/编码管线 | 是，前提是 source 加入场景并启用对应 mixer | libobs API、PCM 缓冲、时间戳 | OBS source 生命周期、时钟、重采样、插件崩溃 | 需要隔离时的正式路径 |
| 云端 MP3/WAV 播放 | 解码后再送入上面任一路径 | 取决于最终 sink | MP3/WAV 解码器或云端 PCM | 流式帧不是独立音频文件，不能逐帧错误解码 | 只有必须用云音色时采用 |

### 5. 评分矩阵（辅助，不替代硬性约束）

权重：功能匹配 25%、稳定性/恢复 20%、性能 15%、运维复杂度 15%、许可证/合规 15%、成本/退出 10%。分数为调研阶段估计，未验证项使用保守分，不代表实测。

| 方案 | 功能 25 | 稳定 20 | 性能 15 | 运维 15 | 许可 15 | 成本/退出 10 | 加权结果 | 置信度 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Qt speechd/eSpeak | 5 | 6 | 7 | 7 | 6 | 9 | 6.4/10 | 中 |
| Azure Speech F0/S0 | 9 | 6～8 | 8 | 4 | 5～7（F0/付费层条款不同） | 6 | 6.6～7.4/10 | 中 |
| sherpa-onnx + 已审计模型 | 8 | 6 | 7～9 | 5 | 8（模型审计后） | 8 | 6.9～7.6/10 | 中 |
| piper1-gpl | 8 | 5 | 9 | 5 | 2（当前发布前提） | 6 | 5.8/10 | 中高 |
| 腾讯云 | 8 | 6 | 8 | 5 | 5 | 6 | 6.5/10 | 中 |
| 阿里云 | 8 | 6 | 8 | 5 | 5 | 5 | 6.3/10 | 中 |

**解释**：Azure 的许可分取决于使用 F0 还是付费层，稳定性分取决于地区、连接复用和限流处理；sherpa 的范围来自“模型、OBS 进程稳定性和目标硬件”尚未验证；piper 的低许可证分不是技术评分，而是当前 MIT/分发假设下的硬性风险；云方案的功能分不抵消其网络、密钥和成本运营负担。

### 6. 影响面与改动范围

本轮没有实施改动；以下是通过 PoC 后可能产生的影响面：

- **新增组件**：TTS 后端、文本清洗器、有界队列、单 worker，以及本机 `QAudioSink` 或 OBS audio source sink；远程路线还需要 HTTP/TLS、鉴权、限流和重试状态。
- **接线位置**：在 `BiliDock::set_danmaku_ws()` 增加 `danmaku_received`/礼物/SC 的第二个消费者；不改 WebSocket 包解析和展示消费者。
- **构建与打包**：远程 REST 路线可复用 Qt Network，避免第一阶段随包分发 Azure SDK；若启用 C++ SDK 或本地 fallback，仍需评估 native library、模型、数据目录、再分发条款以及 CMake、DEB、AUR、CI 同步变更。
- **线程影响**：新增 TTS worker 和模型推理资源；Qt/OBS 主线程只做轻量提交、状态更新和 source 生命周期操作。
- **OBS 影响**：第一阶段依赖 Desktop Audio 捕获；若需要隔离桌面声音或不依赖用户捕获配置，才新增可配置/可激活的 audio source，并处理场景、mixer、采样率、时间戳和音量设置。
- **用户影响**：新增语音开关、音色、音量、语速、队列策略、Azure endpoint/key、网络隐私提示；若启用本地 fallback，还增加随包模型的磁盘占用和第三方许可证说明。
- **团队与维护者影响**：增加云 API/配额变化、凭据安全、native ABI（若使用 SDK/本地模型）、模型升级、崩溃回归、两种发行版打包和第三方许可证审计工作。
- **上下游影响**：上游 B站协议层无需感知 TTS；下游 OBS 音频管线可能被注入额外音频，但必须保持推流和弹幕展示故障隔离。
- **未来接手者影响**：必须保存版本、SHA-256、模型目录契约、fallback 语义和停止/清空不变量，避免仅靠隐含运行环境维护。
- **回滚边界**：若 TTS 初始化或 PoC 失败，应可通过关闭功能/移除模型后端恢复原有 WebSocket、展示和推流行为。

## 反方观点与分歧

### 1. “系统 speechd 已经足够，不需要神经模型”

- **支持理由**：系统包成熟、安装成本低、Qt 集成最短，适合低端机器和本机辅助读屏；speechd daemon 还能复用用户已有音频设置。
- **反方证据**：speechd 后端没有 PCM 合成能力，不能直接满足观众可听的 OBS source 路径；eSpeak NG 的 formant 音质和中文混排问题可能不适合直播。
- **判断**：A 方案可以通过 Desktop Audio 捕获同时满足主播和观众，但不能提供显式 OBS source 的隔离能力；若需求强调高质量或独立音轨，不能作为主方案。`CONFIRMED` 能力结论，音质结论仍需语料 PoC。

### 2. “Piper 速度最快，应该直接使用”

- **支持理由**：Piper/VITS 在公开 benchmark 中通常有较好 CPU RTF，C API 有流式分块输出，适合短句。
- **反方证据**：当前 `piper1-gpl` 为 GPLv3；最新 Release 主要是 Python wheel/sdist，`libpiper` 的发行集成仍需源码/依赖构建；中文 voice 和音素化管线在持续演进。
- **判断**：如果插件明确改为 GPLv3 并接受该生态，可重新评估；在当前许可证声明和免费分发前提下，先不选。许可证结论是工程风险判断，不是法律意见。

### 3. “云 TTS 免费额度足够，开源插件也可以默认使用”

- **支持理由**：Azure F0 提供每月 50 万字符 Neural TTS 免费额度，且 `zh-CN` 神经语音、raw PCM 和 C++/REST 接口均可用；腾讯和阿里也有免费资源或流式能力。[S33][S35][S36][S38]
- **反方证据**：Azure F0 只有 20 次/60 秒且不可调；用户仍需注册云账号、配置密钥；网络、地区、并发和欠费会影响直播；Microsoft Product Terms 将预置神经语音输出的商用使用权写在付费层，和部分问答/社区答复存在冲突。[S34][S42][S43]
- **判断**：Azure 官方后端适合作为本项目的远程优先 PoC 和可选后端；F0 不能作为高峰生产吞吐或商用授权的默认保证。正式发布应默认关闭、使用用户自己的资源，并将 F0 限制为评估/低流量场景。`CONFIRMED` 能力结论 + `CONFLICTED` 商用条款结论。

### 4. “edge-tts 没有 API key，因此是最合适的免费 Azure 替代”

- **支持理由**：`edge-tts` 使用 Edge Read Aloud 服务，不需要 Azure key，且能够获取自然语音和 MP3 输出。
- **反方证据**：它依赖非官方服务协议，服务端可能随时改变或限流；维护者明确不建议把它作为商业业务依赖；仓库 GPLv3，返回 MP3 还增加解码和流式边界处理。[S20][S44]
- **判断**：可用于个人实验，不纳入 MIT 插件的正式后端。免费不等于获得服务授权、稳定性保证或商用输出权。`INFERRED`。

### 5. “sherpa-onnx 是 Apache-2.0，所以可以无条件打包”

- **支持理由**：框架仓库有 Apache-2.0 LICENSE，提供 C API 和 shared library。
- **反方证据**：模型权重、数据集、词典、FST、eSpeak/piper phonemize、onnxruntime 和转换脚本可能有各自许可证；官方/社区资料显示部分中文模型为 non-commercial 或许可证不完整。
- **判断**：只能说框架许可证较友好；必须建立第三方清单并锁定模型文件哈希，完成法律核验后才能进入 DEB/AUR。

### 6. sherpa-onnx 版本信息存在冲突（`CONFLICTED`）

- 一次 GitHub/API 检索结果报告 v1.13.7 于 **2026-09-01** 发布。
- 官方 Releases 页面在本轮可稳定复核到的最新条目是 v1.13.6，发布时间 **2026-08-18**，并提供 Linux x64 shared 资产。[S9]
- **处理方式**：实现前不直接引用未复核的 v1.13.7；以可下载且可校验的 Release 为准，固定 tag、资产 URL、SHA-256 和 onnxruntime ABI。该版本差异不改变“优先 PoC sherpa”的方向，但会改变构建和打包细节。

### 7. OBS 中文 TTS 崩溃案例不能忽略，但也不能直接判定 sherpa 不可用

- `obs-squawk` 是将 sherpa-onnx 嵌入 OBS 插件的真实先例，证明集成方向存在工程路径。
- OBS 官方论坛的资源评价记录“使用中文 voice pack 后崩溃”，发布时间为 **2024-06-21**；原因未定位，可能涉及数据路径、G2PW/词典、onnxruntime 线程或 ABI，而非单一模型必然缺陷。[S30]
- **判断**：将其作为高优先级失败 PoC，而不是作为绝对淘汰结论。首次运行应在独立 helper/CLI 中验证模型，再在最小 OBS source 插件中验证。

## 风险与不确定性

### 1. 风险清单

| 风险 | 概率 | 影响 | 不确定性 | 最坏情况 | 缓解与回滚 |
|---|---|---|---|---|---|
| 桌面音频捕获设备配置不匹配 | 中 | 高 | 中 | 主播能听到但观众无声，或其他系统声音被带入直播 | 首轮固定输出设备并做端到端验证；UI/文档明确 OBS 捕获前置条件；需要隔离时切换 OBS source |
| native TTS 在 OBS 内崩溃 | 中 | 极高 | 高 | OBS 进程退出，直播中断或未保存状态 | 独立 CLI → 最小 source → 插件；不在主线程加载模型；必要时隔离 helper 进程；失败关闭 TTS |
| 模型许可证不能再分发 | 中 | 高 | 高 | DEB/AUR 发布被撤回或产生合规风险 | 锁定模型卡、权重、数据集、转换工具和依赖许可证；无证据不打包 |
| 中文 OOV/多音字/表情读错 | 高 | 中 | 高 | 语音内容错误、观众体验差 | 真实弹幕语料 A/B；清洗、别名、丢弃规则；允许切换模型 |
| 弹幕风暴导致队列延迟或达到 1000 | 高 | 高 | 中高 | 为满足“不丢弃”而持续朗读旧消息，超过 30 秒体验预算；若持久化失败还可能无法接收新内容 | 先过滤重复/URL/表情；普通弹幕合并、礼物/SC 保留边界；超过内存上限写入持久化队列；不得阻塞 OBS/Qt 关键线程 |
| 持久化队列磁盘不可写或磁盘耗尽 | 中 | 高 | 高 | 内存队列达到 1000 后无法继续保证“不丢弃”，可能迫使接收侧背压或暂停 TTS | PoC 注入权限错误、磁盘满和重启恢复；监控磁盘空间；在实现前确定最后保护动作 |
| 模型加载阻塞或 CPU 抢占 OBS | 中 | 高 | 中 | OBS UI/编码卡顿，掉帧或推流质量下降 | worker 线程、冷启动前置/懒加载选择、CPU/RSS/帧率 PoC、限制线程数 |
| 音频时间戳/采样率错误 | 中 | 高 | 高 | 爆音、断续、延迟漂移、音频丢弃 | 固定内部 PCM 格式；验证 16/22.05/44.1kHz 到 OBS 48kHz；单调时间戳和长时压测 |
| speechd/Qt 插件缺失 | 中 | 低中 | 中 | 本机无声 | `availableEngines()`/状态检测；UI 显示不可用；静默回退，不影响直播 |
| Azure F0 限流或月度额度耗尽 | 高 | 高 | 高 | 20 次/60 秒无法承载峰值，队列持续增长，或免费额度用尽后远程 TTS 停止 | PoC 实测批量合并、连接复用和 429；显示字符/事务用量；生产改用 S0/付费资源或启用本地 fallback |
| 云端断网、密钥泄漏或服务条款不满足 | 中 | 高 | 高 | TTS 全部失败、账单失控、账号风险，或发布后被要求停止商用输出 | 默认关闭、不内置密钥、受限 token/用户自有资源、TLS/超时；F0 只作评估；本地 fallback；发布前核对 Product Terms |
| Azure Speech SDK 再分发条款不满足 | 中 | 高 | 中高 | MIT 插件发行包无法合法携带 SDK，需临时改为 REST 或撤回包 | PoC 先用 Qt Network + REST；若用 SDK，保留 license/REDIST 条款并完成发布审计 |
| onnxruntime/OBS 其他插件符号冲突 | 中 | 高 | 高 | 加载失败或进程崩溃 | shared library 版本固定、可见性控制、干净 OBS 多插件验证；必要时进程隔离 |
| sherpa 版本/Release 不一致 | 中 | 中高 | 中 | CI 能编译但发行包缺库或 ABI 不匹配 | 固定已复核 tag 和资产，保存 SHA-256，不追踪浮动 branch |

### 2. 失败隔离原则

无论最终选择哪一后端，必须保持以下不变量：

1. TTS 初始化失败不能阻止 `DanmakuWebSocket` 连接和 `DanmakuDisplay` 展示。
2. 单条文本清洗、合成、解码或播放失败不能终止 worker，也不能传播到 OBS 开播/停播流程。
3. 队列必须有明确上限 1000；普通弹幕先合并、仍无法消化时写入持久化队列，礼物/SC 保留事件边界；已接受消息不得静默丢弃，不能无限增长或阻塞 OBS/Qt 关键线程。
4. 停播/退出必须可中断当前工作，并在可验证时间内清空音频残留；停播/退出后是否持久化尚未播放的消息属于待确认行为。
5. TTS 后端、队列和音频 sink 之间使用稳定的内部消息/PCM 契约，不能让云 SDK 或模型 API 渗透到 WebSocket 解析层。

### 3. 成本全景

#### 本地路线

- **软件成本**：sherpa shared library、onnxruntime 相关库、模型、词典/FST/音素数据；官方 Release 显示库资产约 9.1MB 级，模型从十几 MB 到一百多 MB，具体随模型变化。[S9][S10]
- **开发成本**：需要 worker、文本清洗、音频 sink、CMake/DEB/AUR 安装路径和许可证清单；只有在需要独立音轨时才增加 OBS source。
- **运行成本**：无按字网络费用，但消耗 CPU、内存和磁盘；OBS 内推理的资源竞争需要以目标机器实测。
- **维护成本**：模型更新、onnxruntime ABI、发行版库搜索路径、第三方许可证和崩溃回归测试。
- **退出成本**：保留 `ITtsBackend`/PCM sink 边界后，可删除模型后端并回退 speechd/关闭功能；不产生云协议数据锁定。

#### 云路线

- **Azure 直接费用与额度**：Neural TTS F0 每月 50 万字符免费，但同时有 20 次/60 秒的不可调事务上限；S0 为付费层，默认 30 TPS，额度可申请调整。[S33][S34]
- **Azure 计费边界**：免费字符额度不等于商用输出授权；Product Terms 对预置神经语音的商用输出权写明付费层条件，F0 正式商用应视为未确认。[S42][S43]
- **腾讯/阿里直接费用**：腾讯当前通用精品音色按字符计费并给出免费资源；阿里普通 TTS 按调用次数规则计费，短弹幕每条请求至少形成一次计费调用。[S23][S27]
- **估算公式**：
  - Azure：`月度字符 ≈ Σ 清洗后实际发送字符`；`请求数 = Σ 合成批次`，必须同时满足 F0 月度字符额度和 20 次/60 秒事务额度；S0 费用以当前定价页和账户区域为准。
  - 腾讯：`计费字符 ≈ 合成字符总量 - 可用免费额度`，再按账户、音色和日用量梯度计算。
  - 阿里普通 TTS：`计费调用数 = Σ ceil(每次请求有效字符数 / 100)`，具体单价以控制台当前套餐为准。
- **隐藏成本**：账号注册、密钥管理、隐私披露、地区网络、限流处理、云厂商协议升级、客服/配额和用户配置支持；Azure C++ SDK 还需要处理二进制再分发条款，REST 则增加自行维护鉴权、HTTP 流和 PCM 边界的成本。[S38][S41]
- **退出成本**：可以替换 WSS 后端，但需保留本地 fallback；若将云密钥下放给每个用户，无法集中控制账单和撤销已泄露密钥。

### 4. 元评审

- **剩余未知**：Azure 在目标区域和真实弹幕负载下的首包延迟、429 行为和 F0 可用吞吐；普通弹幕合并是否仍满足“全部朗读”；持久化队列在磁盘异常时的最后保护动作；“停播/退出时不丢弃”是否也适用于已入队消息；OBS Desktop Audio 是否能稳定捕获指定设备、是否接受其他系统声音混入；本地 fallback 模型的中文质量、OOV、权重与转换产物再分发权；OBS 内 sherpa 稳定性。
- **最弱证据**：Azure 真实首包延迟和可持续吞吐、商用输出权在 F0 下的最终法律解释、真实弹幕清洗效果、OBS 中文音色崩溃根因和 x86_64 目标机 RTF。目前主要来自官方限制、社区案例或不同硬件 benchmark，不是本仓库 PoC。
- **可能错误的假设**：项目会继续按 MIT/开放分发；用户可以提供 Azure 账号/密钥或接受付费层；F0 的 20 次/60 秒可以通过合并满足体验预算；持久化介质在高峰期可用；用户接受 Desktop Audio 捕获带来的系统声音混入；“不丢弃”不要求停播后继续朗读。
- **遗漏角度**：本轮未覆盖 Flatpak/AppImage 的沙箱与音频权限、低端 ARM/Wayland/PipeWire 组合、不同 OBS 音频 mixer 配置和正式法律意见；若扩大平台范围，候选集需要重新筛选。
- **继续一轮的最高边际收益**：不是继续阅读更多 TTS 宣传页，而是完成 Azure REST/SDK 的目标区域首包与限流实验、真实弹幕批量合并实验，以及本机播放 → OBS Desktop Audio 捕获 → 回放的端到端验证；随后再验证本地 fallback 的许可证和稳定性。

## 建议

### 1. 已确认的音频路由

采用“本机播放 + OBS 桌面音频捕获”的第一阶段路径：

- TTS 输出到用户明确选择的系统播放设备，主播可以直接听到。
- OBS 捕获同一播放设备，观众可以通过直播音轨听到。
- 该路径不是插件可独立保证的闭环；PoC 必须验证 OBS 设备选择、静音/监听、其他桌面声音混入和回声风险。
- 如果用户不接受桌面声音混入，或希望即使未配置 Desktop Audio 也能让观众听到，再实现独立 OBS audio source。

### 2. 推荐的技术路线

按用户已确认的“远程优先、免费优先、低 CPU”约束采用以下顺序：

1. **远程后端首选**：先用 Qt Network 调 Azure Speech REST，选择 `zh-CN` 语音和 raw PCM；复用 HTTPS 连接，按块接收音频并送入本机 sink。REST 通过后，再以 C++ SDK 对首包延迟和流式控制做对照。
2. **免费层边界**：F0 只用于 PoC、低流量和功能评估，不承诺高峰吞吐或商用输出权；正式商用按用户自己的 Azure 付费资源/S0 设计，不在插件中内置密钥。
3. **本地 fallback**：只有在断网、429、额度耗尽或用户选择离线模式时启用 sherpa-onnx；先固定已复核的 Linux x64 shared Release，再对 `vits-melo-tts-zh_en` 与 `vits-piper-zh_CN-chaowen-medium-int8` 做 A/B。许可证没有完整证据前不得随 DEB/AUR 发布。
4. **模型分发**：若启用本地 fallback，模型、词典、native library、版本、SHA-256、许可证和运行时搜索路径随 DEB/AUR 固定打包；不采用未经审计的运行时下载。
5. **架构**：`danmaku_received`/`gift_received`/`super_chat_received` → 敏感词/重复/URL/表情清洗 → 有界队列（最多 1000）→ 单 worker → TTS backend → PCM audio sink。过滤后的已接受内容不得静默丢弃。
6. **队列体验预算**：先以入队到开始播放 10 秒目标、30 秒告警阈值做 PoC；普通弹幕合并，无法消化时写入持久化队列；达到 1000 后不静默丢弃内容，礼物/SC 保留边界。
7. **音频 sink**：第一阶段使用本机音频设备 + OBS Desktop Audio 捕获；只有需要隔离桌面声音或摆脱用户捕获配置时，才使用 OBS audio source + `obs_source_output_audio()`。
8. **发布策略**：默认功能可关闭；远程配置、隐私、配额和失败原因可见；Azure/本地后端任一失败都不能阻断 WebSocket、弹幕展示、开播或推流。

### 3. PoC 计划与通过标准

#### PoC-1：Azure 远程 TTS 与 F0 限制

- **环境**：目标 Linux x86_64 直播机；先用 Qt Network + REST raw PCM，再对照 Azure C++ Speech SDK；固定 Azure region、`zh-CN` voice、连接复用和 PCM 采样率。
- **输入**：采样至少 500～1000 条真实弹幕，覆盖中文、英文、数字、emoji、颜文字、URL、无标点、重复刷屏和生僻字；按已确认规则清洗后保存测试集。
- **记录**：HTTP/SDK 连接建立时间、首个音频字节时间、首个可播放 PCM 时间、P50/P95、429/超时率、请求数、发送字符数、CPU、内存和网络流量。
- **专项验证**：单条请求与批量合并请求对比；验证 F0 20 次/60 秒限制、50 万字符/月额度、断网/429/服务错误恢复，以及复用连接是否降低首包延迟。
- **通过标准（建议值）**：功能正确；普通测试负载下首音和 P95 满足 10 秒预算；F0 限制可被准确计量并触发可见告警；远程失败不影响弹幕展示和推流。该结果不能推导出 F0 的生产或商用授权。

#### PoC-2：本地 fallback 模型与文本质量

- **环境**：目标 Linux x86_64 直播机；固定 sherpa Release、线程数、模型版本。
- **输入**：采样至少 500～1000 条真实弹幕，覆盖中文、英文、数字、emoji、颜文字、URL、无标点、重复刷屏和生僻字；去除敏感内容后保存测试集。
- **记录**：冷启动时间、首个 PCM 时间、RTF P50/P95、RSS、CPU、音频时长、OOV/丢词、人工可接受率。
- **通过标准（建议值）**：RTF 在目标机上稳定小于 1；冷启动和首音延迟满足 10 秒体验预算；连续 30 分钟无崩溃；人工可接受率至少 80%；失败文本可被过滤且不影响其他消息；许可证清单可进入随包审计。

#### PoC-3：本机播放与 OBS 桌面捕获

- 验证 TTS 输出设备与 OBS Desktop Audio 捕获设备一致；分别测试主播监听、直播录制/预览和实际推流回放。
- 验证音量、静音、停止当前音频、切换默认设备、设备消失、其他桌面声音混入和回声/重复捕获。
- **可选显式 OBS 路径**：若桌面捕获不满足隔离要求，再创建最小 OBS audio source，输入模型 PCM，验证加入场景后的音频、采样率转换、单调时间戳、source 停止和释放。
- **通过标准**：连续 30 分钟无爆音/重叠/时间戳漂移；主播和观众均能听到同一播报；停播后无残余音频；显式 source 释放时 OBS 无泄漏或崩溃。

#### PoC-4：弹幕压力与失败注入

- 以 20 条/秒持续 10 分钟，再以突发 100 条/秒压测；观察队列达到 1000 前后的延迟、合并率、持久化量、CPU、内存和 UI/推流稳定性，不再以“丢弃率”为默认指标。
- 模拟模型文件缺失、动态库缺失、Azure 断网、429、额度耗尽、合成超时、音频设备消失、磁盘不可写/耗尽、重启恢复和停播竞态。
- **通过标准**：内存队列有界；普通弹幕合并后保留全部文本；超出内存上限后持久化且可恢复；礼物/SC 事件边界不被合并；单条延迟和 1000 条上限有可观测指标；TTS 失败时弹幕展示和推流持续。

#### PoC-5：发行和许可证

- 在干净 Ubuntu 24.04 与 Arch 环境安装 DEB/AUR，确认库、模型和数据目录可发现。
- 对每个本地/SDK 文件记录来源 URL、版本/tag、SHA-256、许可证、归属和再分发条件；对 Azure F0/S0 记录服务条款和商用使用边界。
- 用 `ldd`/`readelf` 检查本地/SDK 共享库依赖、RPATH/RUNPATH 和潜在 onnxruntime 冲突；用一个含其他插件的 OBS 实例回归。
- **通过标准**：没有依赖开发机私有路径；未安装 TTS 依赖时插件可正常加载；MIT LICENSE、第三方清单和 Azure 使用条款得到维护者/法律确认；失败时可删除 TTS 资产并回退到无语音版本。

### 4. 重新评估条件

- Azure F0 不能满足实测的 10 秒/30 秒体验预算，或 20 次/60 秒限制无法通过合并缓解：改用 S0/付费层、增加本地 fallback，或重新比较腾讯成本与阿里 C++ SDK。
- Azure Product Terms 最终确认 F0 不具备所需商用输出权，或 SDK 再分发条款无法满足：正式版本只支持用户自有付费 Azure 资源，或改用 Qt REST/已审计本地模型。
- 用户明确接受注册账号/按量计费并要求其他云音色：重新比较腾讯成本与阿里 C++ SDK。
- 插件改为 GPLv3 或获得明确的 GPL 兼容分发许可：重新评估 `piper1-gpl`。
- 目标扩展到 ARM、Flatpak、AppImage 或 Windows/macOS：重新评估模型、音频 API、沙箱和打包路线。
- sherpa-onnx 停止维护、Release 不再提供目标 ABI、onnxruntime 版本与 OBS 冲突，或 PoC 出现不可接受崩溃：切换到隔离 helper、另一离线后端或暂时保留 speechd fallback。
- 目标弹幕量超过单 worker 可处理能力：在“不丢弃”前提下重新设计合并、持久化和接收侧背压，不靠无限扩大内存队列解决。

## 暂定决策记录（非正式 ADR）

**状态**：Proposed；尚未进入实现，也不替代正式 ADR。

**决定**：

1. TTS 作为独立弹幕 sink，不能进入 WebSocket/开播核心链路。
2. 不以 `QTextToSpeech`/speechd 作为高质量或 OBS 注入主后端。
3. 按用户偏好，以 Azure Speech 官方 REST 为远程优先 PoC；C++ Speech SDK 作为 REST 延迟/流式能力不足时的第二选择。
4. Azure F0 仅用于限额内评估；正式商用按用户自己的付费资源/S0 设计，不内置密钥；F0 商用输出权在 Product Terms 与问答资料之间存在冲突，发布前必须确认。
5. 以 sherpa-onnx + 已审计中文模型作为本地 fallback/离线路线 PoC；如果启用，模型随 DEB/AUR 打包，许可证未审计前不得发布。
6. piper1-gpl 暂不作为当前 MIT/分发前提下的默认依赖。
7. 所有经过过滤的弹幕、礼物和 SC 内容原则上朗读；过滤敏感词、重复弹幕、URL 和表情；队列最多 1000，已接受消息不允许静默丢弃。
8. 暂以入队到开始播放 10 秒为目标、30 秒为告警阈值；达到 1000 后普通弹幕合并、超限持久化，礼物/SC 保留边界。
9. 音频路由已确认；在 PoC 通过、许可证核验和发行验证前，不修改代码和打包依赖。

**后果**：

- 正面：远程路线降低本机 CPU 占用，Azure raw PCM 可接入现有音频管线；后端边界便于替换；本地 fallback 保留断网/离线能力。
- 负面：远程路线增加网络、密钥、配额、隐私和服务条款维护；F0 不能承诺峰值吞吐；若使用 SDK/本地模型，还增加 native ABI、模型和许可证维护。
- 回滚：未通过 PoC 时可关闭远程/本地 TTS，保留原有 WebSocket、展示和推流；若已实现后端抽象，可移除 Azure SDK/模型资产，切换 REST、speechd 或仅文字展示。

## 参考来源

> 日期为页面发布/更新日期；官方页面未提供独立日期时明确标注。访问日期统一为 2026-09-01。

| 编号 | 标题 | 发布者 | 发布时间/版本 | URL |
|---|---|---|---|---|
| S1 | Qt TextToSpeech Engines | Qt | Qt 6.11.1 文档版，日期未标 | https://doc.qt.io/qt-6.11/qttexttospeech-engines.html |
| S2 | QTextToSpeech Class | Qt | Qt 6.11.1 文档版，日期未标 | https://doc.qt.io/qt-6.11/qtexttospeech.html |
| S3 | speech dispatcher Qt backend source / plugin metadata | Qt | v6.11.1，文件日期未标 | https://github.com/qt/qtspeech/tree/v6.11.1/src/plugins/tts/speechdispatcher |
| S4 | Speech Dispatcher Documentation | Brailcom/Speech Dispatcher | 文档版本 0.12.0，日期未标 | https://github.com/brailcom/speechd |
| S5 | eSpeak NG languages / repository | eSpeak NG | 持续维护，日期未标 | https://github.com/espeak-ng/espeak-ng/blob/master/docs/languages.md |
| S6 | questions about mandarin data packet（Issue #1044） | eSpeak NG maintainers/community | 页面日期未稳定显示 | https://github.com/espeak-ng/espeak-ng/issues/1044 |
| S7 | C API — sherpa | k2-fsa | **文档站点版本 1.3**，日期未标 | https://k2-fsa.github.io/sherpa/onnx/c-api/index.html |
| S8 | sherpa-onnx repository / LICENSE | k2-fsa | 持续维护，访问 2026-09-01 | https://github.com/k2-fsa/sherpa-onnx |
| S9 | v1.13.6 Release | k2-fsa | **2026-08-18** | https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.6 |
| S10 | vits-piper-zh_CN-chaowen-medium samples | k2-fsa | 日期未标 | https://k2-fsa.github.io/sherpa/onnx/tts/all/Chinese/vits-piper-zh_CN-chaowen-medium.html |
| S11 | OHF-Voice/piper1-gpl repository | OHF-Voice | 创建 2025-03-28，持续维护 | https://github.com/OHF-Voice/piper1-gpl |
| S12 | piper1-gpl v1.7.0 Release | OHF-Voice | **2026-08-15** | https://github.com/OHF-Voice/piper1-gpl/releases/tag/v1.7.0 |
| S13 | libpiper C API (`piper.h`) | OHF-Voice | main，日期未标 | https://github.com/OHF-Voice/piper1-gpl/blob/main/libpiper/include/piper.h |
| S14 | Piper license discussion / issues | Rhasspy Piper maintainers/community | 2023，页面日期各异 | https://github.com/rhasspy/piper/discussions/271 |
| S15 | Source API Reference (`obs_source_t`) | OBS Project | 文档版本 32.1.0，日期未标 | https://docs.obsproject.com/reference-sources |
| S16 | OBS Studio Backend Design | OBS Project | 日期未标 | https://docs.obsproject.com/backend-design |
| S17 | QAudioSink Class | Qt | Qt 6 文档版，日期未标 | https://doc.qt.io/qt-6/qaudiosink.html |
| S18 | blivechat-tts | xfgryujk | 2025-12-08（调研时页面记录） | https://github.com/xfgryujk/blivechat-tts |
| S19 | kinoko7danmaku | MerlinCN | 2026-08-01（调研时页面记录） | https://github.com/MerlinCN/kinoko7danmaku |
| S20 | edge-tts | rany2 | 持续维护至 2026-09-01 | https://github.com/rany2/edge-tts |
| S21 | 本仓库弹幕结构与信号 | bilibili-live-obs-plugin | 当前代码 | `src/danmaku-ws.h`、`src/bili-dock.cpp`、`docs/adr/2026-07-25-danmaku-websocket.md` |
| S22 | 流式文本语音合成 | 腾讯云 | **2026-03-27** | https://cloud.tencent.com/document/product/1073/108595 |
| S23 | 语音合成计费概述（在线版） | 腾讯云 | **2026-08-10** | https://cloud.tencent.com/document/product/1073/34112 |
| S24 | TencentCloud speech SDK for C++ | 腾讯云 | 日期未标 | https://github.com/TencentCloud/tencentcloud-speech-sdk-cpp |
| S25 | 使用 WebSocket 协议实现流式文本语音合成 | 阿里云 | 日期未标 | https://help.aliyun.com/zh/isi/developer-reference/streaming-text-tts-wss |
| S26 | 使用 C++ SDK 实现流式文本语音合成 | 阿里云 | 文档显示 2026 年更新 | https://help.aliyun.com/zh/isi/developer-reference/c-sdk |
| S27 | 计费项 | 阿里云 | **2026-07-22** | https://help.aliyun.com/zh/isi/product-overview/pricing |
| S28 | chaowen voice model card | Piper Voices | **2026-01-29** 提交 | https://huggingface.co/rhasspy/piper-voices/commit/10eb5c756ae21b759c8344d54aef86f9399ae92d |
| S29 | obs-squawk：OBS + sherpa-onnx 先例 | royshil/locaal-ai | 2024-06 起，OBS 论坛资源页标注 Stalled | https://github.com/locaal-ai/obs-squawk |
| S30 | Squawk - Real-Time Local Text-to-Speech with AI（资源页及评价） | OBS Project Forum | **2024-06-21**；评价记录中文 voice pack 崩溃 | https://obsproject.com/forum/resources/squawk-real-time-local-text-to-speech-with-ai.1965/ |
| S31 | `vits-melo-tts-zh_en` model metadata | k2-fsa | Issue 页面 **2025-05-23** | https://github.com/k2-fsa/sherpa-onnx/issues/2241 |
| S32 | `vits-melo-tts-zh_en` ONNX conversion script | k2-fsa | 持续维护，日期未标 | https://github.com/k2-fsa/sherpa-onnx/blob/master/scripts/melo-tts/export-onnx.py |
| S33 | Azure Speech in Foundry Tools pricing | Microsoft | 页面日期未标，访问 **2026-09-01** | https://azure.microsoft.com/en-us/pricing/details/speech/ |
| S34 | Quotas and limits for Azure Speech | Microsoft | **2026-06-26** | https://learn.microsoft.com/en-us/azure/ai-services/speech-service/speech-services-quotas-and-limits |
| S35 | Language and Voice Support for Azure Speech | Microsoft | **2026-08-13** | https://learn.microsoft.com/en-us/azure/ai-services/speech-service/language-support |
| S36 | About the Speech SDK | Microsoft | 页面日期未标，访问 **2026-09-01** | https://learn.microsoft.com/en-us/azure/ai-services/speech-service/speech-sdk |
| S37 | Install the Speech SDK / setup platform | Microsoft | **2026-01-30** | https://learn.microsoft.com/en-us/azure/ai-services/speech-service/quickstarts/setup-platform |
| S38 | Text to speech REST API | Microsoft | 页面日期未标，访问 **2026-09-01** | https://learn.microsoft.com/en-us/azure/ai-services/speech-service/rest-text-to-speech |
| S39 | How to lower speech synthesis latency using Speech SDK | Microsoft | 页面日期未标，访问 **2026-09-01** | https://learn.microsoft.com/en-us/azure/ai-services/speech-service/how-to-lower-speech-synthesis-latency |
| S40 | C++ `SpeechSynthesizer` / `AudioOutputStream` reference | Microsoft | 页面日期未标，访问 **2026-09-01** | https://learn.microsoft.com/en-us/cpp/cognitive-services/speech/speechsynthesizer；https://learn.microsoft.com/en-us/cpp/cognitive-services/speech/audio-audiooutputstream |
| S41 | Microsoft Cognitive Services Speech SDK software license terms | Microsoft | `license_202311`，页面日期未标 | https://csspeechstorage.blob.core.windows.net/drop/license_202311.html |
| S42 | Microsoft Azure Services Product Terms（TTS Service output use rights） | Microsoft | 当前条款日期未标，访问 **2026-09-01** | https://www.microsoft.com/licensing/terms/productoffering/MicrosoftAzure/MCA |
| S43 | Please clarify the conflicting information regarding Azure TTS commercial use | Microsoft Q&A | **2026-03-03** | https://learn.microsoft.com/en-us/answers/questions/5805156/please-clarify-the-conflicting-information-regardi |
| S44 | Query regarding affiliation with Azure（edge-tts Discussion #261） | rany2/edge-tts | 页面日期未稳定显示，访问 **2026-09-01** | https://github.com/rany2/edge-tts/discussions/261 |

### 参考来源评分摘要

| 来源组 | 权威性 | 一手性 | 时效性 | 独立性 | 可验证性 | 总体用途 |
|---|---:|---:|---:|---:|---:|---|
| Qt/OBS 官方文档与源码（S1-S3、S15-S17） | 5 | 5 | 4～5 | 4 | 5 | 核心 API、能力和音频管线事实 |
| sherpa/Piper 官方仓库与 Release（S7-S13、S28） | 5 | 5 | 4～5 | 4 | 5 | 框架、版本、模型入口和许可证线索 |
| 云厂商官方文档（S22-S27、S33-S40） | 5 | 5 | 4～5 | 3～4 | 5 | 协议、价格、配额、中文能力、低延迟和 SDK/REST 能力 |
| Microsoft Product Terms/SDK 条款（S41-S42） | 5 | 5 | 4～5 | 3 | 5 | 商用输出权和二进制再分发边界；发布前仍需法律核验 |
| Microsoft Q&A/社区讨论（S43-S44） | 3～4 | 3～4 | 3～4 | 3 | 3～4 | 发现冲突和风险线索，不能替代 Product Terms |
| Speech Dispatcher/eSpeak 官方资料（S4-S6） | 5 | 5 | 4 | 4 | 5 | 系统链路、语言支持和中文风险 |
| 社区项目/Issue（S18-S20、S29-S30、S44） | 4 | 4～5 | 3～4 | 4 | 4 | 失败模式、架构实践和风险线索，不能单独证明核心结论 |
