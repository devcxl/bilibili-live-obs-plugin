# 技术方案：实时弹幕展示功能

> **部分被取代**：本方案描述的是早期 Web 端协议实现。重连策略已由 [ADR-006](../adr/2026-09-13-danmaku-connection-state-machine.md) 更新为三态状态机 + 指数退避（仅手动关闭后不重连）；协议层已按 [ADR-004](../adr/2026-09-02-open-live-protocol-refactoring.md) 重构为 Open Live 官方长链。以下内容为历史记录。

## 概述

基于 `docs/research/bilibili-live-danmaku-websocket.md` 的协议调研，在 OBS 插件的推流线路 UI 下方添加实时弹幕展示区域。核心流程：HTTP 获取 Token → QWebSocket WSS 连接 → 认证 → 心跳维持 → 解析二进制消息 → Brotli 解压 → 弹幕/礼物/SC 类型分发 → UI 展示。

---

## 技术栈

沿用项目现有技术栈，新增 1 个系统依赖：

| 组件 | 选择 | 备注 |
|------|------|------|
| WebSocket 客户端 | Qt6 QWebSocket | `Qt6::Network` 已引入，无需新依赖 |
| 压缩解压 | libbrotli-dev | 通过 pkg-config 查找，protover=3 必需 |
| 二进制包头 | 手写大端序 | `arpa/inet.h` (`htonl`/`ntohl`) 或手动位移 |
| 弹幕缓存 | `std::vector` 环形缓冲区 | 固定预分配，无动态内存 |
| 心跳 | QTimer | 30 秒间隔 |
| 重连 | QTimer + 指数退避 | 独立定时器 |

---

## 架构设计

### 新模块

```
src/
├── danmaku-ws.h / danmaku-ws.cpp         # DanmakuWebSocket — WebSocket 客户端核心
├── danmaku-display.h / danmaku-display.cpp # DanmakuDisplay — QWidget 弹幕展示面板
```

### 类图与职责

```
┌──────────────────────┐      owns       ┌─────────────────────┐
│   plugin-main.cpp    │ ───────────────→│  DanmakuWebSocket   │
│  (创建 + 绑定)       │                 │                     │
└──────┬───────────────┘                 │  - ws_ (QWebSocket) │
       │ 传递指针到 BiliDock              │  - heartbeat_timer_ │
       │                                 │  - reconnect_timer_ │
┌──────▼───────────────┐                 │  - message_cache_   │
│     BiliDock         │                 │  - seq_             │
│                     │   signals/slots  │  - room_id_         │
│  + danmaku_display_  │◄────────────────│                     │
│  + danmaku_ws_       │                 └─────────────────────┘
└──────────────────────┘                          │
       │                                          │ HTTP
       │  owns                                    ▼
┌──────▼───────────────┐                 ┌─────────────────────┐
│   DanmakuDisplay     │                 │   BilibiliApi       │
│   (QWidget)          │                 │  + get_danmu_info() │
│                      │                 │  + get_buvid3()     │
│  - QListWidget       │                 │  + sign_wbi()       │
│  - popularity_label_ │                 └─────────────────────┘
└──────────────────────┘
```

### 数据流（端到端）

```
LiveService::start_live() → state_->is_live = true
    │
    ▼
BiliDock::do_start_live() 成功后:
    │
    ▼
DanmakuWebSocket::connect_to_room(room_id)
    │
    ├──① BilibiliApi::get_danmu_info(room_id)  → HTTP GET → {token, host_list}
    │       ├── 需要 WBI 签名 (w_rid + wts)
    │       └── 需要 buvid3 cookie（首次调用 get_buvid3()，缓存复用）
    │
    ├──② BilibiliApi::get_buvid3()             → 获取/刷新 buvid3
    │
    ├──③ QWebSocket::open("wss://{host}/sub")
    │
    ├──④ on_ws_connected() → send_auth_packet()
    │       构造认证包: 16字节头(protover=1, op=7, seq=1) + JSON body {uid, roomid, protover=3, key=token}
    │
    ├──⑤ on_ws_binary_message() → parse_packet_loop()
    │       ├── OP=8, protover=1 → 认证结果 ({"code":0} 表示成功)
    │       ├── OP=5, protover=0 → 未压缩业务消息 → dispatch(cmd)
    │       ├── OP=5, protover=3 → Brotli 解压 → 递归 parse_packet_loop()
    │       └── OP=3            → 心跳应答 → 读取人气值 (Int32 BE)
    │
    ├──⑥ 认证成功 → start heartbeat_timer_ (30s)
    │       heartbeat 包: 16字节头(protover=1, op=2, seq=seq_++, body=空)
    │
    └──⑦ 每条消息 → emit danmaku_received / gift_received / super_chat_received
                    → DanmakuDisplay::append_message() → QListWidget 追加
```

