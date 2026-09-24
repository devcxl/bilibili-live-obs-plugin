---
name: "task-03-danmaku-ws"
depends_on: ["task-01-cmake-and-structs", "task-02-api-get-danmu-info"]
labels: ["backend"]
worktree_root: ".worktree/task-03-danmaku-ws/"
test_commands: []
verify_commands:
  - "cmake --build build --target bilibili-live-obs 2>&1 | tail -5"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "danmaku-ws.h 定义 DanmakuWebSocket 类，含所有信号、槽、成员变量"
    verification_type: manual
  - criteria: "danmaku-ws.cpp 实现 connect_to_room / disconnect_from_room 生命周期管理"
    verification_type: manual
  - criteria: "实现 WebSocket 连接 → 认证包发送 → 心跳维持（30s）完整流程"
    verification_type: manual
  - criteria: "实现二进制包解析（16 字节头 + protover=0/3 分支 + Brotli 解压）"
    verification_type: manual
  - criteria: "实现消息类型分发（DANMU_MSG / SEND_GIFT / SUPER_CHAT_MESSAGE）"
    verification_type: manual
  - criteria: "实现指数退避重连（1s→2s→4s→...→max 30s）"
    verification_type: manual
  - criteria: "编译通过（可能因 danmaku-display.cpp 缺实现报 undefined symbol）"
    verification_type: lint
---

# Task 3: DanmakuWebSocket 客户端完整实现

## 目标概述

实现 `DanmakuWebSocket` 类，完成 B站弹幕 WebSocket 客户端的全部核心逻辑：获取连接参数 → WSS 连接 → 认证 → 心跳维持 → 二进制包解析 → Brotli 解压 → 消息类型分发 → 重连。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `src/danmaku-ws.h` | 补充 DanmakuWebSocket 类完整定义（Task 1 已定义结构体） | ~80 行 |
| `src/danmaku-ws.cpp`（新建） | 完整实现（连接/认证/心跳/重连/解析/解压/分发） | ~250 行 |

## 实现步骤

### 步骤 1：danmaku-ws.h — 补充类定义

在 Task 1 创建的数据结构之后，追加以下内容：

```cpp
#include <vector>
#include <QObject>
#include <QTimer>
#include <QWebSocket>
#include <QAbstractSocket>
#include "bilibili-api.h"

// data structures defined above...

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

    // 消息缓存
    [[nodiscard]] const DanmakuMessage &cached_message(size_t index) const;
    [[nodiscard]] size_t cached_message_count() const;

signals:
    void danmaku_received(const DanmakuMessage &msg);
    void gift_received(const GiftMessage &msg);
    void super_chat_received(const SuperChatMessage &msg);
    void connection_state_changed(bool connected, int popularity);

private slots:
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_binary_message(const QByteArray &data);
    void on_ws_error(QAbstractSocket::SocketError error);
    void on_ws_ssl_errors(const QList<QSslError> &errors);
    void send_heartbeat();
    void attempt_reconnect();

private:
    // 内部方法
    void send_auth_packet();
    void parse_packet_loop(const QByteArray &buffer);
    void process_message(const std::string &json_str);
    void start_heartbeat();
    void stop_heartbeat();
    void start_reconnect();
    void stop_reconnect();
    QByteArray brotli_decompress(const QByteArray &compressed);
    static QByteArray build_packet_header(uint32_t packet_len, uint16_t protover,
                                           uint32_t op, uint32_t seq);

    // 成员变量
    QWebSocket *ws_ = nullptr;
    QTimer *heartbeat_timer_ = nullptr;
    QTimer *reconnect_timer_ = nullptr;
    BilibiliApi *api_ = nullptr;

    std::string room_id_;
    std::string token_;
    std::string host_;
    int wss_port_ = 443;

    uint32_t seq_ = 1;
    int popularity_ = 0;
    bool authenticated_ = false;
    bool intentional_disconnect_ = false;

    int reconnect_attempts_ = 0;
    static constexpr int RECONNECT_BASE_DELAY_MS = 1000;
    static constexpr int RECONNECT_MAX_DELAY_MS = 30000;

    static constexpr size_t CACHE_SIZE = 1000;
    std::vector<DanmakuMessage> message_cache_;
    size_t cache_write_pos_ = 0;
    size_t cache_count_ = 0;
};
```

