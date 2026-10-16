# 弹幕异步生命周期、WSS 集群切换与配置快照调研（2026-10）

> **状态**：调研完成，给出推荐方案与证据；落地决策见 [ADR-007](../../adr/2026-10-13-danmaku-http-async-and-wss-failover.md)（本报告不随实现变更，仅回填实测结论）。
>
> **范围**：仅回答「OBS 退出与连接切换时如何不阻塞 UI 线程、如何避免配置数据竞态、是否需要对 `wss_link` 做多集群切换」三类问题。
>
> **结论状态说明**：`CONFIRMED`=一手官方文档/源码已支持；`INFERRED`=由已确认事实推导；`UNVERIFIED`=未能从一手来源验证；`CONFLICTED`=可靠来源之间存在未消除的差异。
>
> **基线**：仓库 HEAD `21983d0`（2026-09-24）。

## 摘要

- **推荐主方案 A（`QNetworkAccessManager` 异步 HTTP + 关闭时 `abort()`）**：把 `start_app` / 项目心跳 / `end_app` 三个 HTTP 调用从「`std::thread` + libcurl 同步阻塞」改为「UI 线程上的 `QNetworkAccessManager` 异步请求」。`Qt6::Network` 已在 `CMakeLists.txt` 中链接、且 `src/bili-dock.cpp` 已在用，属于复用既有依赖。UI 线程不再有任何 `join()`；OBS 退出回调变为 O(1)，关闭延迟不再受 `CURLOPT_TIMEOUT=10s` 影响。
- **官方对 `wss_link` 的原文要求做多集群兜底**：官方 `/v2/app/start` 返回说明写明「返回多个不同集群的地址，默认使用第一个即可，**用于生产环境请做好连接失败后切换集群的兜底逻辑**」。因此「只用 `wss_links[0]`」与官方生产建议不符；但官方 Demo（一手）确实只用第一个，且未提供数组长度的确切分布。结论：设计上应支持切换，但保持极简（轮换 + 每主机冷却），并在数组长度为 1 时退化为无操作。
- **`7001` 的具体数值含义未能从官方文档验证**：官方公开错误码页只列出 `4000–4012` 与 FAQ 提到的 `4001/4009/5000/5002`。官方文档确实记录了「互动玩法未正常 `end` 需间隔 1 分钟以上才能再启动」的冷却语义，但未把该语义绑定到 `7001` 这个数字。
- **配置竞态**：`connect_async` 在工作线程读取 `cfg_->danmaku.*`，UI 线程在设置对话框里写同一字段，构成无同步数据竞争。推荐在 UI 线程构造不可变快照结构体、按值传给工作线程（C++ Core Guidelines CP.31 / CP.24），成本极低。
- **关闭模式参照**：obs-websocket 在 `obs_module_unload` 中停止服务并 `join` 其 I/O 线程（无超时，源码自带注释承认会拖慢调用线程）。本仓库当前把 `join` 放在 `OBS_FRONTEND_EVENT_EXIT` 回调（UI 线程）里，是最坏位置。

## 背景与问题定义

当前与本次问题相关的代码事实（均以 HEAD `21983d0` 为准）：

| 事实 | 证据位置 |
|---|---|
| 工作线程执行 `start_app`（libcurl 同步、超时 10s） | `src/danmaku-ws.cpp:103-106`、`src/danmaku/open-live-client.cpp:111` |
| 析构在 UI 线程 `join()` 工作线程 | `src/danmaku-ws.cpp:42-48` |
| 发起新连接时在 UI 线程 `join()` 上一个线程 | `src/danmaku-ws.cpp:85-106` |
| 工作线程读 `cfg_->danmaku.open_live_*`（裸指针，无同步） | `src/danmaku-ws.cpp:130-137` |
| UI 线程写 `cfg_->danmaku.open_live_*` | `src/bili-dock.cpp:1362-1369` |
| `end_app` 与项目心跳用 detached 线程 | `src/danmaku-ws.cpp:125-128`、`src/danmaku-ws.cpp:360-362` |
| 仅使用 `wss_links[0]`；项目心跳固定 20s | `src/danmaku-ws.cpp:170-175` |
| OBS 退出事件里卸载并删除全部服务 | `src/plugin-main.cpp:150-152`、`src/plugin-main.cpp:114-122` |
| 三态连接状态机与 `connect_gen_` 守卫 | `src/danmaku-ws.h:110-114, 186-187`；ADR-006 |

