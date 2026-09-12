#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QTimer>
#include <QWebSocket>
#include <QAbstractSocket>
#include <QSslError>

#include "bilibili-api.h"
#include "config-manager.h"
#include "danmaku/danmaku-packet.h"

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
// 生命周期约定：不自动重连。连接被关闭/请求失败即退出本次弹幕互动会话
// （停心跳、结束官方项目），需要用户手动重连才会重新建立。
class DanmakuWebSocket : public QObject {
    Q_OBJECT
public:
    explicit DanmakuWebSocket(QObject *parent = nullptr);
    ~DanmakuWebSocket() override;

    // 依赖注入
    void set_api(BilibiliApi *api);
    void set_config(ConfigManager *cfg);

    // 生命周期管理
    void connect_to_room(const std::string &room_id = "");
    void disconnect_from_room();
    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] int popularity() const;

signals:
    void danmaku_received(const DanmakuMessage &msg);
    void gift_received(const GiftMessage &msg);
    void super_chat_received(const SuperChatMessage &msg);
    void guard_received(const GuardMessage &msg);
    void like_received(const LikeMessage &msg);
    void entry_received(const EntryMessage &msg);
    void connection_state_changed(bool connected, int popularity);

private slots:
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_binary_message(const QByteArray &data);
    void on_ws_error(QAbstractSocket::SocketError error);
    void on_ws_ssl_errors(const QList<QSslError> &errors);
    void send_heartbeat();
    void send_open_http_heartbeat();

private:
    void send_auth_packet();
    void start_heartbeat();
    void stop_heartbeat();
    void end_session();   // 连接关闭后终止本次弹幕互动（不自动重连）

    void connect_async(uint64_t gen);

    // 成员变量
    QWebSocket *ws_ = nullptr;
    QTimer *heartbeat_timer_ = nullptr;
    QTimer *open_heartbeat_timer_ = nullptr;
    BilibiliApi *api_ = nullptr;
    ConfigManager *cfg_ = nullptr;

    std::thread fetch_thread_;

    std::string room_id_;
    std::string open_game_id_;
    std::string open_auth_body_;

    uint32_t seq_ = 1;
    int popularity_ = 0;
    bool authenticated_ = false;
    bool is_connecting_ = false;
    // 本次弹幕互动会话是否处于活动状态：true 期间连接关闭/出错即视为退出互动
    bool session_active_ = false;

    // 连接代次：每次 connect/disconnect 递增
    std::atomic<uint64_t> connect_gen_{0};
};
