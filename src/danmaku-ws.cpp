#include "danmaku-ws.h"
#include "danmaku/danmaku-codec.h"
#include "danmaku/danmaku-parser.h"

#include <obs-module.h>
#include <QtEndian>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <QUrl>

using json = nlohmann::json;

namespace {
// 绝对时限：QNAM 的 transferTimeout 只约束“无数据”的空闲时长，
// 慢速滴流仍可能长期不返回。reply 作为 context，销毁后定时器自动失效。
void abort_on_http_deadline(QNetworkReply *reply, int timeout_ms)
{
    QTimer::singleShot(timeout_ms, reply, [reply]() {
        if (!reply->isFinished()) {
            // 标记为超时（而非用户/生命周期取消），供心跳等回调区分处理
            reply->setProperty("danmaku_deadline_expired", true);
            reply->abort();
        }
    });
}
} // namespace

DanmakuWebSocket::DanmakuWebSocket(QObject *parent)
    : QObject(parent)
{
    nam_.setTransferTimeout(HTTP_TRANSFER_TIMEOUT_MS);

    ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setObjectName(QStringLiteral("heartbeat_timer"));
    heartbeat_timer_->setSingleShot(true);
    open_heartbeat_timer_ = new QTimer(this);
    open_heartbeat_timer_->setObjectName(QStringLiteral("open_heartbeat_timer"));
    open_heartbeat_timer_->setSingleShot(false);   // 固定 20s 节拍，不因 WSS 重连而重启
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setObjectName(QStringLiteral("reconnect_timer"));
    reconnect_timer_->setSingleShot(true);
    auth_timer_ = new QTimer(this);
    auth_timer_->setObjectName(QStringLiteral("auth_timer"));
    auth_timer_->setSingleShot(true);

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
    connect(auth_timer_, &QTimer::timeout, this, &DanmakuWebSocket::on_auth_timeout);
}

DanmakuWebSocket::~DanmakuWebSocket()
{
    shutdown_requested_ = true;
    disconnect_from_room();
    // 退出路径不等待网络：abort 会同步触发 finished，回调凭 shutdown_requested_
    // 放弃后续动作。被取消的 start/end 在服务端是否已生效无法从客户端确认。
    abort_reply(start_reply_);
    abort_reply(heartbeat_reply_);
    abort_all_end_replies();
    blog(LOG_INFO,
         "[danmaku-open] shutdown: 官方请求已取消，服务端是否已处理无法确认");
}

void DanmakuWebSocket::set_config(ConfigManager *cfg) { cfg_ = cfg; }

void DanmakuWebSocket::set_network_access_manager(QNetworkAccessManager *manager)
{
    nam_injected_ = manager;
}

QNetworkAccessManager *DanmakuWebSocket::network_manager()
{
    return nam_injected_ ? nam_injected_ : &nam_;
}

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
    auth_rejections_ = 0;        // 用户主动开启时给一次全新的鉴权重试预算
    start_connect_attempt();
}