需要解决：退出/切换房间时 UI 线程最多阻塞约 10s（`start_app` 在途）；工作线程与 UI 线程对配置的无同步读写；官方生产建议的多集群兜底当前未实现。

## 研究方法

| 轮次 | 工作 | 结果 |
|---|---|---|
| Round 1 | 阅读仓库 `danmaku-ws.*`、`open-live-client.*`、`plugin-main.cpp`、`config-manager.h`、`bili-dock.cpp`；阅读 ADR-004/006 | 确认阻塞点、竞态点、状态机与设计约束 |
| Round 2 | 抓取 B 站 `open-live.bilibili.com/document` 一手页面与会话响应注释、FAQ、开放平台通用错误码页 | 确认 `wss_link` 多集群语义、20s 项目心跳、`end` 强制要求、冷却规则；`7001` 未获官方依据 |
| Round 3 | 拉取一手 Demo `bilibili-openplatform/OpenLive_CSharpDemo` 源码 | 官方 Demo 仅用 `WssLink.FirstOrDefault()`，未做多集群切换 |
| Round 4 | 拉取 obs-studio `OBSBasic.cpp`、`obs-frontend-api.h`、官方模块文档与 PR；拉取 obs-websocket `Stop()` 源码 | 确认 EXIT 在 UI 线程、EXIT 后前端口 API 被销毁、obs-websocket 关闭模式 |
| Round 5 | 抓取 Qt 6 文档（QNAM/QNetworkReply/QFuture/QtConcurrent/QThread）与 Ubuntu 24.04 包版本 | 确认线程亲和性、`abort()`、`QtConcurrent::run` 不可取消、Qt 6.4.2 |
| Round 6 | 抓取 C++ Core Guidelines（CP.20/CP.24/CP.26/CP.31/CP.60） | 配置快照与线程所有权依据 |

来源策略：B 站/Qt/obsproject 视为一手（B站官方=直播开放平台与开放平台文档；Qt官方=doc.qt.io；obsproject=官方 docs 与 GitHub 源码）；社区复述仅作辅助，不支撑结论。

## 关键发现

### 1. `wss_link` 的官方语义：多集群、默认第一个、生产需兜底切换（`CONFIRMED`，置信度：高）

**发现内容**：官方 `/v2/app/start` 响应示例中，`websocket_info.wss_link` 字段的原文注释为：

> 「wss 长连地址，返回多个不同集群的地址，默认使用第一个即可，用于生产环境请做好连接失败后切换集群的兜底逻辑」

即：地址是**不同集群的镜像**；官方允许直接用第一个；但对生产环境**明确要求实现连接失败后切换集群的兜底逻辑**。

**依据来源**：
- 《哔哩哔哩直播开放文档·项目开启》官方文档，访问 2026-10-13。[S1]

**边界与不确定项（`UNVERIFIED`）**：
- 数组**实际通常返回几个**（1 个还是多个、是否随集群负载变化）官方未给出明确说明，示例只给了 1 个元素的占位数组。因此「是否值得做多集群切换」无法仅凭官方文档判定，需要运行时实测（打印 `wss_links.size()`）才能确认价值。

### 2. 官方 Demo（一手）只用第一个链接，也无多集群切换（`CONFIRMED`，置信度：高）

**发现内容**：B 站官方组织 `bilibili-openplatform` 的 C# Demo 中，连接与重连均取 `WssLink.FirstOrDefault()`，未遍历/轮换其余链接；README 声明「基础功能，更多高级功能（例如重连、心跳等）请自行探索」。

**依据来源**：
- `bilibili-openplatform/OpenLive_CSharpDemo`（一手组织），`OpenBLive/Runtime/WebSocketBLiveClient.cs`：`var url = WssLink.FirstOrDefault();`（连接与重连两处）。[S2]

