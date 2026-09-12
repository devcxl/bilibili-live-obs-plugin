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

bool DanmakuWebSocket::is_connected() const { return authenticated_; }

int DanmakuWebSocket::popularity() const { return popularity_; }

void DanmakuWebSocket::connect_to_room(const std::string &room_id)
{
    // 如果当前已经建立连接且已鉴权成功，直接返回，避免破坏正常长连接
    if (authenticated_) {
        blog(LOG_INFO, "[danmaku-open] already connected, ignoring connect request");
        return;
    }
    // 如果正在连接中，避免并发重复发起 start_app 请求导致触发 7001 冷却期
    if (is_connecting_) {
        blog(LOG_INFO, "[danmaku-open] connection in progress, ignoring duplicate connect request");
        return;
    }

    is_connecting_ = true;
    session_active_ = true;
    room_id_ = room_id;
    uint64_t my_gen = ++connect_gen_;

    stop_heartbeat();

    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }

    blog(LOG_INFO, "[danmaku-open] initiating Open Live official start_app");
    fetch_thread_ = std::thread([this, my_gen]() {
        connect_async(my_gen);
    });
}

void DanmakuWebSocket::connect_async(uint64_t gen)
{
    if (!cfg_) {
        is_connecting_ = false;
        QMetaObject::invokeMethod(this, [this]() {
            emit connection_state_changed(false, 0);
        }, Qt::QueuedConnection);
        return;
    }

    auto res = danmaku::OpenLiveClient::start_app(
        cfg_->danmaku.open_live_app_id,
        cfg_->danmaku.open_live_access_key,
        cfg_->danmaku.open_live_secret,
        cfg_->danmaku.open_live_code
    );

    QMetaObject::invokeMethod(this, [this, gen, res]() {
        if (gen != connect_gen_.load() || !session_active_) {
            is_connecting_ = false;
            return;
        }

        if (!res.ok) {
            is_connecting_ = false;
            blog(LOG_WARNING, "[danmaku-open] start_app failed: %s (code=%d)",
                 res.msg.c_str(), res.code);
            // 不自动重试：start_app 失败即退出本次弹幕互动，等待用户手动重连
            end_session();
            return;
        }

        open_game_id_ = res.game_id;
        open_auth_body_ = res.auth_body;

        if (res.wss_links.empty()) {
            is_connecting_ = false;
            blog(LOG_WARNING, "[danmaku-open] no wss_link returned from official open platform");
            end_session();
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
    session_active_ = false;
    is_connecting_ = false;
    ++connect_gen_;

    stop_heartbeat();
    open_heartbeat_timer_->stop();

    // 优雅关闭 Open Live 官方项目
    if (!open_game_id_.empty() && cfg_) {
        std::string gid = open_game_id_;
        int64_t aid = cfg_->danmaku.open_live_app_id;
        std::string ak = cfg_->danmaku.open_live_access_key;
        std::string sk = cfg_->danmaku.open_live_secret;
        std::thread([aid, gid, ak, sk]() {
            danmaku::OpenLiveClient::end_app(aid, gid, ak, sk);
        }).detach();
        open_game_id_.clear();
        open_auth_body_.clear();
    }

    if (ws_->state() != QAbstractSocket::UnconnectedState) {
        ws_->abort();
    }

    authenticated_ = false;
    popularity_ = 0;
    room_id_.clear();
    seq_ = 1;

    emit connection_state_changed(false, 0);
}

// 连接被对端关闭/发生错误后终止本次弹幕互动：停心跳、结束官方项目，
// 不自动重连，等待用户点击「重连」重新发起。
void DanmakuWebSocket::end_session()
{
    blog(LOG_INFO, "[danmaku-open] session ended, awaiting manual reconnect");
    disconnect_from_room();
}

void DanmakuWebSocket::on_ws_connected()
{
    blog(LOG_INFO, "[danmaku-open] official websocket connected, sending auth");
    send_auth_packet();
}

void DanmakuWebSocket::on_ws_disconnected()
{
    bool was_active = session_active_;
    authenticated_ = false;
    is_connecting_ = false;
    stop_heartbeat();

    blog(LOG_INFO, "[danmaku-open] websocket disconnected (session_active=%d)", was_active);

    if (!was_active) return;   // 主动断开：状态已由 disconnect_from_room() 收尾

    // 连接关闭即退出本次弹幕互动，不自动重连
    end_session();
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
                emit connection_state_changed(true, popularity_);
            } else {
                blog(LOG_WARNING, "[danmaku-open] auth failed: %s", pkt.body.toStdString().c_str());
                ws_->close();
            }
        } else if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::HeartbeatReply)) {
            if (pkt.body.size() >= 4) {
                popularity_ = static_cast<int>(
                    qFromBigEndian<uint32_t>(pkt.body.constData()));
                emit connection_state_changed(true, popularity_);
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