// 发起一次连接尝试：异步 start_app，成功后建立 WSS 长连；
// 失败由 attempt 结果或 WSS 断开决定是否退避重试。
void DanmakuWebSocket::start_connect_attempt()
{
    if (shutdown_requested_) return;

    // 旧场次 end_app 未返回时创建新场次，可能被官方判定为「未正常 end」；
    // 等它有界结束（QNAM transfer timeout 兜底）后再发起。
    if (has_pending_end_app()) {
        blog(LOG_INFO, "[danmaku-open] 等待旧场次 end_app 完成后再 start_app");
        schedule_reconnect(END_PENDING_RETRY_MS, END_PENDING_RETRY_MS);
        return;
    }
    // 上一次 start_app 仍未返回（例如刚停止又立即启动）：等它返回，
    // 成功则补 end，失败则继续；避免并发创建两个官方场次。
    if (start_reply_) {
        blog(LOG_INFO, "[danmaku-open] 等待上一次 start_app 返回后再发起新场次");
        schedule_reconnect(END_PENDING_RETRY_MS, END_PENDING_RETRY_MS);
        return;
    }
    if (end_cooldown_active()) {
        blog(LOG_INFO, "[danmaku-open] end_app 曾失败，服务端可能仍保留旧场次，冷却后再 start_app");
        schedule_reconnect(COOLDOWN_BACKOFF_MS, COOLDOWN_BACKOFF_MS);
        return;
    }

    uint64_t my_gen = ++connect_gen_;
    stop_heartbeat();
    attempt_in_flight_ = true;
    set_state(State::Connecting);

    if (!cfg_) {
        attempt_in_flight_ = false;
        set_state(State::Closed);
        return;
    }

    attempt_creds_ = snapshot_credentials();
    if (!attempt_creds_.complete()) {
        blog(LOG_WARNING,
             "[danmaku-open] start_app 参数不完整（请在设置中填写 App ID, AccessKey, Secret 与主播身份码），停止弹幕互动");
        session_active_ = false;
        attempt_in_flight_ = false;
        set_state(State::Closed);
        return;
    }

    blog(LOG_INFO, "[danmaku-open] initiating Open Live official start_app");
    QNetworkReply *reply = danmaku::OpenLiveClient::post_start(*network_manager(), attempt_creds_);
    start_reply_ = reply;
    if (!reply) {
        attempt_in_flight_ = false;
        schedule_reconnect(RECONNECT_BASE_DELAY_MS);
        return;
    }
    // 凭据按请求快照捕获：迟到的旧 start 成功时用它补发 end_app，
    // 不会被后续尝试覆盖后的 attempt_creds_ 污染。
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, my_gen, creds = attempt_creds_]() {
                on_start_reply(reply, my_gen, creds);
            });
    abort_on_http_deadline(reply, HTTP_REQUEST_DEADLINE_MS);
}

void DanmakuWebSocket::on_start_reply(QNetworkReply *reply, uint64_t gen,
                                      const danmaku::OpenLiveCredentials &creds)
{
    if (start_reply_ == reply) start_reply_ = nullptr;

    const bool net_ok = (reply->error() == QNetworkReply::NoError);
    const QByteArray body = reply->readAll();
    if (!net_ok) {
        blog(LOG_WARNING, "[danmaku-open] start_app 请求失败: %s",
             reply->errorString().toUtf8().constData());
    }
    reply->deleteLater();

    if (shutdown_requested_) return;   // abort 后的回调用不了，服务端结果不可知

    const bool stale = (gen != connect_gen_.load()) || !session_active_;

    danmaku::OpenLiveStartResult res;
    if (net_ok) res = danmaku::OpenLiveClient::parse_start_response(body);

    // 已被新的连接尝试或手动关闭取代：成功场次必须补 end_app，
    // 否则官方会认为上一场未正常结束（影响道具与下一场）。
    if (stale) {
        if (net_ok && res.ok && !res.game_id.empty()) {
            blog(LOG_INFO,
                 "[danmaku-open] 已过期的 start_app 成功 (game_id=%s)，补发 best-effort end_app",
                 res.game_id.c_str());
            post_end_app(res.game_id, creds);
        }
        return;
    }

    attempt_in_flight_ = false;

    if (!net_ok) {
        schedule_reconnect(RECONNECT_BASE_DELAY_MS);
        return;
    }

    if (!res.ok) {
        blog(LOG_WARNING, "[danmaku-open] start_app failed: %s (code=%d)",
             res.msg.c_str(), res.code);
        // 7001 为疑似官方冷却期（未获官方文档证实）：起点与封顶对齐官方
        // “未正常 end 需间隔 1 分钟”，避免以秒级重试持续冲击冷却期
        if (res.code == 7001) {
            schedule_reconnect(COOLDOWN_BACKOFF_MS, COOLDOWN_BACKOFF_MS);
            return;
        }
        schedule_reconnect(RECONNECT_BASE_DELAY_MS);
        return;
    }

    open_game_id_ = res.game_id;
    open_auth_body_ = res.auth_body;
    session_creds_ = creds;
    wss_links_ = res.wss_links;
    reset_link_failover();
    heartbeat_failures_ = 0;

    blog(LOG_INFO, "[danmaku-open] start_app ok: game_id=%s, wss_link 数量=%zu",
         open_game_id_.c_str(), wss_links_.size());

    if (wss_links_.empty()) {
        blog(LOG_WARNING, "[danmaku-open] no wss_link returned from official open platform");
        release_open_project();
        schedule_reconnect(3000);
        return;
    }

    // 项目心跳按「官方场次」计时：WSS 各次重连不重启这一节拍。
    open_heartbeat_timer_->start(OPEN_HEARTBEAT_INTERVAL_MS);
    open_ws_on_chosen_link();
}