**含义**：官方 API 文档要求「生产做兜底切换」，但官方示例本身没做——说明这是「生产建议」而非「协议强制」。对本插件而言，多集群切换是**官方推荐但非阻塞项**。

### 3. 项目心跳官方推荐 20s；长连心跳 30s；两者不可混用（`CONFIRMED` + `CONFLICTED`，置信度：高）

**发现内容**：
- **项目心跳（`/v2/app/heartbeat`）**：官方推荐持续间隔 20s 调用；互动玩法超过 60s、插件/工具超过 180s 未收到项目心跳会自动关闭当前场次并停止长连推送。当前代码 20s 符合官方推荐。
- **长连（WS）心跳**：官方 Operation 表写明 `OP_HEARTBEAT`「客户端发送的心跳包(30秒发送一次)」，并说明「服务端 30s 内没有收到连续的心跳包则主动断开连接」。当前代码 WS 心跳 30s 符合。
- **`CONFLICTED`**：同一「长连数据协议」页面在开头信息点里另有一句「心跳频率 20s」，与其 Operation 表的 30s 以及开放平台镜像页的「频率 30s」矛盾。可理解为该句在指项目心跳，但官方页面自身表述不一致。

**依据来源**：
- 《项目心跳 / 心跳说明》官方文档。[S1]
- 《长链数据协议说明》官方文档（Operation 表 30s；页内另有 20s 表述）。[S3]
- 开放平台镜像《直播间消息 websocket 协议》（「频率 30s」）。[S4]

### 4. `end_app` 为强制调用；冷却语义有官方依据，但 `7001` 数值未获官方依据（`CONFIRMED` + `UNVERIFIED`，置信度：冷却规则高 / `7001` 未能验证）

**发现内容**：
- 官方**警告**：「游戏结束的时候，一定要调用 end 接口，否则将会影响礼物投放，会直接影响到您的收益」。
- `end_app` 语义：「项目关闭时需要主动调用此接口……调用后会同步下线互动道具等内容，项目关闭后才能进行下一场次互动」。
- 冷却规则：「互动玩法一个直播间只能同时启动一个，并且如果玩法没正常调用 end 结束，**需要间隔一分钟以上才能启动第二个**」。
- **`7001` 未能验证**：官方公开「请求通用错误」页只列出 `4000–4012`；FAQ 提到 `4001/4009/5000/5002`。检索到的社区/第三方资料也没有把 `7001` 绑定到「冷却期/已存在项目」。代码里「`7001` = 官方冷却期」目前是**未经验证的假设**。

**依据来源**：
- 《项目开启 / 项目关闭 / 项目心跳》官方文档。[S1]
- 《开放平台请求通用错误》官方错误码页。[S5]
- 《常见问题 FAQ》（含 4001/4009/5000/5002、冷却 1 分钟）。[S6]

**工程影响**：800ms 级退避（当前 `7001` 起点 5s）相对官方「1 分钟」冷却偏短；若确为冷却错误，5s 起步仍会持续触发限流。稳妥做法：对「疑似冷却」类失败把退避起点提高到 ≥ 60s，并保留 `end_app` 失败重试，而不是仅依据未验证的 `7001`。

### 5. OBS 退出事件在 UI 线程触发，且返回后前端口 API 立即失效（`CONFIRMED`，置信度：高）

**发现内容**：
- 官方文档：`OBS_FRONTEND_EVENT_EXIT`「在程序即将退出时触发。这是调用任何前端 API 进行保存/清理等的最后机会。**从该事件回调返回后，不允许再进行任何前端 API 调用**」。
- 源码：EXIT 由 `OBSBasic::closeWindow()`（UI 线程，经 `closeEvent` 进入）同步分发；紧接着执行 `obs_frontend_set_callbacks_internal(nullptr); api = nullptr;`，注释为「Destroys the frontend API so plugins can't continue calling it」。

**依据来源**：
- OBS 官方《Frontend API》文档。[S7]
- obs-studio 源码 `frontend/widgets/OBSBasic.cpp`（`closeWindow()` 中 EXIT 分发与前端口 API 销毁），master。[S8]

