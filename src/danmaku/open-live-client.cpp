#include "open-live-client.h"

#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/hmac.h>
#include <openssl/md5.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

#include <QNetworkRequest>
#include <QUrl>
#include <nlohmann/json.hpp>

namespace danmaku {
namespace {

constexpr const char *START_URL = "https://live-open.biliapi.com/v2/app/start";
constexpr const char *HEARTBEAT_URL = "https://live-open.biliapi.com/v2/app/heartbeat";
constexpr const char *END_URL = "https://live-open.biliapi.com/v2/app/end";

std::string md5_hex(const std::string &data)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest);
    std::ostringstream oss;
    for (unsigned char byte : digest)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return oss.str();
}

std::string hmac_sha256_hex(const std::string &key, const std::string &data)
{
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(),
         result, &result_len);

    std::ostringstream oss;
    for (unsigned int i = 0; i < result_len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(result[i]);
    return oss.str();
}

std::string generate_nonce()
{
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(now);
    std::uniform_int_distribution<uint64_t> dist;
    return std::to_string(dist(rng));
}

// 官方签名：x-bili-* 头按字段名字典序拼接成 string_to_sign 后 HMAC-SHA256，
// Authorization 在签名后追加（不参与签名输入）。
std::map<std::string, std::string> signed_headers(const QByteArray &body,
                                                  const OpenLiveCredentials &creds)
{
    std::map<std::string, std::string> headers = {
        {"x-bili-accesskeyid", creds.access_key},
        {"x-bili-content-md5", md5_hex(std::string(body.constData(), static_cast<size_t>(body.size())))},
        {"x-bili-signature-method", "HMAC-SHA256"},
        {"x-bili-signature-nonce", generate_nonce()},
        {"x-bili-signature-version", "1.0"},
        {"x-bili-timestamp", std::to_string(std::time(nullptr))},
    };

    std::string string_to_sign;
    for (const auto &[key, value] : headers)
        string_to_sign += key + ":" + value + "\n";
    if (!string_to_sign.empty() && string_to_sign.back() == '\n')
        string_to_sign.pop_back();

    headers["Authorization"] = hmac_sha256_hex(creds.access_secret, string_to_sign);
    return headers;
}

QNetworkReply *post_json(QNetworkAccessManager &nam, const char *url,
                         const QByteArray &body, const OpenLiveCredentials &creds)
{
    QNetworkRequest request{QUrl(QString::fromUtf8(url))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    for (const auto &[key, value] : signed_headers(body, creds))
        request.setRawHeader(QByteArray::fromStdString(key), QByteArray::fromStdString(value));
    return nam.post(request, body);
}

// nlohmann::json 跨类型提取会抛 type_error，统一走防御式读取
std::string json_str(const nlohmann::json &obj, const char *key)
{
    auto it = obj.find(key);
    if (it == obj.end()) return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
}

int json_int(const nlohmann::json &obj, const char *key, int fallback)
{
    auto it = obj.find(key);
    return (it != obj.end() && it->is_number()) ? it->get<int>() : fallback;
}

nlohmann::json parse_json(const QByteArray &body)
{
    return nlohmann::json::parse(body.constData(), body.constData() + body.size(), nullptr, false);
}

} // namespace

QNetworkReply *OpenLiveClient::post_start(QNetworkAccessManager &nam,
                                          const OpenLiveCredentials &creds)
{
    nlohmann::json request_body = {
        {"code", creds.code},
        {"app_id", creds.app_id},
    };
    return post_json(nam, START_URL, QByteArray::fromStdString(request_body.dump()), creds);
}

QNetworkReply *OpenLiveClient::post_heartbeat(QNetworkAccessManager &nam,
                                              const std::string &game_id,
                                              const OpenLiveCredentials &creds)
{
    nlohmann::json request_body = {{"game_id", game_id}};
    return post_json(nam, HEARTBEAT_URL, QByteArray::fromStdString(request_body.dump()), creds);
}

QNetworkReply *OpenLiveClient::post_end(QNetworkAccessManager &nam,
                                        const std::string &game_id,
                                        const OpenLiveCredentials &creds)
{
    nlohmann::json request_body = {
        {"app_id", creds.app_id},
        {"game_id", game_id},
    };
    return post_json(nam, END_URL, QByteArray::fromStdString(request_body.dump()), creds);
}

OpenLiveStartResult OpenLiveClient::parse_start_response(const QByteArray &body)
{
    OpenLiveStartResult res;

    auto j = parse_json(body);
    if (j.is_discarded()) {
        res.msg = "解析官方开放平台响应 JSON 失败";
        return res;
    }

    res.code = json_int(j, "code", -1);
    res.msg = json_str(j, "message");
    if (res.msg.empty()) res.msg = json_str(j, "msg");

    if (res.code != 0 || !j.contains("data") || !j["data"].is_object())
        return res;

    const auto &data = j["data"];
    res.ok = true;

    if (data.contains("game_info") && data["game_info"].is_object())
        res.game_id = json_str(data["game_info"], "game_id");

    if (data.contains("anchor_info") && data["anchor_info"].is_object()) {
        const auto &anchor = data["anchor_info"];
        res.room_id = json_int(anchor, "room_id", 0);
        res.anchor_name = json_str(anchor, "uname");
    }

    if (data.contains("websocket_info") && data["websocket_info"].is_object()) {
        const auto &ws_info = data["websocket_info"];
        res.auth_body = json_str(ws_info, "auth_body");
        auto links = ws_info.find("wss_link");
        if (links != ws_info.end() && links->is_array()) {
            for (const auto &link : *links) {
                if (link.is_string()) res.wss_links.push_back(link.get<std::string>());
            }
        }
    }

    return res;
}

OpenLiveAck OpenLiveClient::parse_ack(const QByteArray &body)
{
    OpenLiveAck ack;

    auto j = parse_json(body);
    if (j.is_discarded()) {
        ack.msg = "解析官方开放平台响应 JSON 失败";
        return ack;
    }

    ack.code = json_int(j, "code", -1);
    ack.msg = json_str(j, "message");
    if (ack.msg.empty()) ack.msg = json_str(j, "msg");
    ack.ok = (ack.code == 0);
    return ack;
}

} // namespace danmaku
