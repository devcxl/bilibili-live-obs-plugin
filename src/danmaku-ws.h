#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QAbstractSocket>
#include <QByteArray>
#include <QMetaType>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QSslError>
#include <QTimer>
#include <QWebSocket>

#include "config-manager.h"
#include "danmaku/danmaku-packet.h"
#include "danmaku/open-live-client.h"

// ─── 弹幕消息 ───
struct DanmakuMessage {
    std::string cmd;           // "LIVE_OPEN_PLATFORM_DM"
    std::string username;      // 发送者昵称
    std::string uid;           // 发送者 OpenID
    std::string message;       // 弹幕文本
    std::string fan_badge;     // 粉丝勋章名（可选）
    int fan_badge_level = 0;   // 粉丝勋章等级
    int guard_level = 0;       // 大航海等级：0=无，1=总督，2=提督，3=舰长
    bool is_admin = false;     // 是否房管
    int dm_type = 0;           // 0: 普通弹幕, 1: 表情包弹幕
    std::string emoji_url;     // 表情包图片 URL
};

// ─── 礼物消息 ───
struct GiftMessage {
    std::string username;      // 送礼者昵称
    std::string uid;           // 送礼者 OpenID
    std::string gift_name;     // 礼物名称
    int num = 1;               // 数量
    int combo_num = 0;         // 连送次数（combo）
    int64_t price = 0;         // 单价（1000 = 1元 = 10电池）
    bool paid = true;          // 是否付费礼物
    int guard_level = 0;       // 大航海等级
    std::string medal_name;    // 勋章名
    int medal_level = 0;       // 勋章等级
    std::string action = "赠送"; // "赠送" / "开通" / "续费"
};

// ─── SC 醒目留言 ───
struct SuperChatMessage {
    int64_t message_id = 0;    // 留言 ID
    std::string username;      // 发送者昵称
    std::string uid;           // 发送者 OpenID
    std::string message;       // SC 内容
    int price = 0;             // 金额（单位：分，例如 5000 代表 50 元）
    int guard_level = 0;       // 大航海等级
    std::string medal_name;    // 勋章名
    int medal_level = 0;       // 勋章等级
};

// ─── 付费大航海（上舰/续费）───
struct GuardMessage {
    std::string username;      // 上舰用户昵称
    std::string uid;           // 用户 OpenID
    int guard_level = 3;       // 1: 总督, 2: 提督, 3: 舰长
    int guard_num = 1;         // 数量
    std::string guard_unit = "月"; // 单位（如"月"）
    std::string medal_name;    // 粉丝勋章名
    int medal_level = 0;       // 粉丝勋章等级
    int64_t timestamp = 0;     // 秒级时间戳
};

// ─── 点赞信息 ───
struct LikeMessage {
    std::string username;      // 点赞用户昵称
    std::string uid;           // 用户 OpenID
    std::string like_text = "点赞了"; // 点赞文案
    int64_t like_count = 1;    // 点赞点击次数
    std::string medal_name;    // 粉丝勋章名
    int medal_level = 0;       // 粉丝勋章等级
    int64_t timestamp = 0;     // 时间戳
};

// ─── 进房与互动消息 ───
struct EntryMessage {
    std::string username;      // 观众昵称
    std::string uid;           // 观众 OpenID
    int guard_level = 0;       // 大航海等级：0=无，1=总督，2=提督，3=舰长
    std::string medal_name;    // 粉丝勋章名
    int medal_level = 0;       // 粉丝勋章等级
    int msg_type = 1;          // 1: 进入房间, 2: 关注, 3: 分享
};

// 注册到 Qt 元对象系统（支持跨线程信号/槽值类型传递）
Q_DECLARE_METATYPE(DanmakuMessage)
Q_DECLARE_METATYPE(GiftMessage)
Q_DECLARE_METATYPE(SuperChatMessage)
Q_DECLARE_METATYPE(GuardMessage)
Q_DECLARE_METATYPE(LikeMessage)
Q_DECLARE_METATYPE(EntryMessage)