**推断（`INFERRED`，置信度：高）**：由于该回调运行在 UI 线程、且其后 OBS 还要继续走 `applicationShutdown()`，在其中做长时间阻塞（`join`）会冻结 UI 并拖延退出。官方没有明说「禁止阻塞」，但「EXIT 后 API 失效 + UI 线程同步分发」两条事实已足以判定：**不应把可能长达 10s 的线程 `join` 放在 EXIT 回调里**。

### 6. `obs_module_unload` 的官方定位是「保存数据 + 释放引用」，不得再调 libobs（`CONFIRMED`，置信度：高）

**发现内容**：官方模块文档与 PR 明确：`obs_module_unload` 仅用于保存设置与释放该模块持有的强引用；**返回后不得再调用 libobs API**；且因为可能仍有外部强引用，libobs 可能在 unload 返回后继续回调插件对象的 destroy，因此不能在 unload 里释放仍会被用到的资源。

**依据来源**：
- OBS 官方《Module API Reference》。[S9]
- obs-studio PR #13327「libobs, docs: Document intended use of obs_module_unload」。[S10]

**对本仓库的含义**：当前把重型卸载放在 `OBS_FRONTEND_EVENT_EXIT`（`dock_unload()`），而 `obs_module_unload` 里几乎不做实事。只要 `dock_unload()` 变为非阻塞，这个分工可保留；无需把清理挪到 `obs_module_unload`。

### 7. obs-websocket 的关闭模式：停服 + 等线程池 + `join()`（无超时）（`CONFIRMED`，置信度：高）

**发现内容**：obs-websocket 在 `obs_module_unload` 中调用 `WebSocketServer::Stop()`；`Stop()` 顺序为 `stop_listening()` → 逐个关闭会话 → `_threadPool.waitForDone()` → 以 10ms 步进忙等会话清空 → `_serverThread.join()`。源码注释自承：「This can delay the thread that it is running on. Bad but kinda required.」即它选择**阻塞等待而不设超时**。

**依据来源**：
- obs-websocket 源码 `src/websocketserver/WebSocketServer.cpp`（`Stop()`），commit `e28f849`。[S11]
- obs-websocket 源码 `src/obs-websocket.cpp`（`obs_module_unload`）。[S12]

**边界**：obs-websocket 是本地 WS 服务（websocketpp/Asio），其 I/O 线程关闭是可控的、秒级完成；本项目面对的是外部 HTTP（libcurl、10s 超时且 `curl_easy_perform` 无法从外部中断），两者不可直接照搬。可借鉴的是「关闭时集中停止 + 显式回收」，但不能无条件接受无超时 `join`。

### 8. Qt 6 关键语义（`CONFIRMED`，置信度：高）

- **QNetworkAccessManager 线程亲和性**：官方原文「One QNetworkAccessManager instance should be enough for the whole Qt application. Since QNetworkAccessManager is based on QObject, **it can only be used from the thread it belongs to.**」——UI 线程创建即在 UI 线程使用。[S13]
- **QNetworkReply 生命周期**：`abort()`「立即中止操作并关闭仍打开的连接」；被中止会得到 `OperationCanceledError`。**不要在 `finished()`/`errorOccurred()` 槽里直接 delete，用 `deleteLater()`**。[S14]
- **QNAM 析构与在途回复**：回复以 QNAM 为父对象，析构 QNAM 会一并删除回复；因此在 UI 线程上中止并销毁 QNAM 是安全的，且不会再有跨线程回调。[S13]
- **QtConcurrent::run 的 future 不可取消**：官方原文「the future returned by `QtConcurrent::run()` **cannot be canceled**」；基础模式下「canceling or pausing can be issued only if the computations behind the future has not been started」。只有 Run With Promise 模式（`QPromise::isCanceled`）才支持任务内主动取消。[S15][S16]
- **QThread 停止惯用法**：`requestInterruption()` / `isInterruptionRequested()` / `wait(unsigned long time)` 均在 Qt 6.4 存在，可用于「带超时等待」。[S17]

### 9. Ubuntu 24.04 的 Qt 版本为 6.4.2，推荐 API 均可用（`CONFIRMED`，置信度：高）