### 步骤 2：danmaku-ws.cpp — 构造函数/析构函数

```cpp
#include "danmaku-ws.h"
#include <obs-module.h>
#include <arpa/inet.h>
#include <brotli/decode.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

DanmakuWebSocket::DanmakuWebSocket(QObject *parent)
    : QObject(parent)
{
    ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setSingleShot(true);
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);

    message_cache_.resize(CACHE_SIZE);

    connect(ws_, &QWebSocket::connected, this, &DanmakuWebSocket::on_ws_connected);
    connect(ws_, &QWebSocket::disconnected, this, &DanmakuWebSocket::on_ws_disconnected);
    connect(ws_, &QWebSocket::binaryMessageReceived,
            this, &DanmakuWebSocket::on_ws_binary_message);
    connect(ws_, &QWebSocket::errorOccurred, this, &DanmakuWebSocket::on_ws_error);
    connect(ws_, &QWebSocket::sslErrors, this, &DanmakuWebSocket::on_ws_ssl_errors);
    connect(heartbeat_timer_, &QTimer::timeout, this, &DanmakuWebSocket::send_heartbeat);
    connect(reconnect_timer_, &QTimer::timeout, this, &DanmakuWebSocket::attempt_reconnect);
}

DanmakuWebSocket::~DanmakuWebSocket()
{
    disconnect_from_room();
}
```

### 步骤 3：danmaku-ws.cpp — 生命周期管理

```cpp
void DanmakuWebSocket::set_api(BilibiliApi *api) { api_ = api; }

bool DanmakuWebSocket::is_connected() const { return authenticated_; }

int DanmakuWebSocket::popularity() const { return popularity_; }

void DanmakuWebSocket::connect_to_room(const std::string &room_id)
{
    if (!api_) {
        blog(LOG_ERROR, "[danmaku] api not set");
        return;
    }

    disconnect_from_room();
    room_id_ = room_id;
    intentional_disconnect_ = false;
    reconnect_attempts_ = 0;

    ApiResult res = api_->get_danmu_info(room_id);
    if (!res.ok || res.code != 0 || !res.data.contains("data")) {
        blog(LOG_WARNING, "[danmaku] getDanmuInfo failed: code=%d msg=%s",
             res.code, res.msg.c_str());
        start_reconnect();
        return;
    }

    auto &data = res.data["data"];
    token_ = data.value("token", "");
    auto &hosts = data["host_list"];
    if (hosts.empty() || !hosts[0].contains("host")) {
        blog(LOG_WARNING, "[danmaku] host_list empty");
        start_reconnect();
        return;
    }

    host_ = hosts[0]["host"].get<std::string>();
    wss_port_ = hosts[0].value("wss_port", 443);

    QString wss_url = QString("wss://%1:%2/sub").arg(
        QString::fromStdString(host_)).arg(wss_port_);
    blog(LOG_INFO, "[danmaku] connecting to %s", wss_url.toStdString().c_str());
    ws_->open(QUrl(wss_url));
}

void DanmakuWebSocket::disconnect_from_room()
{
    intentional_disconnect_ = true;
    stop_heartbeat();
    stop_reconnect();
    authenticated_ = false;
    ws_->close();
    room_id_.clear();
}
```

### 步骤 4：danmaku-ws.cpp — WebSocket 事件处理

```cpp
void DanmakuWebSocket::on_ws_connected()
{
    blog(LOG_INFO, "[danmaku] ws connected, sending auth");
    seq_ = 1;
    send_auth_packet();
}

void DanmakuWebSocket::on_ws_disconnected()
{
    blog(LOG_INFO, "[danmaku] ws disconnected");
    authenticated_ = false;
    stop_heartbeat();
    emit connection_state_changed(false, 0);

    if (!intentional_disconnect_) {
        start_reconnect();
    }
}

void DanmakuWebSocket::on_ws_error(QAbstractSocket::SocketError error)
{
    blog(LOG_WARNING, "[danmaku] ws error: %d", static_cast<int>(error));
}

void DanmakuWebSocket::on_ws_ssl_errors(const QList<QSslError> &errors)
{
    for (const auto &e : errors) {
        blog(LOG_WARNING, "[danmaku] SSL error: %s",
             e.errorString().toStdString().c_str());
    }
    // 不中断连接（可信网络环境）
    ws_->ignoreSslErrors();
}
```