// ─── B站开放平台 (Open Live) 官方长连接客户端 ───
// 生命周期约定：
//   * 用户开启弹幕互动后，网络抖动/对端关闭会触发指数退避自动重连；
//   * 用户主动关闭（disconnect_from_room）后不再自动重连；
//   * 官方 HTTP（start_app / 项目心跳 / end_app）经 UI 线程的
//     QNetworkAccessManager 异步执行，不阻塞、可 abort；析构时不等待网络，
//     仅取消在途请求（此时服务端是否已处理在协议上无法确认）。
class DanmakuWebSocket : public QObject {
    Q_OBJECT
public:
    // 连接状态（驱动 UI：按钮文案 / 可用性）
    enum class State {
        Closed,      // 已关闭：未建立连接且无重连计划
        Connecting,  // 连接中：握手/鉴权中，或处于重连退避等待
        Connected,   // 已连接：鉴权成功，正常接收弹幕
    };
    Q_ENUM(State)

    explicit DanmakuWebSocket(QObject *parent = nullptr);
    ~DanmakuWebSocket() override;

    // 依赖注入
    void set_config(ConfigManager *cfg);
    // 测试/嵌入用：注入不归本对象所有的 QNetworkAccessManager，必须比本对象活得久；
    // 传 nullptr 恢复内置 manager。
    void set_network_access_manager(QNetworkAccessManager *manager);

    // 生命周期管理
    void connect_to_room(const std::string &room_id = "");
    void disconnect_from_room();
    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] int popularity() const;

signals:
    void danmaku_received(const DanmakuMessage &msg);
    void gift_received(const GiftMessage &msg);
    void super_chat_received(const SuperChatMessage &msg);
    void guard_received(const GuardMessage &msg);
    void like_received(const LikeMessage &msg);
    void entry_received(const EntryMessage &msg);
    void connection_state_changed(DanmakuWebSocket::State state, int popularity);

private slots:
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_binary_message(const QByteArray &data);
    void on_ws_error(QAbstractSocket::SocketError error);
    void on_ws_ssl_errors(const QList<QSslError> &errors);
    void send_heartbeat();
    void send_open_http_heartbeat();
    void attempt_reconnect();
    void on_auth_timeout();