**发现内容**：Ubuntu 24.04（noble）`qt6-base` 源码包版本为 `6.4.2+dfsg-21.1build5`，即 Qt 6.4.2。本次推荐涉及的 API 均不晚于 6.4：`QNetworkAccessManager::setTransferTimeout`（5.15 起）、`QNetworkReply::abort`、`QFuture::cancel`、`QThread::requestInterruption`（5.2 起）。当前代码里的 `QT_VERSION_CHECK(6,5,0)` 兼容分支与此一致。

**依据来源**：
- Ubuntu 包页 `packages.ubuntu.com · noble · qt6-base`。[S18]

## 方案对比

| 维度 | (a) QNAM 异步（UI 线程） | (b) std::thread + curl + 停止标志 | (c) QtConcurrent + QFutureWatcher | (d) curl_multi 长驻工作线程 |
|---|---|---|---|---|
| 关闭延迟保证 | O(1)，无 `join`，`abort()` 即时 | 无外部中断手段；要么阻塞至多 10s，要么 detach（不安全） | `QtConcurrent::run` future 不可取消，仍需 `waitForFinished()` → 退化回阻塞 | 事件循环可控，但需自行设计与线程回收 |
| 取消语义 | `QNetworkReply::abort()` → `OperationCanceledError`，语义明确 | 停止标志对 `curl_easy_perform` 无效（无法打断进行中的传输） | 运行中不可取消（官方文档确认） | 可取消（`curl_multi_remove_handle`） |
| 线程安全 | 全程 UI 线程，无跨线程共享 | 需快照 + 原子标志 + 生命周期守卫，易漏 | 结果经 future 回传，但仍有阻塞点 | 需跨 线程消息/队列 |
| 代码量 | 中：复用 OpenLiveClient 的签名逻辑，只替换传输层 | 小改即可缓解阻塞，但要正确处理 `this` 生命周期 | 小，但收益有限 | 大（事件循环、超时、取消、回收） |
| 可测试性 | 中：签名逻辑可抽成纯函数单测；网络用 QNAM | 中 | 中 | 低 |
| 适配三态状态机 | 天然：`Connecting` 覆盖 start_app 在途与兜底等待；回复后开 WS 仍 `Connecting` 至鉴权回复 | 需要保留 `attempt_in_flight_` 与代次守卫 | 需要额外状态映射 | 需要额外状态映射 |
| 结论 | **推荐** | 仅作最小改动兜底 | 不采用为关闭方案 | 过度设计 |

**(a) 的落点**：`start_app` / 项目心跳 / `end_app` 改为 UI 线程 QNAM 异步请求；`disconnect_from_room()` 中 `abort()` 在途回复并由 QNAM 析构兜底。既消除 `join`，也顺带消除两处 detached 线程（`end_app`、项目心跳），符合 CP.26「不要 `detach()` 线程」。

**(b) 的落点**：保留 `std::thread` + curl，但在 UI 线程先构造配置快照（发现 9/CP.31），工作线程只持快照与 `shared_ptr` 共享态，禁止再触碰 `this`；退出时通过 `shared_ptr` 通知并允许「本次 `start_app` 结果被代次守卫丢弃」。可将阻塞从「必等 10s」降为「通常 <1 次请求周期」，但无法像 (a) 一样给出 O(1) 保证。

## 推荐方案

### 主方案 A：`QNetworkAccessManager` 异步化 + 配置快照