void DanmakuWebSocket::disconnect_from_room()
{
    session_active_ = false;      // 用户主动关闭：不再自动重连
    authenticated_ = false;       // 避免下一次未鉴权尝试被旧的已鉴权状态短路
    attempt_in_flight_ = false;
    ++connect_gen_;

    stop_heartbeat();
    stop_reconnect();
    auth_timer_->stop();
    open_heartbeat_timer_->stop();

    // 在途 start_app 不取消：让它自然返回；若成功但已过期，回调会用
    // 原凭据快照补发 best-effort end_app（退出路径除外，见析构函数）。
    abort_reply(heartbeat_reply_);   // 旧场次心跳已无意义

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
    handle_connection_lost("disconnected");
}

void DanmakuWebSocket::on_ws_error(QAbstractSocket::SocketError error)
{
    blog(LOG_WARNING, "[danmaku-open] websocket error: %s (code=%d)",
         ws_->errorString().toUtf8().constData(), static_cast<int>(error));

    // 连接从未建立时（DNS/TLS/握手失败）QWebSocket 不一定发 disconnected，
    // 此处兜底进入统一失败处理；已建立的连接由 disconnected 负责收尾。
    if (state_ != State::Connected &&
        ws_->state() == QAbstractSocket::UnconnectedState) {
        handle_connection_lost("socket error");
    }
}

void DanmakuWebSocket::on_ws_ssl_errors(const QList<QSslError> &errors)
{
    // 仅记录，不忽略：官方长连必须走正常 TLS 校验。
    for (const auto &e : errors) {
        blog(LOG_WARNING, "[danmaku-open] ssl error: %s", e.errorString().toUtf8().constData());
    }
}

// 单次 WSS 尝试的握手+鉴权超时：按连接失败处理（可切换集群），
// 与显式鉴权拒绝（凭据/场次问题）区分。
void DanmakuWebSocket::on_auth_timeout()
{
    if (shutdown_requested_ || !session_active_) return;
    if (authenticated_) return;   // 已鉴权成功，计时器已停止；防御迟到的事件

    blog(LOG_WARNING, "[danmaku-open] wss handshake/auth timeout (%d ms)",
         AUTH_DEADLINE_MS);
    if (ws_->state() != QAbstractSocket::UnconnectedState) {
        ws_->abort();   // 可能同步触发 disconnected 并完成收尾
    }
    if (!reconnect_timer_->isActive()) {
        handle_connection_lost("auth timeout");
    }
}

