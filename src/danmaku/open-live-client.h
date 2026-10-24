#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace danmaku {

// 开放平台开发者凭据快照：在 UI 线程构造，按值传递给异步请求，
// 避免异步回调直接读取可变的 ConfigManager（CP.31）。
struct OpenLiveCredentials {
    int64_t app_id = 0;
    std::string access_key;
    std::string access_secret;
    std::string code;

    [[nodiscard]] bool complete() const
    {
        return app_id > 0 && !access_key.empty() && !access_secret.empty() && !code.empty();
    }
};

struct OpenLiveStartResult {
    bool ok = false;
    int code = -1;
    std::string msg;
    std::string game_id;
    std::string auth_body;
    std::vector<std::string> wss_links;
    int64_t room_id = 0;
    std::string anchor_name;
};

// 项目心跳 / end_app 等仅需业务码与文案的响应
struct OpenLiveAck {
    bool ok = false;
    int code = -1;
    std::string msg;
};

// B站直播开放平台（Open Live）协议客户端：
// 负责官方签名（HMAC-SHA256）、请求体构造与响应解析；传输由调用方注入的
// QNetworkAccessManager 在其所属线程异步完成（不阻塞、可用 abort 取消）。
class OpenLiveClient {
public:
    // 三个官方接口的异步请求；返回在途回复，调用方负责 finished 处理后 deleteLater()
    static QNetworkReply *post_start(QNetworkAccessManager &nam,
                                     const OpenLiveCredentials &creds);
    static QNetworkReply *post_heartbeat(QNetworkAccessManager &nam,
                                         const std::string &game_id,
                                         const OpenLiveCredentials &creds);
    static QNetworkReply *post_end(QNetworkAccessManager &nam,
                                   const std::string &game_id,
                                   const OpenLiveCredentials &creds);

    // 纯解析函数（无状态、不抛异常）
    static OpenLiveStartResult parse_start_response(const QByteArray &body);
    static OpenLiveAck parse_ack(const QByteArray &body);
};

} // namespace danmaku