---

## 接口设计

### BilibiliApi 新增方法

```cpp
// bilibili-api.h
class BilibiliApi {
public:
    // 新增：获取弹幕 WebSocket 连接参数
    ApiResult get_danmu_info(const std::string &room_id);
};
```

**HTTP 调用**：`GET https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?id={room_id}&type=0`

**WBI 签名**：params `{"id": room_id, "type": 0}` → 调用 `sign_wbi()` 追加 `w_rid` + `wts`

**Cookie 需求**：调用前确保 `cookie_str_` 中包含 `buvid3`（若已登录则自动携带 SESSDATA 等）

**返回解析**：
```json
{
  "code": 0,
  "data": {
    "token": "Eac3Lm1JADz...",
    "host_list": [
      {"host": "tx-sh-live-comet-02.chat.bilibili.com", "wss_port": 443, "ws_port": 2244},
      {"host": "broadcastlv.chat.bilibili.com", "wss_port": 443, "ws_port": 2244}
    ]
  }
}
```

### DanmakuWebSocket 类接口

```cpp
class DanmakuWebSocket : public QObject {
    Q_OBJECT
public:
    explicit DanmakuWebSocket(QObject *parent = nullptr);
    ~DanmakuWebSocket() override;

    // 依赖注入
    void set_api(BilibiliApi *api);

    // 生命周期
    void connect_to_room(const std::string &room_id);
    void disconnect_from_room();
    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] int popularity() const;

    // 消息缓存（供 UI 一次性读取历史）
    [[nodiscard]] const DanmakuMessage &cached_message(size_t index) const;
    [[nodiscard]] size_t cached_message_count() const;

signals:
    // 连接状态
    void connection_state_changed(bool connected, int popularity);

    // 消息推送（Q_ARG 为值类型，Qt 自动复制，线程安全）
    void danmaku_received(const DanmakuMessage &msg);
    void gift_received(const GiftMessage &msg);
    void super_chat_received(const SuperChatMessage &msg);

private slots:
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_binary_message(const QByteArray &data);
    void on_ws_error(QAbstractSocket::SocketError error);
    void on_ws_ssl_errors(const QList<QSslError> &errors);
    void send_heartbeat();
    void attempt_reconnect();

private:
    void send_auth_packet();
    void parse_packet_loop(const QByteArray &buffer);
    void process_message(const std::string &json_str);
    void start_heartbeat();
    void start_reconnect();
    void stop_reconnect();
    QByteArray brotli_decompress(const QByteArray &compressed);
    static QByteArray build_packet_header(uint32_t packet_len, uint16_t protover,
                                           uint32_t op, uint32_t seq);

    // ── 成员 ──
    QWebSocket *ws_;                // 拥有
    QTimer *heartbeat_timer_;       // 拥有，30s 间隔，单次触发模式 + restart
    QTimer *reconnect_timer_;       // 拥有，单次触发，指数退避
    BilibiliApi *api_ = nullptr;    // 不拥有

    // 连接参数
    std::string room_id_;
    std::string token_;
    std::string host_;              // 当前连接的 host
    int wss_port_ = 443;
    std::string buvid3_;            // 首次获取后缓存
    std::string uid_;               // "0" = 游客

    // 状态
    uint32_t seq_ = 1;
    int popularity_ = 0;
    bool authenticated_ = false;
    bool intentional_disconnect_ = false;

    // 重连
    int reconnect_attempts_ = 0;
    static constexpr int RECONNECT_BASE_DELAY_MS = 1000;
    static constexpr int RECONNECT_MAX_DELAY_MS = 30000;

    // 弹幕缓存（环形缓冲区，预分配固定大小）
    static constexpr size_t CACHE_SIZE = 1000;
    std::vector<DanmakuMessage> message_cache_;
    size_t cache_write_pos_ = 0;
    size_t cache_count_ = 0;
};
```

### 数据结构

```cpp
// 弹幕消息
struct DanmakuMessage {
    std::string cmd;           // "DANMU_MSG" 等
    std::string username;
    std::string uid;
    std::string message;       // 弹幕文本
    std::string fan_badge;     // 粉丝勋章名（可选）
    int fan_badge_level = 0;   // 粉丝勋章等级
    int guard_level = 0;       // 大航海等级：0=无，1=总督，2=提督，3=舰长
};

// 礼物消息
struct GiftMessage {
    std::string username;
    std::string gift_name;
    int num = 0;               // 数量
    int combo_num = 0;         // 连送次数（combo）
    std::string action;        // "赠送" / "续费"
};

// SC 醒目留言
struct SuperChatMessage {
    std::string username;
    std::string message;
    int price = 0;             // 金额（人民币，单位：元）
};

// 注册到 Qt 元对象系统（支持跨线程信号/槽传递值类型）
Q_DECLARE_METATYPE(DanmakuMessage)
Q_DECLARE_METATYPE(GiftMessage)
Q_DECLARE_METATYPE(SuperChatMessage)
```