### 步骤 5：danmaku-ws.cpp — 认证包发送

```cpp
void DanmakuWebSocket::send_auth_packet()
{
    json auth_body = {
        {"uid", 0},
        {"roomid", std::stoll(room_id_)},
        {"protover", 3},
        {"platform", "web"},
        {"type", 2},
        {"key", token_}
    };

    std::string body_str = auth_body.dump();
    QByteArray header = build_packet_header(
        static_cast<uint32_t>(16 + body_str.size()),
        1,   // protover
        7,   // op = AUTH
        seq_++
    );

    QByteArray packet = header + QByteArray::fromStdString(body_str);
    ws_->sendBinaryMessage(packet);
}
```

### 步骤 6：danmaku-ws.cpp — 二进制包解析

```cpp
QByteArray DanmakuWebSocket::build_packet_header(
    uint32_t packet_len, uint16_t protover, uint32_t op, uint32_t seq)
{
    QByteArray header(16, 0);
    *reinterpret_cast<uint32_t*>(header.data()) = htonl(packet_len);
    *reinterpret_cast<uint16_t*>(header.data() + 4) = htons(protover);
    *reinterpret_cast<uint32_t*>(header.data() + 8) = htonl(op);
    *reinterpret_cast<uint32_t*>(header.data() + 12) = htonl(seq);
    return header;
}

void DanmakuWebSocket::on_ws_binary_message(const QByteArray &data)
{
    parse_packet_loop(data);
}

void DanmakuWebSocket::parse_packet_loop(const QByteArray &buffer)
{
    int offset = 0;
    while (offset + 16 <= buffer.size()) {
        uint32_t packet_len = ntohl(*reinterpret_cast<const uint32_t*>(
            buffer.constData() + offset));
        uint16_t protover   = ntohs(*reinterpret_cast<const uint16_t*>(
            buffer.constData() + offset + 4));
        uint32_t op         = ntohl(*reinterpret_cast<const uint32_t*>(
            buffer.constData() + offset + 8));
        // uint32_t seq field at offset+12, ignored

        if (packet_len < 16 || offset + packet_len > static_cast<uint32_t>(buffer.size())) {
            break;
        }

        QByteArray body = buffer.mid(offset + 16, packet_len - 16);

        if (op == 8) {
            // 认证结果
            auto j = json::parse(body.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.value("code", -1) == 0) {
                authenticated_ = true;
                blog(LOG_INFO, "[danmaku] auth success");
                start_heartbeat();
                stop_reconnect();
                reconnect_attempts_ = 0;
                emit connection_state_changed(true, popularity_);
            } else {
                blog(LOG_WARNING, "[danmaku] auth failed: %s",
                     body.toStdString().c_str());
                ws_->close();
            }
        } else if (op == 3) {
            // 心跳应答（人气值）
            if (body.size() >= 4) {
                popularity_ = static_cast<int>(
                    ntohl(*reinterpret_cast<const uint32_t*>(body.constData())));
            }
        } else if (op == 5) {
            // 业务消息
            if (protover == 0) {
                // 未压缩
                auto j = json::parse(body.toStdString(), nullptr, false);
                if (!j.is_discarded()) {
                    process_message(body.toStdString());
                }
            } else if (protover == 3) {
                // Brotli 压缩
                QByteArray decompressed = brotli_decompress(body);
                if (!decompressed.isEmpty()) {
                    parse_packet_loop(decompressed);
                } else {
                    blog(LOG_WARNING, "[danmaku] brotli decompress failed");
                }
            }
        }

        offset += packet_len;
    }
}
```

### 步骤 7：danmaku-ws.cpp — Brotli 解压

```cpp
#include <brotli/decode.h>

QByteArray DanmakuWebSocket::brotli_decompress(const QByteArray &compressed)
{
    BrotliDecoderState *state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) return {};

    std::vector<uint8_t> out_buffer(65536);
    size_t available_in = compressed.size();
    const uint8_t *next_in = reinterpret_cast<const uint8_t*>(compressed.constData());

    QByteArray result;
    BrotliDecoderResult rc;
    do {
        size_t available_out = out_buffer.size();
        uint8_t *next_out = out_buffer.data();
        rc = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                            &available_out, &next_out, nullptr);
        result.append(reinterpret_cast<const char*>(out_buffer.data()),
                      out_buffer.size() - available_out);
    } while (rc == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

    BrotliDecoderDestroyInstance(state);

    if (rc == BROTLI_DECODER_RESULT_SUCCESS) {
        return result;
    }
    return {};
}
```

