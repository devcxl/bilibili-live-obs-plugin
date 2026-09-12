# ADR-005: 移除弹幕自动重连，连接关闭即退出弹幕互动

## 状态

已接受 (Accepted) — 2026-09-13

取代 [ADR-002](2026-07-25-danmaku-websocket.md) 的「决定 4：指数退避重连策略」，并修正 [ADR-004](2026-09-02-open-live-protocol-refactoring.md) 决策 1 中「指数退避断线重连」的描述。

---

## 背景与问题

`DanmakuWebSocket`（Open Live 官方长链模式）原先存在两处自动重连：

1. **断线自动重连**：`on_ws_disconnected()` 在非主动断开时 2 秒后重新建连。
2. **失败退避重试**：`start_app` 请求失败（含 `7001` 冷却期错误）、`wss_link` 为空时，按 `min(base × 2^n, 30s)` 指数退避反复重试。

启用该策略时弹幕连接不在开播流程的显式控制之下，产生以下实际问题：

| 问题 | 说明 |
|------|------|
| 冷却期放大 | `7001` 表示官方项目开启了同类项目或处于冷却期，自动重试会以 5s→10s→20s→30s 持续冲击接口，延长冷却 |
| 状态不可预期 | 用户点击停播/登出后，后台仍可能在重连线程中重新 `start_app`，与用户意图冲突 |
| 会话泄漏 | 旧的 `game_id` 会话在重试路径上被覆盖，`end_app` 可能未送达官方侧 |
| 交互语义模糊 | UI 显示「已断开」但实际仍在后台自愈，用户无法判断弹幕到底是否在工作 |

## 决策

**移除全部自动重连能力。连接关闭（或被对端关闭、请求失败）即视为退出本次弹幕互动，恢复只能由用户显式触发。**

具体约定：

1. 删除 `reconnect_timer_`、`reconnect_attempts_`、`RECONNECT_MAX_DELAY_MS` 及 `start_reconnect()` / `stop_reconnect()` / `attempt_reconnect()`。
2. 新增 `end_session()`：终止本次互动会话，停心跳、`end_app` 结束官方项目、复位状态并发 `connection_state_changed(false, 0)`。
3. 触发 `end_session()` 的路径：
   - WebSocket 断开（`on_ws_disconnected`，且会话仍处于活动状态）；
   - 鉴权失败（`AuthReply.code != 0`，逻辑上仍走 `ws_->close()`）；
   - `start_app` 请求失败或未返回 `wss_link`。
4. 以 `session_active_` 取代 `intentional_disconnect_` 的语义：会话活动期间的任何关闭都视为「退出互动」，`disconnect_from_room()` 主动断开时自行完成收尾，`on_ws_disconnected()` 不再重复处理。
5. 保留手动重连入口：`DanmakuDisplay` 的「重连」按钮 → `reconnect_requested` → `BiliDock::start_danmaku()`。
6. 首次连接仍为自动：开播成功、恢复开播状态、登录完成、开放平台设置保存后仍会调用 `start_danmaku()`。

状态文案同步为「已关闭」（断开态）/「已连接」（鉴权成功态）。

## 替代方案及排除理由

| 方案 | 排除理由 |
|------|----------|
| 保留指数退避自动重连（原方案） | 与用户显式控制冲突，`7001` 冷却期会被重试放大，后台重连不可见 |
| 仅对 `7001` 保留退避，其他错误不重连 | 退避逻辑与生命周期强耦合，保留半套机制仍需定时器与代次判断，复杂度不降反升 |
| 自动重连但设次数上限（如 3 次） | 仍存在「用户已停播但后台在重连」的窗口；上限用尽后行为与完全不重连一致，收益有限 |
| 断开即隐藏弹幕面板 | 用户失去弹幕历史阅读能力，且停播路径已有显式 `hide()` |

## 后果与影响

**正面**

- 连接生命周期完全由用户/开播流程驱动，行为可预期、可观测。
- 消除 `7001` 冷却期的自我放大，降低被官方限流的概率。
- 会话不再泄漏：任何失败路径都会调用 `end_app` 收尾。
- 删除定时器、代次退避相关的约 30 行状态机代码，`danmaku-ws.cpp` 生命周期更直观。

**负面与缓解**

| 影响 | 缓解 |
|------|------|
| 网络抖动会导致弹幕中断，需人工点击重连 | 状态栏显示「已关闭」，按钮常驻可见；后续可考虑「自动重连」作为可选配置项 |
| 长直播中用户可能未察觉弹幕已停 | `connection_state_changed(false, 0)` 已驱动 UI 状态更新；如需更强提示可另加 OBS 日志/托盘提醒 |

**回滚边界**

改动集中在 `DanmakuWebSocket` 生命周期与 `DanmakuDisplay` 文案，回滚只需恢复 `reconnect_timer_` 与 `start_reconnect()` 调用点，不影响协议编解码、消息解析、TTS 与推流链路。

## 相关文档

- [ADR-002: 弹幕 WebSocket 客户端架构设计](2026-07-25-danmaku-websocket.md)（决定 4 已被本 ADR 取代）
- [ADR-004: Bilibili 开放平台协议规范与弹幕模块分层重构](2026-09-02-open-live-protocol-refactoring.md)
