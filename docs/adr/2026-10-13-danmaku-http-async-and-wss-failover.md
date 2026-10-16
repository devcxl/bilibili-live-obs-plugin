# ADR-007: Open Live HTTP 传输层异步化（QNetworkAccessManager）与 WSS 集群兜底

## 状态

已接受 (Accepted) — 2026-10-13

## 背景与问题

issue #19（AI 代码审查）遗留问题的核实结论（证据见 `docs/dev/research/danmaku-async-shutdown-and-failover.md`）：

1. **退出/重连阻塞**：`start_app` 在工作线程中同步执行（libcurl，`CURLOPT_TIMEOUT=10s`），`DanmakuWebSocket` 析构与 `start_connect_attempt()` 在 UI 线程 `join()` 该线程；OBS `OBS_FRONTEND_EVENT_EXIT` 回调（UI 线程、返回后前端口 API 立即失效）就在这条路径上，最坏冻结 UI 约 10s。
2. **配置数据竞态**：工作线程无同步读取 `cfg_->danmaku.*`，UI 线程设置对话框同时写入同一字段。
3. **未做官方要求的集群兜底**：官方 `/v2/app/start` 文档原文「wss 长连地址，返回多个不同集群的地址，默认使用第一个即可，用于生产环境请做好连接失败后切换集群的兜底逻辑」，而实现仅使用 `wss_link[0]`。
4. **场次会话泄漏（既有行为）**：每次 WSS 断线重连都重新 `start_app`，旧 `game_id` 从不 `end_app`，依赖官方 60s 无心跳自动关闭。

## 决策

### 决策 1：三个官方 HTTP 调用改为 UI 线程 QNetworkAccessManager 异步

- `OpenLiveClient` 收敛为纯协议层：签名头（HMAC-SHA256）、请求体构造、响应解析；`post_start` / `post_heartbeat` / `post_end` 接收调用方注入的 `QNetworkAccessManager&`，返回在途 `QNetworkReply*`。
- `DanmakuWebSocket` 持有自己的 QNAM（UI 线程创建与使用，`setTransferTimeout(10s)`）；删除 `std::thread`、detached 线程与全部 `join()`。
- 关闭语义：`disconnect_from_room()` 对在途 start/心跳 `abort()`；`end_app` 允许完成；析构时若 `end_app` 在途，最多等待 1.5s（官方明确要求正常 end，否则下场次受 1 分钟冷却影响）。除此之外退出为 O(1)，不再受 `CURLOPT_TIMEOUT` 影响。

### 决策 2：凭据快照替代跨线程读配置

`OpenLiveCredentials{app_id, access_key, access_secret, code}` 在 UI 线程构造、按值传递（C++ Core Guidelines CP.31/CP.24）；`start_app` 成功后固定为「本场次快照」，供项目心跳与 `end_app` 复用，异步回调不再回读 `cfg_`。

### 决策 3：会话复用 + `wss_link` 多集群兜底

- `start_app` 成功后保存全部 `wss_link` 与 `auth_body`（官方场次会话）；WSS 断线重连**复用会话、不重新 `start_app`**（修复决策背景第 4 条的会话泄漏）。
- 未达 `Connected` 的失败：对当前链接打 60s 冷却并轮换下一集群；单会话内在链接间反复失败（阈值 `max(2, links×2)`）或项目心跳连续被官方拒绝（2 次）时，判定会话失效：`end_app` 结束场次后重新 `start_app`。
- 进入 `Connected` 后复位链接索引与冷却（回到官方默认的第一个链接）。
- 新增 INFO 日志记录 `wss_link` 实际数量，用于回填调研文档中的未验证项。

### 决策 4：疑似冷却退避对齐官方 1 分钟

`7001` 的「冷却期/已存在项目」语义**未获官方文档证实**（官方公开错误码页仅列 4000–4012）。保留该分支，但退避起点与封顶均改为 60s（对齐官方「未正常 end 需间隔 1 分钟」），避免以 5s/30s 持续冲击冷却期。

### 决策 5：`restore_live_state()` 复用 `start_danmaku()`

OBS 重启/插件重载的恢复路径不再自行 `connect_to_room`，统一委托 `start_danmaku()`（保留面板清空与推流状态文案），消除重复的连接编排逻辑。

## 替代方案与排除理由

| 方案 | 排除理由 |
|---|---|
| 保留 std::thread + libcurl，加停止标志 | `curl_easy_perform` 无法从外部中断，退出仍须等待或 detach；无法给出 O(1) 退出保证 |
| QtConcurrent + QFutureWatcher | 官方文档明确 `QtConcurrent::run()` 返回的 future 不可取消，仍需 `waitForFinished()`，退化回阻塞 |
| curl_multi 长驻工作线程 | 需自建事件循环、超时、取消与线程回收，复杂度远超收益 |
| 退出时也立即 abort `end_app` | 官方明确未正常 end 会影响道具投放与下一场次（1 分钟冷却） |

## 后果与影响

- 正面：OBS 退出与重连路径不再阻塞 UI 线程；配置竞态消除；符合官方生产环境的集群兜底要求；场次会话不再泄漏。
- 负面与缓解：`wss_link` 实际数量未能从一手来源确认；切换逻辑在 `size()==1` 时退化为无操作，并以运行时日志实测回填。
- 回滚边界：改动集中在 `src/danmaku-ws.*`、`src/danmaku/open-live-client.*`、`src/bili-dock.cpp`（restore 路径）与 `src/plugin-main.cpp`（移除 `set_api`）；协议编解码、消息解析、渲染与推流链路不受影响。

## 相关文档

- `docs/dev/research/danmaku-async-shutdown-and-failover.md`（一手来源调研报告）
- [ADR-006](2026-09-13-danmaku-connection-state-machine.md)：连接状态机（本 ADR 保持其 Closed/Connecting/Connected 语义不变）