### DanmakuDisplay 控件接口

```cpp
class DanmakuDisplay : public QWidget {
    Q_OBJECT
public:
    explicit DanmakuDisplay(QWidget *parent = nullptr);

    void set_max_visible_items(int count);
    void set_popularity(int popularity);

public slots:
    void append_danmaku(const DanmakuMessage &msg);
    void append_gift(const GiftMessage &msg);
    void append_super_chat(const SuperChatMessage &msg);
    void clear_all();

private:
    void trim_items();           // 保持可见条目数受控

    QListWidget *list_widget_;   // 弹幕列表
    QLabel *popularity_label_;   // 人气值显示
    QLabel *status_label_;       // 连接状态（"已连接"/"已断开"）
    int max_visible_ = 500;      // UI 最多展示条目数
};
```

---

## CMakeLists.txt 变更

```cmake
# 新增依赖：libbrotli-dev
pkg_check_modules(LIBBROTLI REQUIRED libbrotlienc libbrotlidec)

# SOURCES 新增
set(SOURCES
  src/plugin-main.cpp
  src/config-manager.cpp
  src/bilibili-api.cpp
  src/auth-service.cpp
  src/bili-dock.cpp
  src/danmaku-ws.cpp          # 新增
  src/danmaku-display.cpp     # 新增
)

set(HEADERS
  src/config-manager.h
  src/bilibili-api.h
  src/auth-service.h
  src/bili-dock.h
  src/danmaku-ws.h            # 新增
  src/danmaku-display.h       # 新增
)

# target_include_directories 新增
target_include_directories(bili-live-obs PRIVATE
  ${LIBBROTLI_INCLUDE_DIRS}     # 新增
)

# target_link_libraries 新增
target_link_libraries(bili-live-obs PRIVATE
  ${LIBBROTLI_LIBRARIES}        # 新增
)

# CPACK 依赖声明新增
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "... libbrotli1 ...")         # 添加 libbrotli1 运行时依赖
```

---

## plugin-main.cpp 变更

```cpp
// 新增全局指针
static DanmakuWebSocket *s_danmaku_ws = nullptr;

// init_services() 中新增
s_danmaku_ws = new DanmakuWebSocket();
s_danmaku_ws->set_api(s_api);

// destroy_services() 中新增
delete s_danmaku_ws; s_danmaku_ws = nullptr;

// dock_load() 中新增
s_dock->set_danmaku_ws(s_danmaku_ws);
```

**BiliDock 新增方法**：`void set_danmaku_ws(DanmakuWebSocket *ws);`

---

## BiliDock 集成要点

### UI 布局（init_ui 尾部新增）

在 `main->addWidget(stream_route_group_)` 之后、`status_bar_` 之前插入：

```cpp
// ── Danmaku display ──
danmaku_display_ = new DanmakuDisplay();
danmaku_display_->hide();
main->addWidget(danmaku_display_);
```

### 开播/停播联动

在 `do_start_live()` 成功分支（`bili_live_started_ = true`）之后：

```cpp
if (danmaku_ws_ && !state_->room_id.empty()) {
    danmaku_ws_->connect_to_room(state_->room_id);
    danmaku_display_->show();
}
```

在 `do_stop_live()` 成功分支：

```cpp
if (danmaku_ws_) {
    danmaku_ws_->disconnect_from_room();
    danmaku_display_->hide();
}
```

### 信号绑定（set_danmaku_ws 中）

```cpp
void BiliDock::set_danmaku_ws(DanmakuWebSocket *ws) {
    danmaku_ws_ = ws;
    connect(ws, &DanmakuWebSocket::danmaku_received,
            danmaku_display_, &DanmakuDisplay::append_danmaku);
    connect(ws, &DanmakuWebSocket::gift_received,
            danmaku_display_, &DanmakuDisplay::append_gift);
    connect(ws, &DanmakuWebSocket::super_chat_received,
            danmaku_display_, &DanmakuDisplay::append_super_chat);
    connect(ws, &DanmakuWebSocket::connection_state_changed,
            this, [this](bool connected, int popularity) {
        danmaku_display_->set_popularity(popularity);
    });
}
```

---