### 步骤 8：danmaku-ws.cpp — 消息类型分发

```cpp
void DanmakuWebSocket::process_message(const std::string &json_str)
{
    auto j = json::parse(json_str, nullptr, false);
    if (j.is_discarded()) return;

    std::string cmd = j.value("cmd", "");

    if (cmd == "DANMU_MSG") {
        DanmakuMessage msg;
        msg.cmd = cmd;
        auto &info = j["info"];
        msg.message = info[1].get<std::string>();
        msg.username = info[2][1].get<std::string>();
        msg.uid = std::to_string(info[2][0].get<int64_t>());
        if (!info[3].is_null() && info[3].is_array() && info[3].size() >= 2) {
            msg.fan_badge = info[3][1].get<std::string>();
            msg.fan_badge_level = info[3][0].get<int>();
        }

        // 环形缓冲区写入
        message_cache_[cache_write_pos_] = msg;
        cache_write_pos_ = (cache_write_pos_ + 1) % CACHE_SIZE;
        if (cache_count_ < CACHE_SIZE) cache_count_++;

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
    // 未知 cmd 静默忽略
}
```

### 步骤 9：danmaku-ws.cpp — 心跳

```cpp
void DanmakuWebSocket::start_heartbeat()
{
    heartbeat_timer_->start(30000);
}

void DanmakuWebSocket::stop_heartbeat()
{
    heartbeat_timer_->stop();
}

void DanmakuWebSocket::send_heartbeat()
{
    if (!authenticated_) return;

    QByteArray header = build_packet_header(
        16,     // packet_len = header only, no body
        1,      // protover
        2,      // op = HEARTBEAT
        seq_++
    );
    ws_->sendBinaryMessage(header);
    heartbeat_timer_->start(30000);
}
```

### 步骤 10：danmaku-ws.cpp — 重连

```cpp
void DanmakuWebSocket::start_reconnect()
{
    if (intentional_disconnect_ || room_id_.empty()) return;

    int delay = RECONNECT_BASE_DELAY_MS * (1 << reconnect_attempts_);
    if (delay > RECONNECT_MAX_DELAY_MS) delay = RECONNECT_MAX_DELAY_MS;
    reconnect_attempts_++;

    blog(LOG_INFO, "[danmaku] reconnect attempt %d in %dms",
         reconnect_attempts_, delay);
    reconnect_timer_->start(delay);
}

void DanmakuWebSocket::stop_reconnect()
{
    reconnect_timer_->stop();
}

void DanmakuWebSocket::attempt_reconnect()
{
    if (!room_id_.empty()) {
        connect_to_room(room_id_);
    }
}
```

### 步骤 11：danmaku-ws.cpp — 缓存查询

```cpp
const DanmakuMessage &DanmakuWebSocket::cached_message(size_t index) const
{
    return message_cache_[index % CACHE_SIZE];
}

size_t DanmakuWebSocket::cached_message_count() const
{
    return cache_count_;
}
```

### 步骤 12：验证编译

```bash
cmake --build build --target bilibili-live-obs 2>&1 | tail -5
```

预期：编译通过（可能因 danmaku-display.cpp 缺实现报 undefined symbol，属预期）。

## 注意事项

1. **SSL 错误处理**：通过 `ws_->ignoreSslErrors()` 忽略（OBS 插件在可信网络环境运行）
2. **Brotli 流式解压**：使用 `BrotliDecoderDecompressStream` 而非一次性 `BrotliDecoderDecompress`，因为压缩数据可能很大（多弹幕合并）
3. **protover=3**：认证包声明 protover=3，服务器可能回退到 protover=0（未压缩）
4. **消息去重**：B站 WebSocket 不发送重复消息，无需客户端去重
5. **重连获取新 token**：`connect_to_room()` 每次都重新调用 `get_danmu_info()`，确保 token 有效

## 验收

参考 frontmatter `acceptance` 字段中的 7 条验收标准。