1. **传输层替换**：新增/改造一个基于 `QNetworkAccessManager` 的异步客户端，复用 `OpenLiveClient` 现有的 HMAC-SHA256 + MD5 签名与请求体构造逻辑（`src/danmaku/open-live-client.cpp`），仅把 `curl_easy_perform` 换成 `QNetworkAccessManager::post`。`start_app`、`send_heartbeat`、`end_app` 皆走异步。
2. **超时**：用 `QNetworkAccessManager::setTransferTimeout`（Qt 6.4 可用）设定上限，避免同步阻塞且行为可预期。
3. **关闭**：`disconnect_from_room()` 中 `abort()` 所有在途回复；`QNetworkReply` 不再由代码手动 delete（`deleteLater()` 或随 QNAM 析构）。`DanmakuWebSocket` 析构不再 `join()`。
4. **配置快照**：在 UI 线程构造不可变 `OpenLiveCredentials{app_id, access_key, secret, code}` 按值传入异步请求，彻底移除工作线程对 `cfg_->danmaku.*` 的直接读取。特别是 `end_app` 与心跳应使用「发起该会话时捕获的快照」，而非再次读 `cfg_`。
5. **状态机不变**：保持 `Closed/Connecting/Connected`（ADR-006）与 `connect_gen_` 代次守卫；`attempt_in_flight_` 由「线程在途」语义改为「HTTP 回复未返回」语义，其余退避与 7001 处理逻辑不变，仅按发现 4 把疑似冷却退避起点提高。

### 风险与回滚

| 风险 | 缓解 | 回滚 |
|---|---|---|
| QNAM 异步改造触及 `open-live-client` 传输层 | 签名/请求体构造保持纯函数、先加单测；先只迁移 `start_app` 再迁移心跳/`end_app` | 保留原 `OpenLiveClient` 同步实现，逐步替换；回滚只需恢复调用点 |
| 异步回复与代次/会话关闭竞争 | 回复处理入口统一做 `connect_gen_` 与 `session_active_` 校验（现有模式） | 恢复 `connect_gen_` 单一来源即可 |
| `setTransferTimeout` 行为与旧超时不一致 | 显式设定并记日志；与现有 10s 目标对齐 | 调整超时参数 |
| 直接改 `cfg_` 到快照可能遗漏字段 | 快照结构体只覆盖 Open Live 四字段，编译期穷举 | 恢复直读（会退化为已知竞态） |

**回滚边界**：改动集中在 `src/danmaku-ws.*` 与 `src/danmaku/open-live-client.*` 的传输调用点，不影响协议编解码（`danmaku-codec`/`danmaku-parser`）、展示与推流链路。

### WSS 多集群切换（发现 1/2 的直接落地，低复杂度）

- `start_app` 成功后保存**全部** `wss_link`（不是只取 `[0]`），记 `current_link_index_`，默认 0。
- 触发切换：同一次会话内，WSS 连接失败/鉴权回复失败且**未达到 Connected** 时，先轮换到下一个链接再进入整体退避（即「集群切换」优先于「整体退避」）。
- 每主机冷却：对刚失败的链接打时间戳，冷却窗口内不再选它（避免对已挂集群热循环）。
- 复位：进入 `Connected` 且稳定一段时间后，`current_link_index_` 复位为 0（回到官方默认）。
- 当 `wss_links.size() == 1` 时，以上逻辑退化为无操作。
- 明确不采用 RFC 8305（Happy Eyeballs）式并发探测：这些是**同协议不同集群的镜像**，不是 IPv4/IPv6 双栈，并发建连复杂度高、收益低。

**不确定项**：数组实际长度分布未能从一手来源确认，落地前应实测记录 `wss_links.size()`；若线上几乎恒为 1，可先把切换逻辑作为低优先级项。

## 待验证与不确定性

| 结论 | 置信度 | 备注 / 如何验证 |
|---|---|---|
| `wss_link` 为多集群、生产需兜底 | 高 | 官方原文；数组实际长度需实测 |
| 项目心跳 20s、长连心跳 30s | 高 | 官方文档；页面内部存在 20s/30s 冲突 |
| `end_app` 强制、未正常 `end` 有 1 分钟冷却 | 高 | 官方文档 |
| `7001` = 冷却期/已存在项目 | **未能验证** | 官方错误码页与 FAQ 未收录 `7001`；需权威渠道或抓包复核 |
| EXIT 在 UI 线程、返回后 API 失效 | 高 | 官方文档 + obs-studio 源码 |
| obs-websocket 关闭无超时 `join` | 高 | 源码；但其为本地服务，不可直接照搬 |
| QNAM 线程亲和、`abort()` 语义 | 高 | Qt 官方文档 |
| `QtConcurrent::run` future 不可取消 | 高 | Qt 官方文档（6.5 与 6.11 一致） |
| Ubuntu 24.04 = Qt 6.4.2，API 可用 | 高 | Ubuntu 包页 |