private:
    void send_auth_packet();
    void start_heartbeat();
    void stop_heartbeat();
    void set_state(State state);
    void start_connect_attempt();                              // 发起 start_app 连接尝试
    void on_start_reply(QNetworkReply *reply, uint64_t gen,
                        const danmaku::OpenLiveCredentials &creds); // start_app 异步回复
    void handle_connection_lost(const char *reason);           // 统一的断开/连接失败入口（幂等）
    void handle_auth_rejection(const QByteArray &body);        // 长连显式鉴权拒绝（非集群故障）
    void on_heartbeat_failure(const std::string &game_id);     // 项目心跳失败累计与场次失效判定
    bool open_ws_on_chosen_link();                             // 用当前 auth_body + 选定集群链接建连
    const std::string &choose_wss_link();                      // 轮换跳过冷却中的链接
    void mark_current_link_failed();                           // 当前链接冷却并轮换到下一位
    void reset_link_failover();                                // 复位链接索引/冷却/失败计数
    void release_open_project();                               // 异步结束官方场次（end_app）
    void post_end_app(const std::string &game_id,
                      const danmaku::OpenLiveCredentials &creds); // 追加一次 best-effort end_app
    void abort_all_end_replies();                              // 取消全部在途 end_app（仅退出时）
    [[nodiscard]] bool has_pending_end_app() const { return !end_replies_.empty(); }
    [[nodiscard]] bool end_cooldown_active() const;
    void schedule_reconnect(int base_delay_ms = RECONNECT_BASE_DELAY_MS,
                            int max_delay_ms = RECONNECT_MAX_DELAY_MS);
    void stop_reconnect();
    void abort_reply(QNetworkReply *&reply);                   // abort + deleteLater 在途回复
    [[nodiscard]] danmaku::OpenLiveCredentials snapshot_credentials() const;
    [[nodiscard]] QNetworkAccessManager *network_manager();

    // 成员变量
    QWebSocket *ws_ = nullptr;
    QTimer *heartbeat_timer_ = nullptr;
    QTimer *open_heartbeat_timer_ = nullptr;
    QTimer *reconnect_timer_ = nullptr;
    QTimer *auth_timer_ = nullptr;   // 单次 WSS 尝试的握手+鉴权总时限
    QNetworkAccessManager nam_;   // UI 线程创建并使用，随本对象析构回收在途回复
    QNetworkAccessManager *nam_injected_ = nullptr;   // 非拥有；仅测试/嵌入注入
    ConfigManager *cfg_ = nullptr;

    QNetworkReply *start_reply_ = nullptr;       // 在途 start_app
    QNetworkReply *heartbeat_reply_ = nullptr;   // 在途项目心跳
    // 在途 end_app（可并发，数量有界）；game_id 用于超限时准确记录被放弃的场次
    std::vector<std::pair<QNetworkReply *, std::string>> end_replies_;

    std::string room_id_;
    std::string open_game_id_;
    std::string open_auth_body_;

    // 官方场次的集群链接（wss_link）：多个地址互为不同集群的镜像，失败时轮换
    std::vector<std::string> wss_links_;
    size_t wss_link_index_ = 0;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> link_cooldowns_;
    int ws_attempt_failures_ = 0;   // 本场次内未达 Connected 的连续失败数
    int heartbeat_failures_ = 0;    // 项目心跳连续失败数
    int auth_rejections_ = 0;       // 连续显式鉴权拒绝次数（鉴权成功后清零）

    // end_app 失败可能意味着服务端仍保留旧场次：在此之前不重新 start_app
    std::chrono::steady_clock::time_point end_cooldown_until_{};

    // 官方 HTTP 的凭据快照：attempt_ 为本次尝试，session_ 为成功建立场次后固定
    danmaku::OpenLiveCredentials attempt_creds_;
    danmaku::OpenLiveCredentials session_creds_;

    uint32_t seq_ = 1;
    int popularity_ = 0;
    State state_ = State::Closed;

    // 是否已通过鉴权，可以收发心跳与业务消息（由 state_ == Connected 派生）
    bool authenticated_ = false;

    // 用户意图：true = 保持弹幕互动开启（断开后自动重连）；false = 已手动关闭
    bool session_active_ = false;

    // 连接代次：每次 connect/disconnect 递增
    std::atomic<uint64_t> connect_gen_{0};

    int reconnect_attempts_ = 0;
    // 是否有一次 start_app 连接尝试正在进行（其回复尚未返回）
    bool attempt_in_flight_ = false;
    // 析构标记：禁止再发起任何新请求或重连
    bool shutdown_requested_ = false;

    static constexpr int RECONNECT_BASE_DELAY_MS = 2000;
    static constexpr int RECONNECT_MAX_DELAY_MS = 30000;
    static constexpr int COOLDOWN_BACKOFF_MS = 60000;   // 疑似官方冷却期（7001 未验证）的退避
    static constexpr int HTTP_TRANSFER_TIMEOUT_MS = 10000;
    static constexpr int HTTP_REQUEST_DEADLINE_MS = 15000; // 单请求绝对时限（QNAM 仅空闲超时）
    static constexpr int AUTH_DEADLINE_MS = 10000;      // 单次 WSS 握手+鉴权总时限
    static constexpr int EXPLICIT_AUTH_RETRY_DELAY_MS = 5000;
    static constexpr int MAX_EXPLICIT_AUTH_RETRIES = 1; // 显式拒绝后最多换新场次重试一次
    static constexpr int MAX_PENDING_END_APP = 4;       // 并发上限；超限放弃最旧并冷却，不保证全部送达
    static constexpr int END_PENDING_RETRY_MS = 1000;   // 旧场次 end_app 未完成时的重试间隔
    static constexpr int OPEN_HEARTBEAT_INTERVAL_MS = 20000;
    static constexpr int MAX_BACKOFF_EXPONENT = 4;      // 2s×2^4 = 32s → 封顶 30s
    static constexpr int LINK_COOLDOWN_MS = 60000;      // 单集群冷却，避免对故障集群热循环
    static constexpr int HEARTBEAT_FAILURE_LIMIT = 2;   // 连续心跳失败达到该值判定场次失效
};

Q_DECLARE_METATYPE(DanmakuWebSocket::State)
