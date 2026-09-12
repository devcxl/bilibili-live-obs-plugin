#include "danmaku-ws.h"
#include "danmaku/danmaku-packet.h"
#include "danmaku/danmaku-codec.h"
#include "danmaku/danmaku-parser.h"
#include "danmaku/open-live-client.h"

#include <obs-module.h>
#include <QtEndian>
#include <nlohmann/json.hpp>
#include <chrono>
#include <QMetaObject>

using json = nlohmann::json;

DanmakuWebSocket::DanmakuWebSocket(QObject *parent)
    : QObject(parent)
{
    ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setSingleShot(true);
    open_heartbeat_timer_ = new QTimer(this);
    open_heartbeat_timer_->setSingleShot(true);
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);

    connect(ws_, &QWebSocket::connected, this, &DanmakuWebSocket::on_ws_connected);
    connect(ws_, &QWebSocket::disconnected, this, &DanmakuWebSocket::on_ws_disconnected);
    connect(ws_, &QWebSocket::binaryMessageReceived,
            this, &DanmakuWebSocket::on_ws_binary_message);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(ws_, &QWebSocket::errorOccurred, this, &DanmakuWebSocket::on_ws_error);
#else
    connect(ws_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &DanmakuWebSocket::on_ws_error);
#endif
    connect(ws_, &QWebSocket::sslErrors, this, &DanmakuWebSocket::on_ws_ssl_errors);
    connect(heartbeat_timer_, &QTimer::timeout, this, &DanmakuWebSocket::send_heartbeat);
    connect(open_heartbeat_timer_, &QTimer::timeout, this, &DanmakuWebSocket::send_open_http_heartbeat);
    connect(reconnect_timer_, &QTimer::timeout, this, &DanmakuWebSocket::attempt_reconnect);
}

DanmakuWebSocket::~DanmakuWebSocket()
{
    disconnect_from_room();
    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }
}

void DanmakuWebSocket::set_api(BilibiliApi *api) { api_ = api; }

void DanmakuWebSocket::set_config(ConfigManager *cfg) { cfg_ = cfg; }

bool DanmakuWebSocket::is_connected() const { return state_ == State::Connected; }

int DanmakuWebSocket::popularity() const { return popularity_; }

void DanmakuWebSocket::set_state(State state)
{
    if (state_ == state) return;
    state_ = state;
    blog(LOG_INFO, "[danmaku-open] state -> %s",
         state == State::Connected  ? "connected"
         : state == State::Connecting ? "connecting"
                                      : "closed");
    emit connection_state_changed(state_, popularity_);
}

void DanmakuWebSocket::connect_to_room(const std::string &room_id)
{
    // 已连接或已在连接流程中：仅更新房间号，不打断现有连接
    if (state_ != State::Closed) {
        if (!room_id.empty()) room_id_ = room_id;
        blog(LOG_INFO, "[danmaku-open] connect request ignored (state=%d)",
             static_cast<int>(state_));
        return;
    }

    if (!room_id.empty()) room_id_ = room_id;
    session_active_ = true;      // 用户意图：保持弹幕互动开启
    start_connect_attempt();
}

// 发起一次连接尝试：start_app + WSS 握手，失败由 attempt 结果决定是否退避重试
void DanmakuWebSocket::start_connect_attempt()
{
    uint64_t my_gen = ++connect_gen_;
    stop_heartbeat();
    attempt_in_flight_ = true;
    set_state(State::Connecting);

    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }

    if (!cfg_) {
        attempt_in_flight_ = false;
        set_state(State::Closed);
        return;
    }

    blog(LOG_INFO, "[danmaku-open] initiating Open Live official start_app");
    fetch_thread_ = std::thread([this, my_gen]() {
        connect_async(my_gen);
    });
}

// 异步结束官方项目会话（不阻塞 UI，失败仅记日志）
void DanmakuWebSocket::release_open_project()
{
    if (open_game_id_.empty() || !cfg_) {
        open_game_id_.clear();
        open_auth_body_.clear();
        return;
    }

    std::string gid = open_game_id_;
    int64_t aid = cfg_->danmaku.open_live_app_id;
    std::string ak = cfg_->danmaku.open_live_access_key;
    std::string sk = cfg_->danmaku.open_live_secret;

    open_game_id_.clear();
    open_auth_body_.clear();

    std::thread([aid, gid, ak, sk]() {
        danmaku::OpenLiveClient::end_app(aid, gid, ak, sk);
    }).detach();
}