void DanmakuWebSocket::send_auth_packet()
{
    if (open_auth_body_.empty()) {
        blog(LOG_WARNING, "[danmaku-open] auth_body empty, aborting auth");
        if (ws_->state() != QAbstractSocket::UnconnectedState) ws_->close();
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
            if (shutdown_requested_ || !session_active_) {
                ws_->close();   // 已手动关闭/退出：忽略迟到的鉴权回复
                continue;
            }
            if (!auth_timer_->isActive()) {
                // 该次 WSS 尝试已结束（超时/已处理过鉴权回复）：迟到消息一律丢弃
                ws_->close();
                continue;
            }
            auto j = json::parse(pkt.body.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.value("code", -1) == 0) {
                auth_timer_->stop();
                authenticated_ = true;
                auth_rejections_ = 0;
                blog(LOG_INFO, "[danmaku-open] auth success! listening for official live events");
                start_heartbeat();
                reconnect_attempts_ = 0;   // 连接稳定，重置退避
                reset_link_failover();
                heartbeat_failures_ = 0;
                set_state(State::Connected);
            } else {
                handle_auth_rejection(pkt.body);
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

// 项目心跳：由场次级周期定时器驱动（20s 固定节拍）；上一拍未返回则跳过本拍。
void DanmakuWebSocket::send_open_http_heartbeat()
{
    if (shutdown_requested_ || !session_active_ || open_game_id_.empty()) {
        open_heartbeat_timer_->stop();
        return;
    }
    if (heartbeat_reply_) return;   // 在途请求由 QNAM transfer timeout 兜底

    const std::string game_id = open_game_id_;
    QNetworkReply *reply = danmaku::OpenLiveClient::post_heartbeat(
        *network_manager(), game_id, session_creds_);
    heartbeat_reply_ = reply;
    if (!reply) {
        blog(LOG_WARNING, "[danmaku-open] 项目心跳请求创建失败 (game_id=%s)",
             game_id.c_str());
        return;
    }

    connect(reply, &QNetworkReply::finished, this,
            [this, game_id, reply]() {
        if (heartbeat_reply_ == reply) heartbeat_reply_ = nullptr;

        // abort/退出产生的结果，或属于已结束的旧场次：一律不得影响当前场次。
        // 但到达绝对时限而被 abort 的请求仍算一次失败（服务端未收到心跳）。
        const bool deadline_expired =
            reply->property("danmaku_deadline_expired").toBool();
        const bool canceled =
            (reply->error() == QNetworkReply::OperationCanceledError) && !deadline_expired;
        const bool current = session_active_ && !shutdown_requested_ &&
                             game_id == open_game_id_;
        if (canceled || !current) {
            reply->deleteLater();
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            blog(LOG_WARNING, "[danmaku-open] 项目心跳请求失败: %s (game_id=%s)",
                 reply->errorString().toUtf8().constData(), game_id.c_str());
            reply->deleteLater();
            on_heartbeat_failure(game_id);
            return;
        }

        auto ack = danmaku::OpenLiveClient::parse_ack(reply->readAll());
        reply->deleteLater();
        if (!ack.ok) {
            blog(LOG_WARNING, "[danmaku-open] 项目心跳被官方拒绝: code=%d msg=%s (game_id=%s)",
                 ack.code, ack.msg.c_str(), game_id.c_str());
            on_heartbeat_failure(game_id);
        } else {
            heartbeat_failures_ = 0;
        }
    });
    abort_on_http_deadline(reply, HTTP_REQUEST_DEADLINE_MS);
}

// 异步结束官方场次（end_app）：失败记录日志并进入冷却；各自凭据用后即弃。
// 旧的 end_app 在途时不会被新 end 取消；超限时放弃最旧并冷却（不保证送达）。
void DanmakuWebSocket::release_open_project()
{
    authenticated_ = false;
    stop_heartbeat();
    open_heartbeat_timer_->stop();
    abort_reply(heartbeat_reply_);

    if (open_game_id_.empty() || session_creds_.access_key.empty()) {
        open_game_id_.clear();
        open_auth_body_.clear();
        wss_links_.clear();
        reset_link_failover();
        heartbeat_failures_ = 0;
        session_creds_ = {};
        return;
    }

    const std::string game_id = open_game_id_;
    const danmaku::OpenLiveCredentials creds = session_creds_;
    open_game_id_.clear();
    open_auth_body_.clear();
    wss_links_.clear();
    reset_link_failover();
    heartbeat_failures_ = 0;
    session_creds_ = {};

    if (shutdown_requested_) return;   // 退出路径不发起新请求（结果本不可知）
    post_end_app(game_id, creds);
}

// 追加一次 best-effort end_app。用于当前场次收尾，以及“迟到的 start 成功”补发。
// 不修改当前场次状态，因此旧场次的收尾不会影响已建立的新场次。
void DanmakuWebSocket::post_end_app(const std::string &game_id,
                                    const danmaku::OpenLiveCredentials &creds)
{
    if (game_id.empty() || creds.access_key.empty()) {
        blog(LOG_WARNING, "[danmaku-open] 缺少 end_app 所需凭据，无法结束场次 (game_id=%s)",
             game_id.c_str());
        return;
    }
    if (end_replies_.size() >= static_cast<size_t>(MAX_PENDING_END_APP)) {
        // 有界：放弃最旧的一次（服务端可能仍保留该场次，必须冷却且如实记录）
        const auto oldest = end_replies_.front();
        end_replies_.erase(end_replies_.begin());
        const std::string dropped_game = oldest.second;
        oldest.first->disconnect(this);
        oldest.first->abort();
        oldest.first->deleteLater();
        end_cooldown_until_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(COOLDOWN_BACKOFF_MS);
        blog(LOG_WARNING,
             "[danmaku-open] 在途 end_app 已达上限 %d，放弃最旧请求 (game_id=%s)；"
             "该场次服务端状态未知，进入 %d ms 冷却",
             MAX_PENDING_END_APP, dropped_game.c_str(), COOLDOWN_BACKOFF_MS);
    }

    QNetworkReply *reply = danmaku::OpenLiveClient::post_end(*network_manager(), game_id, creds);
    if (!reply) {
        blog(LOG_WARNING, "[danmaku-open] end_app 请求创建失败 (game_id=%s)", game_id.c_str());
        return;
    }
    end_replies_.emplace_back(reply, game_id);
    blog(LOG_INFO, "[danmaku-open] end_app 已发送 (game_id=%s)", game_id.c_str());

    connect(reply, &QNetworkReply::finished, this, [this, reply, game_id]() {
        end_replies_.erase(
            std::remove_if(end_replies_.begin(), end_replies_.end(),
                           [reply](const auto &entry) { return entry.first == reply; }),
            end_replies_.end());
        if (shutdown_requested_) {   // abort 回调：不再触碰任何状态
            reply->deleteLater();
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            blog(LOG_WARNING, "[danmaku-open] end_app 请求失败: %s (game_id=%s)",
                 reply->errorString().toUtf8().constData(), game_id.c_str());
            // 服务端可能仍保留旧场次：冷却后才有意义地重新 start_app。
            // 当前已有新场次时不设冷却：官方已允许新场次，旧 end 失败不构成阻塞。
            if (open_game_id_.empty()) {
                end_cooldown_until_ = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(COOLDOWN_BACKOFF_MS);
            }
        } else {
            auto ack = danmaku::OpenLiveClient::parse_ack(reply->readAll());
            if (!ack.ok) {
                blog(LOG_WARNING, "[danmaku-open] end_app 被官方拒绝: code=%d msg=%s (game_id=%s)",
                     ack.code, ack.msg.c_str(), game_id.c_str());
                if (open_game_id_.empty()) {
                    end_cooldown_until_ = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(COOLDOWN_BACKOFF_MS);
                }
            }
            // 成功不清除冷却：同批 end 中可能有被放弃/失败的场次，服务端状态未知
        }
        reply->deleteLater();
    });
    abort_on_http_deadline(reply, HTTP_REQUEST_DEADLINE_MS);
}

// 仅退出路径：断开回调后取消全部在途 end_app（服务端是否已处理不可知）
void DanmakuWebSocket::abort_all_end_replies()
{
    const auto pending = end_replies_;
    end_replies_.clear();
    for (const auto &entry : pending) {
        QNetworkReply *reply = entry.first;
        reply->disconnect(this);   // abort 会同步发 finished；退出时无需再进业务回调
        reply->abort();
        reply->deleteLater();
    }
}

bool DanmakuWebSocket::end_cooldown_active() const
{
    return end_cooldown_until_ > std::chrono::steady_clock::now();
}

// 指数退避：base × 2^n，按 max_delay_ms 封顶（默认 2s → 4s → 8s → 16s → 30s）。
// 指数与计数都做上限钳制，长期反复重连也不会让 int 溢出。
void DanmakuWebSocket::schedule_reconnect(int base_delay_ms, int max_delay_ms)
{
    if (shutdown_requested_ || !session_active_) return;

    const int exponent = std::min(reconnect_attempts_, MAX_BACKOFF_EXPONENT);
    int delay = base_delay_ms * (1 << exponent);
    delay = std::min(delay, max_delay_ms);
    if (reconnect_attempts_ < 1000) ++reconnect_attempts_;   // 饱和，避免 int 溢出

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
    if (shutdown_requested_ || !session_active_) return;
    // 已有连接尝试在进行或已建立连接：本轮交由该连接的结果继续驱动
    if (attempt_in_flight_ || state_ == State::Connected) return;

    // 官方场次仍有效：直接复用 auth_body 重连（必要时切换集群），不重新 start_app
    if (open_ws_on_chosen_link()) return;

    start_connect_attempt();
}

// 统一的断开/连接失败入口（幂等）：区分“从未鉴权成功”（优先切换集群）
// 与“已正常接收过消息”（原地重连）
void DanmakuWebSocket::handle_connection_lost(const char *reason)
{
    stop_heartbeat();
    auth_timer_->stop();
    popularity_ = 0;

    const bool was_authenticated = authenticated_;
    authenticated_ = false;

    blog(LOG_INFO, "[danmaku-open] websocket closed (%s, session_active=%d, authenticated=%d)",
         reason, session_active_ ? 1 : 0, was_authenticated ? 1 : 0);

    // 主动关闭：状态已由 disconnect_from_room() 收尾
    if (!session_active_ || shutdown_requested_) {
        set_state(State::Closed);
        return;
    }
    if (reconnect_timer_->isActive()) return;   // 已有重连计划（可能来自显式拒绝）

    attempt_in_flight_ = false;

    if (!was_authenticated) {
        // 未建立过可用连接：先切换集群，再进入整体退避
        mark_current_link_failed();
        const size_t fail_limit = std::max<size_t>(2, wss_links_.size() * 2);
        if (++ws_attempt_failures_ >= static_cast<int>(fail_limit)) {
            blog(LOG_WARNING,
                 "[danmaku-open] 连续 %d 次未通过鉴权，判定官方场次失效，结束场次后重新 start_app",
                 ws_attempt_failures_);
            release_open_project();
        }
    } else {
        ws_attempt_failures_ = 0;
    }

    // 网络抖动或对端主动断开：自动重连（指数退避）
    schedule_reconnect(RECONNECT_BASE_DELAY_MS);
}

// 显式鉴权拒绝（AuthReply code != 0）：属于凭据/场次失效，不是集群故障。
// 结束旧场次后延迟重试一次新场次；连续拒绝则停止，且不轮换集群链接。
void DanmakuWebSocket::handle_auth_rejection(const QByteArray &body)
{
    stop_heartbeat();
    auth_timer_->stop();
    authenticated_ = false;
    ++auth_rejections_;

    blog(LOG_WARNING, "[danmaku-open] auth failed: %s (连续显式拒绝 %d 次)",
         body.toStdString().c_str(), auth_rejections_);

    // 旧场次已不可用：正常结束，避免官方认为其未正常关闭
    release_open_project();

    if (auth_rejections_ > MAX_EXPLICIT_AUTH_RETRIES) {
        blog(LOG_WARNING,
             "[danmaku-open] 连续 %d 次显式鉴权拒绝，停止弹幕互动（不轮换集群）",
             auth_rejections_);
        session_active_ = false;
        stop_reconnect();
        open_heartbeat_timer_->stop();
        popularity_ = 0;
        if (ws_->state() != QAbstractSocket::UnconnectedState) ws_->close();
        set_state(State::Closed);
        return;
    }

    // 先排重连再关闭：disconnected 回调看到计时器已激活，不会再轮换集群
    schedule_reconnect(EXPLICIT_AUTH_RETRY_DELAY_MS, EXPLICIT_AUTH_RETRY_DELAY_MS);
    if (ws_->state() != QAbstractSocket::UnconnectedState) ws_->close();
}

// 项目心跳连续失败：官方 60s 无心跳会关闭场次，连续达到阈值即主动结束场次重新 start_app
void DanmakuWebSocket::on_heartbeat_failure(const std::string &game_id)
{
    if (shutdown_requested_ || !session_active_ || game_id != open_game_id_)
        return;   // 旧场次的迟到结果不得影响当前场次

    if (++heartbeat_failures_ < HEARTBEAT_FAILURE_LIMIT) return;

    blog(LOG_WARNING,
         "[danmaku-open] 项目心跳连续 %d 次失败，判定官方场次失效，结束场次后重新 start_app (game_id=%s)",
         heartbeat_failures_, game_id.c_str());
    release_open_project();
    // 先排重连再关闭，避免 disconnected 回调把服务端问题误记为集群故障
    schedule_reconnect(RECONNECT_BASE_DELAY_MS);
    if (ws_->state() != QAbstractSocket::UnconnectedState) ws_->close();
}

// 用当前场次的 auth_body 与选定集群链接建连
bool DanmakuWebSocket::open_ws_on_chosen_link()
{
    if (wss_links_.empty() || open_auth_body_.empty()) return false;

    const std::string &link = choose_wss_link();
    blog(LOG_INFO, "[danmaku-open] connecting to official websocket %s (game_id=%s, link %zu/%zu)",
         link.c_str(), open_game_id_.c_str(), wss_link_index_ + 1, wss_links_.size());

    // 每次 WSS 尝试都从干净的单连接状态开始；seq 按连接从 1 计。
    authenticated_ = false;
    stop_heartbeat();
    seq_ = 1;

    // 每次 WSS 尝试独立计时，覆盖握手与鉴权；成功/关闭时停止。
    auth_timer_->start(AUTH_DEADLINE_MS);
    ws_->open(QUrl(QString::fromStdString(link)));
    return true;
}

// 从当前索引开始选择第一个不在冷却期的集群链接；全部冷却时退回当前索引
const std::string &DanmakuWebSocket::choose_wss_link()
{
    const size_t count = wss_links_.size();
    const auto now = std::chrono::steady_clock::now();
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t index = (wss_link_index_ + offset) % count;
        auto cooldown = link_cooldowns_.find(wss_links_[index]);
        if (cooldown == link_cooldowns_.end() || cooldown->second <= now) {
            wss_link_index_ = index;
            return wss_links_[index];
        }
    }
    return wss_links_[wss_link_index_ % count];
}

void DanmakuWebSocket::mark_current_link_failed()
{
    if (wss_links_.empty()) return;

    link_cooldowns_[wss_links_[wss_link_index_]] =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(LINK_COOLDOWN_MS);
    wss_link_index_ = (wss_link_index_ + 1) % wss_links_.size();
}

void DanmakuWebSocket::reset_link_failover()
{
    wss_link_index_ = 0;
    link_cooldowns_.clear();
    ws_attempt_failures_ = 0;
}

// 取消在途回复：先摘指针再 abort。abort() 可能同步触发 finished，
// 回调通过指针/代次守卫把自己的请求当作迟到结果处理；deleteLater 可重复调用。
void DanmakuWebSocket::abort_reply(QNetworkReply *&reply)
{
    if (!reply) return;
    QNetworkReply *pending = reply;
    reply = nullptr;
    pending->abort();
    pending->deleteLater();
}

// 在 UI 线程构造不可变凭据快照，异步回调不再触碰 ConfigManager
danmaku::OpenLiveCredentials DanmakuWebSocket::snapshot_credentials() const
{
    danmaku::OpenLiveCredentials creds;
    if (!cfg_) return creds;

    creds.app_id = cfg_->danmaku.open_live_app_id;
    creds.access_key = cfg_->danmaku.open_live_access_key;
    creds.access_secret = cfg_->danmaku.open_live_secret;
    creds.code = cfg_->danmaku.open_live_code;
    return creds;
}