## 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| WebSocket 库 | QWebSocket (Qt6) | 零新依赖，项目已有 Qt6::Network，原生 WSS 支持 |
| 压缩协议 | Brotli (protover=3) | B站实际使用，3 个独立开源项目一致推荐 |
| 弹幕缓存 | `std::vector` 环形缓冲区，预分配 1000 | 固定内存，无分配开销，OBS 插件不宜占用过多资源 |
| 重连策略 | 指数退避（1s→2s→4s→...→max 30s） | 防止风控触发，参考主流开源实现 |
| buvid3 获取 | 首次 `getDanmuInfo` 调用前 `get_buvid3()`，内存缓存 | 避免重复 HTTP 调用，buvid3 在 session 内稳定 |
| 消息分发 | Qt 信号/槽 | 线程安全（Q_ARG 值类型），解耦 WS 层与 UI 层 |
| 可见条目数 | 500（最多 1000 条缓存） | 避免 QListWidget 渲染性能下降 |
| protover 处理 | 连接时声明 protover=3，服务器可选回退到 protover=0 | 兼顾压缩效率与兼容性 |
| SSL/TLS 错误 | 记录日志但不中断 | OBS 插件在可信网络环境运行，WSS 证书由 CA 验证 |

---

## 消息类型解析映射

```cpp
// 从 JSON body 的 "cmd" 字段分发
void DanmakuWebSocket::process_message(const std::string &json_str) {
    auto j = json::parse(json_str);
    std::string cmd = j.value("cmd", "");

    if (cmd == "DANMU_MSG") {
        // info[1] = 弹幕文本, info[2][1] = 用户名, info[2][0] = UID
        // info[3][1] = 粉丝勋章名, info[3][0] = 粉丝勋章等级
        // info[3][10] = guard_level（大航海等级：0=无，1=总督，2=提督，3=舰长）
        DanmakuMessage msg;
        msg.cmd = cmd;
        msg.message = j["info"][1].get<std::string>();
        msg.username = j["info"][2][1].get<std::string>();
        msg.uid = std::to_string(j["info"][2][0].get<int64_t>());
        if (!j["info"][3].is_null() && j["info"][3].is_array() && j["info"][3].size() >= 2) {
            msg.fan_badge = j["info"][3][1].get<std::string>();
            msg.fan_badge_level = j["info"][3][0].get<int>();
        }
        emit danmaku_received(msg);

    } else if (cmd == "SEND_GIFT") {
        auto &d = j["data"];
        GiftMessage gift;
        gift.username = d.value("uname", "");
        gift.gift_name = d.value("giftName", "");
        gift.num = d.value("num", 0);
        gift.combo_num = d.value("combo_num", 0);
        gift.action = d.value("action", "");
        emit gift_received(gift);

    } else if (cmd == "SUPER_CHAT_MESSAGE") {
        auto &d = j["data"];
        SuperChatMessage sc;
        sc.username = d.value("uname", "");
        sc.message = d.value("message", "");
        sc.price = d.value("price", 0);
        emit super_chat_received(sc);

    }
    // 未知 cmd → 静默忽略（协议容错）
}
```

---

## 错误处理与隔离

| 错误场景 | 处理策略 | 影响范围 |
|----------|----------|----------|
| `getDanmuInfo` HTTP 失败 | `emit connection_state_changed(false, 0)`，启动重连 | 仅弹幕功能 |
| WebSocket 连接失败 | `on_ws_error()` → 启动重连 | 仅弹幕功能 |
| 认证失败（OP=8 code≠0） | `emit connection_state_changed(false, 0)`，不重连 | 仅弹幕功能 |
| Brotli 解压失败 | 记录日志，丢弃当前包，继续接收 | 单个消息包丢失 |
| JSON 解析失败 | 记录日志，丢弃当前消息 | 单个消息丢失 |
| 心跳超时（70s 无响应） | `disconnected()` → 启动重连 | 弹幕连接 |
| 30s 内未认证 | `ws_->close()` → 不重连 | 弹幕连接 |

**核心原则**：弹幕 WebSocket 的任何失败都不应影响 `BilibiliApi` 的 HTTP 调用、`LiveService` 的开播/停播流程、OBS 推流配置。

---

## 假设与待确认

| 假设 | 依据 | 风险 |
|------|------|------|
| QWebSocket 可直连 WSS 443 端口 | Qt6 Network 基于 OpenSSL，支持标准 WSS | 低（已验证 Qt6 官方文档） |
| libbrotli 在主流发行版可用 | Debian/Ubuntu: `apt install libbrotli-dev`；AUR: `brotli` | 低 |
| buvid3 在游客模式下也可获取 | `get_buvid3()` 无需登录态 | 中（风控可能变化） |
| token 有效期内不需要刷新 | 连接建立后 token 仅用于认证，不会在中途验证 | 低 |
| 弹幕频率 ≤ 50 条/秒 | 即使热门直播间，B站服务器有频率限制 | 低 |
| QListWidget 500 条目性能 OK | 实测同规模 Qt Widget 应用流畅 | 低 |
