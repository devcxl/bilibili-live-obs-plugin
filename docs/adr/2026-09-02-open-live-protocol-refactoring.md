# ADR-004: Bilibili 开放平台 (Open Live) 协议规范与弹幕模块分层重构

## 状态
已接受 (Accepted) — 2026-09-02

## 背景与问题

原先的 `DanmakuWebSocket` 实现存在以下架构与设计缺陷：
1. **职责过载**：网络 I/O、二进制封包解包、zlib/Brotli 递归解压、业务 JSON 文本解析全部耦合在单个类中，难以单测与横向扩展。
2. **协议标准性不足**：原本直接解析 Web 端私有协议（`DANMU_MSG`, `SEND_GIFT` 等），缺少对 B 站官方开放平台（Open Live 官方标准长链）`LIVE_OPEN_PLATFORM_*` 命令集的支持。
3. **缺乏配置与凭证管理界面**：用户无法像 TTS 一样在界面中直观查看和配置弹幕模式（Web 扫码直连 / 开放平台官方凭证直连）、过滤选项以及敏感密钥加密。

官方参考规范：
* [长链数据协议说明](https://open-live.bilibili.com/document/657d8e34-f926-a133-16c0-300c1afc6e6b)
* [直播间数据规范](https://open-live.bilibili.com/document/f9ce25be-312e-1f4a-85fd-fef21f1637f8)

---

## 架构设计决策

### 决策 1：四层架构解耦设计

将弹幕体系解耦拆分为四个独立层次：

1. **帧协议层 (`src/danmaku/danmaku-packet.h`)**：
   * 定义 16 字节大端协议头结构与常数（`OP_HEARTBEAT=2`, `OP_HEARTBEAT_REPLY=3`, `OP_SEND_SMS_REPLY=5`, `OP_AUTH=7`, `OP_AUTH_REPLY=8`）；
   * 定义协议版本（`ProtoVer::Normal=0`, `ProtoVer::Popularity=1`, `ProtoVer::Zlib=2`, `ProtoVer::Brotli=3`）。
2. **编解码层 (`src/danmaku/danmaku-codec.h/.cpp`)**：
   * 纯逻辑无状态编解码器：
     * 封包：根据 op/protover 生成 16 字节 Header + Body；
     * 解包：粘包截断防护、多包连续切割分发；
     * 安全解压：zlib / Brotli 解压，硬限制解压输出最大 8MB，递归解压深度 ≤ 2。
3. **消息语义解析层 (`src/danmaku/danmaku-parser.h/.cpp`)**：
   * 优先解析官方标准命令：`LIVE_OPEN_PLATFORM_DM`、`LIVE_OPEN_PLATFORM_SEND_GIFT`、`LIVE_OPEN_PLATFORM_SUPER_CHAT`、`LIVE_OPEN_PLATFORM_GUARD`、`LIVE_OPEN_PLATFORM_LIKE`；
   * 自适应兼容 Web 客户端命令：`DANMU_MSG`、`SEND_GIFT`、`SUPER_CHAT_MESSAGE`、`INTERACT_WORD`、`ENTRY_EFFECT`；
   * 归一化输出统一领域事件：`DanmakuMessage`、`GiftMessage`、`SuperChatMessage`、`EntryMessage`。
4. **网络传输与生命周期层 (`src/danmaku-ws.h/.cpp`)**：
   * 基于 `QWebSocket`，仅负责长连接建立、握手鉴权、30 秒心跳保活、三态连接状态机与指数退避断线重连（详见 [ADR-006](2026-09-13-danmaku-connection-state-machine.md)）。

---

### 决策 2：弹幕与开放平台独立配置面板

* 在 `ConfigManager` 中新增 `DanmakuConfigData`：
  * `mode`: `0` (Web 扫码直连, 默认), `1` (Open Live 官方直连)
  * `open_live_app_id`: 项目 App ID
  * `open_live_access_key`: Access Key ID
  * `open_live_secret`: Access Key Secret（AES-128-CBC 加密持久化存储）
  * `open_live_code`: 主播开播身份码
  * `show_fans_medal`: 是否显示粉丝勋章
  * `show_guard_badge`: 是否显示大航海标志
* 在 `BiliDock` 弹幕控制栏增加「⚙」设置按钮与「弹幕与开放平台设置」对话框，支持实时修改、凭证保存与即时生效。

---

## 目录结构

```text
src/
├── danmaku/
│   ├── danmaku-packet.h       # 协议帧常量与结构定义
│   ├── danmaku-codec.h/.cpp   # 16字节封包解包、多包分包与zlib/brotli安全解压
│   └── danmaku-parser.h/.cpp  # 官方 LIVE_OPEN_PLATFORM_* 消息与 Web 消息双模解析器
├── danmaku-ws.h/.cpp          # 网络 I/O、鉴权握手与生命周期管理
├── config-manager.h/.cpp      # 弹幕配置与 Open Live 凭证加密存储
└── bili-dock.h/.cpp           # 弹幕设置 UI 与交互管理
```

---

## 收益与影响

1. **高标准**：严格对齐 B 站官方开放平台长链与数据规范；
2. **高安全**：彻底杜绝解压炸弹与递归栈溢出，密钥加密落盘；
3. **易扩展**：解析逻辑与网络传输完全解耦，未来若引入新的官方事件（如游戏互动、点赞排行）只需在 `DanmakuParser` 中增加对应 case；
4. **优秀的用户体验**：为主播提供可视化配置界面，与 TTS 设置保持一致的操作体验。