void DanmakuWebSocket::connect_async(uint64_t gen)
{
    auto res = danmaku::OpenLiveClient::start_app(
        cfg_->danmaku.open_live_app_id,
        cfg_->danmaku.open_live_access_key,
        cfg_->danmaku.open_live_secret,
        cfg_->danmaku.open_live_code
    );

    QMetaObject::invokeMethod(this, [this, gen, res]() {
        if (gen != connect_gen_.load() || !session_active_) {
            return;   // 已被新的连接尝试或手动关闭取代
        }

        attempt_in_flight_ = false;   // 本次尝试已得出结论

        if (!res.ok) {
            blog(LOG_WARNING, "[danmaku-open] start_app failed: %s (code=%d)",
                 res.msg.c_str(), res.code);
            // 参数不完整属于配置错误，重试无意义，直接回到已关闭
            if (res.msg.find("参数不完整") != std::string::npos) {
                session_active_ = false;
                set_state(State::Closed);
                return;
            }
            // 7001 为官方冷却期，退避起点放大到 5 秒，避免加剧限流
            schedule_reconnect(res.code == 7001 ? 5000 : RECONNECT_BASE_DELAY_MS);
            return;
        }

        open_game_id_ = res.game_id;
        open_auth_body_ = res.auth_body;

        if (res.wss_links.empty()) {
            blog(LOG_WARNING, "[danmaku-open] no wss_link returned from official open platform");
            release_open_project();
            schedule_reconnect(3000);
            return;
        }

        QString wss_url = QString::fromStdString(res.wss_links[0]);
        blog(LOG_INFO, "[danmaku-open] connecting to official websocket %s (game_id=%s)",
             wss_url.toUtf8().constData(), open_game_id_.c_str());

        // 启动官方 HTTP 项目心跳（每 20 秒发送一次）
        open_heartbeat_timer_->start(20000);

        ws_->open(QUrl(wss_url));
    }, Qt::QueuedConnection);
}

void DanmakuWebSocket::disconnect_from_room()
{
    session_active_ = false;      // 用户主动关闭：不再自动重连
    attempt_in_flight_ = false;
    ++connect_gen_;

    stop_heartbeat();
    stop_reconnect();
    open_heartbeat_timer_->stop();

    release_open_project();

    if (ws_->state() != QAbstractSocket::UnconnectedState) {
        ws_->abort();
    }

    popularity_ = 0;
    room_id_.clear();
    seq_ = 1;
    reconnect_attempts_ = 0;

    set_state(State::Closed);
}

void DanmakuWebSocket::on_ws_connected()
{
    blog(LOG_INFO, "[danmaku-open] official websocket connected, sending auth");
    send_auth_packet();
}

void DanmakuWebSocket::on_ws_disconnected()
{
    stop_heartbeat();
    popularity_ = 0;

    blog(LOG_INFO, "[danmaku-open] websocket disconnected (session_active=%d)",
         session_active_ ? 1 : 0);

    // 主动关闭：状态已由 disconnect_from_room() 收尾
    if (!session_active_) {
        set_state(State::Closed);
        return;
    }

    // 网络抖动或对端主动断开：自动重连（指数退避）
    attempt_in_flight_ = false;
    schedule_reconnect(RECONNECT_BASE_DELAY_MS);
}

void DanmakuWebSocket::on_ws_error(QAbstractSocket::SocketError error)
{
    blog(LOG_WARNING, "[danmaku-open] websocket error: %s (code=%d)",
         ws_->errorString().toUtf8().constData(), static_cast<int>(error));
}

void DanmakuWebSocket::on_ws_ssl_errors(const QList<QSslError> &errors)
{
    for (const auto &e : errors) {
        blog(LOG_WARNING, "[danmaku-open] ssl error: %s", e.errorString().toUtf8().constData());
    }
}

void DanmakuWebSocket::send_auth_packet()
{
    if (open_auth_body_.empty()) {
        blog(LOG_WARNING, "[danmaku-open] auth_body empty, aborting auth");
        return;
    }

    blog(LOG_INFO, "[danmaku-open] sending official auth packet");
    QByteArray body = QByteArray::fromStdString(open_auth_body_);
    QByteArray packet = danmaku::DanmakuCodec::encode_packet(
        danmaku::OpCode::Auth,
        danmaku::ProtoVer::Normal,
        body,
        seq_++
    );
    ws_->sendBinaryMessage(packet);
}