未能验证项的必要条件：官方在页面或应用后台明确给出 `7001` 语义，或使用真实凭证触发一次冷却并抓取返回 `code/message`。

## 参考来源

- [S1] 哔哩哔哩直播开放文档 · 项目开启/项目关闭/项目心跳（官方一手）：https://open-live.bilibili.com/document/eba8e2e1-847d-e908-2e5c-7a1ec7d9266f
- [S2] bilibili-openplatform/OpenLive_CSharpDemo（官方一手，`WebSocketBLiveClient.cs` 使用 `WssLink.FirstOrDefault()`）：https://github.com/bilibili-openplatform/OpenLive_CSharpDemo
- [S3] 哔哩哔哩直播开放文档 · 长链数据协议说明（官方一手，Operation 表 30s）：https://open-live.bilibili.com/document/657d8e34-f926-a133-16c0-300c1afc6e6b
- [S4] 哔哩哔哩开放平台 · 直播间消息 websocket 协议（官方一手，频率 30s）：https://open.bilibili.com/doc/4/5cac94fe-57f9-06db-7515-523d81c44f85
- [S5] 哔哩哔哩开放平台 · 请求通用错误（官方一手，4000–4012）：https://open.bilibili.com/doc/4/8673959e-f7bb-56e6-6e68-d225f971b81b
- [S6] 哔哩哔哩直播开放文档 · 常见问题 FAQ（官方一手）：https://open-live.bilibili.com/document/5dffc297-6fd2-41ff-bd45-6e8b89e2a68e
- [S7] OBS Studio · Frontend API（官方一手，`OBS_FRONTEND_EVENT_EXIT` 语义）：https://docs.obsproject.com/reference-frontend-api
- [S8] obsproject/obs-studio · `frontend/widgets/OBSBasic.cpp`（`closeWindow()` 分发 EXIT 并销毁前端口 API）：https://github.com/obsproject/obs-studio/blob/master/frontend/widgets/OBSBasic.cpp
- [S9] OBS Studio · Module API Reference（官方一手，`obs_module_unload` 定位）：https://docs.obsproject.com/reference-modules
- [S10] obsproject/obs-studio · PR #13327（`obs_module_unload` 文档化）：https://github.com/obsproject/obs-studio/pull/13327
- [S11] obsproject/obs-websocket · `src/websocketserver/WebSocketServer.cpp`（`Stop()`，commit `e28f849`）：https://github.com/obsproject/obs-websocket/blob/e28f84975984340e84fc4879cfc05741cc0afde1/src/websocketserver/WebSocketServer.cpp
- [S12] obsproject/obs-websocket · `src/obs-websocket.cpp`（`obs_module_unload`）：https://github.com/obsproject/obs-websocket/blob/e28f84975984340e84fc4879cfc05741cc0afde1/src/obs-websocket.cpp
- [S13] Qt 6 · QNetworkAccessManager Class（官方一手，线程亲和、回复父对象、`deleteLater()`）：https://doc.qt.io/qt-6/qnetworkaccessmanager.html
- [S14] Qt 6 · QNetworkReply Class（官方一手，`abort()`/`OperationCanceledError`）：https://doc.qt.io/qt-6/qnetworkreply.html
- [S15] Qt 6 · QFuture Class（官方一手，`QtConcurrent::run()` future 不可取消）：https://doc.qt.io/qt-6/qfuture.html
- [S16] Qt 6 · Concurrent Run（官方一手，Run With Promise 取消模式）：https://doc.qt.io/qt-6/qtconcurrentrun.html
- [S17] Qt 6 · QThread Class（官方一手，`requestInterruption`/`wait(unsigned long)`）：https://doc.qt.io/qt-6/qthread.html
- [S18] Ubuntu · noble (24.04) `qt6-base` 6.4.2+dfsg-21.1build5（官方包页）：https://packages.ubuntu.com/source/noble/amd64/qt6-base
- [S19] ISO C++ Core Guidelines（CP.20/CP.24/CP.26/CP.31/CP.60）：https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines

访问日期：2026-10-13