void DanmakuWebSocket::on_ws_binary_message(const QByteArray &data)
{
    auto packets = danmaku::DanmakuCodec::decode_packets(data);

    for (const auto &pkt : packets) {
        if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::AuthReply)) {
            auto j = json::parse(pkt.body.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.value("code", -1) == 0) {
                authenticated_ = true;
                blog(LOG_INFO, "[danmaku-open] auth success! listening for official live events");
                start_heartbeat();
                reconnect_attempts_ = 0;   // 连接稳定，重置退避
                set_state(State::Connected);
            } else {
                blog(LOG_WARNING, "[danmaku-open] auth failed: %s", pkt.body.toStdString().c_str());
                ws_->close();
            }
        } else if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::HeartbeatReply)) {
            if (pkt.body.size() >= 4) {
                popularity_ = static_cast<int>(
                    qFromBigEndian<uint32_t>(pkt.body.constData()));
                emit connection_state_changed(State::Connected, popularity_);
            }
        } else if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::Message)) {
            std::string body_str = pkt.body.toStdString();
            auto event = danmaku::DanmakuParser::parse(body_str);
            if (event) {
                switch (event->type) {
                case danmaku::EventType::Danmaku:
                    emit danmaku_received(event->danmaku);
                    break;
                case danmaku::EventType::Gift:
                    emit gift_received(event->gift);
                    break;
                case danmaku::EventType::SuperChat:
                    emit super_chat_received(event->super_chat);
                    break;
                case danmaku::EventType::Guard:
                    blog(LOG_INFO, "[danmaku-open] 捕获大航海事件: user=%s level=%d num=%d",
                         event->guard.username.c_str(), event->guard.guard_level, event->guard.guard_num);
                    emit guard_received(event->guard);
                    break;
                case danmaku::EventType::Like:
                    blog(LOG_INFO, "[danmaku-open] 捕获点赞事件: user=%s count=%lld",
                         event->like.username.c_str(), static_cast<long long>(event->like.like_count));
                    emit like_received(event->like);
                    break;
                case danmaku::EventType::Entry:
                    blog(LOG_INFO, "[danmaku-open] 捕获进房事件: user=%s guard=%d",
                         event->entry.username.c_str(), event->entry.guard_level);
                    emit entry_received(event->entry);
                    break;
                default:
                    break;
                }
            } else {
                // 输出未被 parser 拦截的原始 CMD
                auto j = json::parse(body_str, nullptr, false);
                if (!j.is_discarded() && j.contains("cmd")) {
                    blog(LOG_INFO, "[danmaku-open] 收到未处理官方CMD: %s", j.value("cmd", "").c_str());
                }
            }
        }
    }
}

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

    QByteArray packet = danmaku::DanmakuCodec::encode_packet(
        danmaku::OpCode::Heartbeat,
        danmaku::ProtoVer::Popularity,
        QByteArray(),
        seq_++
    );

    ws_->sendBinaryMessage(packet);
    start_heartbeat();
}

void DanmakuWebSocket::send_open_http_heartbeat()
{
    if (!session_active_ || open_game_id_.empty() || !cfg_) return;

    std::string gid = open_game_id_;
    std::string ak = cfg_->danmaku.open_live_access_key;
    std::string sk = cfg_->danmaku.open_live_secret;

    std::thread([this, gid, ak, sk]() {
        danmaku::OpenLiveClient::send_heartbeat(gid, ak, sk);
    }).detach();

    open_heartbeat_timer_->start(20000);
}

// 指数退避：2s → 4s → 8s → 16s → 30s 封顶
void DanmakuWebSocket::schedule_reconnect(int base_delay_ms)
{
    if (!session_active_) return;

    int delay = base_delay_ms * (1 << std::min(reconnect_attempts_, 4));
    delay = std::min(delay, RECONNECT_MAX_DELAY_MS);
    reconnect_attempts_++;

    blog(LOG_INFO, "[danmaku-open] scheduling reconnect in %d ms (attempt %d)",
         delay, reconnect_attempts_);
    set_state(State::Connecting);
    reconnect_timer_->start(delay);
}

void DanmakuWebSocket::stop_reconnect()
{
    reconnect_timer_->stop();
}

void DanmakuWebSocket::attempt_reconnect()
{
    if (!session_active_) return;
    // 已有连接尝试在进行或已建立连接：本轮交由该连接的结果继续驱动
    if (attempt_in_flight_ || state_ == State::Connected) return;
    start_connect_attempt();
}

