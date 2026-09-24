#include "bilibili-api.h"

#include <obs-module.h>

// B 站 WBI 签名（w_rid）与 appsign 协议强制要求 MD5，无法替换为更安全的算法。
// 明确已知情 MD5 的密码学缺陷，此处抑制 OpenSSL 3.0 起的弃用警告。
#define OPENSSL_SUPPRESS_DEPRECATED

#include <algorithm>
#include <cstring>
#include <regex>
#include <sstream>
#include <iomanip>
#include <openssl/md5.h>

// ── static constants ──

#define BILI_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" \
    " AppleWebKit/537.36 (KHTML, like Gecko)" \
    " Chrome/120.0.0.0 Safari/537.36"

const std::string BilibiliApi::USER_AGENT = BILI_USER_AGENT;

const std::string BilibiliApi::APP_KEY = "aae92bc66f3edfab";
const std::string BilibiliApi::APP_SEC = "af125a0d5279fd576c1b4418a3e8276d";

const int BilibiliApi::MIXIN_KEY_ENC_TAB[64] = {
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30,
    4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
};

// ── helpers ──

static std::string json_value_raw(const json &v)
{
    if (v.is_string())
        return v.get<std::string>();
    return v.dump();
}

static std::string md5_hex(const std::string &in)
{
    unsigned char hash[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char *>(in.data()), in.size(), hash);
    std::ostringstream out;
    for (auto c : hash)
        out << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return out.str();
}

static std::string url_encode(const std::string &s)
{
    std::ostringstream out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    }
    return out.str();
}

// ── constructor / destructor ──

BilibiliApi::BilibiliApi() = default;
BilibiliApi::~BilibiliApi() = default;

void BilibiliApi::update_cookies(const std::unordered_map<std::string, std::string> &cookies)
{
    std::lock_guard<std::mutex> lock(api_mutex_);
    cookie_str_.clear();
    for (auto &[k, v] : cookies) {
        if (!cookie_str_.empty()) cookie_str_ += "; ";
        cookie_str_ += k + "=" + v;
        if (k == "bili_jct") csrf_ = v;
    }
}

std::string BilibiliApi::cookie_value(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(api_mutex_);
    std::string needle = name + "=";
    size_t pos = 0;
    while ((pos = cookie_str_.find(needle, pos)) != std::string::npos) {
        // 必须是完整 cookie 名（前一个字符为起始或分隔符）
        if (pos == 0 || cookie_str_[pos - 1] == ';' || cookie_str_[pos - 1] == ' ') {
            size_t val_start = pos + needle.size();
            size_t val_end = cookie_str_.find(';', val_start);
            return cookie_str_.substr(val_start,
                val_end == std::string::npos ? std::string::npos : val_end - val_start);
        }
        pos += needle.size();
    }
    return {};
}

int64_t BilibiliApi::cookie_uid() const
{
    try {
        return std::stoll(cookie_value("DedeUserID"));
    } catch (...) {
        return 0;
    }
}

// ── curl callbacks ──

size_t BilibiliApi::write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *str = static_cast<std::string *>(userdata);
    str->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

size_t BilibiliApi::header_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *cookies = static_cast<std::unordered_map<std::string, std::string> *>(userdata);
    std::string header(static_cast<char *>(ptr), size * nmemb);
    static const std::regex set_cookie_re("^set-cookie:\\s*([^=]+)=([^;]+)", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(header, m, set_cookie_re)) {
        (*cookies)[m[1]] = m[2];
    }
    return size * nmemb;
}

// ── request core ──

std::string BilibiliApi::build_query(const json &params) const
{
    if (params.is_null()) return "";
    std::string qs;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!qs.empty()) qs += "&";
        qs += url_encode(it.key()) + "=" + url_encode(json_value_raw(it.value()));
    }
    return qs;
}

json BilibiliApi::appsign(json params) const
{
    params["appkey"] = APP_KEY;
    std::string qs = build_query(params);
    params["sign"] = md5_hex(qs + APP_SEC);
    return params;
}


ApiResult BilibiliApi::do_request(const std::string &method, const std::string &url,
                                   const json &params, const json &data)
{
    std::string cookie_str;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        cookie_str = cookie_str_;
    }

    ApiResult res;
    CURL *curl = curl_easy_init();
    if (!curl) {
        res.msg = "curl initialization failed";
        return res;
    }

    std::string response_body;
    std::string postdata;
    std::unordered_map<std::string, std::string> resp_cookies;

    // Build URL with params
    std::string full_url = url;
    if (!params.is_null()) {
        full_url += "?" + build_query(params);
    }

    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_cookies);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Headers
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "accept: application/json, text/plain, */*");
    headers = curl_slist_append(headers, "accept-language: zh-CN,zh;q=0.9");
    headers = curl_slist_append(headers, "user-agent: " BILI_USER_AGENT);
    headers = curl_slist_append(headers, "origin: https://www.bilibili.com");
    headers = curl_slist_append(headers, "referer: https://www.bilibili.com/");

    if (method == "POST") {
        if (!data.is_null()) {
            postdata = build_query(data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)postdata.size());
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        }
        headers = curl_slist_append(headers, "content-type: application/x-www-form-urlencoded; charset=UTF-8");
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    // Cookies
    if (!cookie_str.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie_str.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode cc = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (cc != CURLE_OK) {
        curl_easy_cleanup(curl);
        res.code = -1;
        res.msg = curl_easy_strerror(cc);
        return res;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    try {
        auto j = json::parse(response_body);
        res.ok = true;
        res.data = j;
        res.code = j.value("code", -1);
        res.msg = j.value("msg", j.value("message", ""));
        res.response_cookies = resp_cookies;
    } catch (...) {
        res.msg = "JSON decode error";
    }

    return res;
}

ApiResult BilibiliApi::do_get(const std::string &url, const json &params)
{
    return do_request("GET", url, params, nullptr);
}

ApiResult BilibiliApi::do_post(const std::string &url, const json &data, const json &params)
{
    return do_request("POST", url, params, data);
}

// ── WBI signing ──

bool BilibiliApi::get_wbi_keys(std::string &img_key, std::string &sub_key)
{
    std::string cookie_str;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        cookie_str = cookie_str_;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.bilibili.com/x/web-interface/nav");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, "user-agent: " BILI_USER_AGENT);
    h = curl_slist_append(h, "referer: https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);

    // 携带当前登录态 Cookie
    if (!cookie_str.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie_str.c_str());
    }

    CURLcode cc = curl_easy_perform(curl);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);

    if (cc != CURLE_OK) return false;

    try {
        auto j = json::parse(body);
        // 未登录时 code=-101，但 wbi_img 数据仍然可用，不检查 code
        if (!j.contains("data") || !j["data"].contains("wbi_img")) return false;
        auto img_url = j["data"]["wbi_img"]["img_url"].get<std::string>();
        auto sub_url = j["data"]["wbi_img"]["sub_url"].get<std::string>();
        img_key = img_url.substr(img_url.rfind('/') + 1);
        sub_key = sub_url.substr(sub_url.rfind('/') + 1);
        img_key = img_key.substr(0, img_key.find('.'));
        sub_key = sub_key.substr(0, sub_key.find('.'));
        return true;
    } catch (...) {
        return false;
    }
}

std::string BilibiliApi::sign_wbi(json params, const std::string &img_key, const std::string &sub_key)
{
    // mixin key
    std::string mixin = img_key + sub_key;
    std::string mixed;
    for (int i = 0; i < 32 && i < 64; i++)
        mixed += mixin[MIXIN_KEY_ENC_TAB[i] % mixin.size()];

    // wts
    params["wts"] = std::to_string(std::time(nullptr));

    // sort keys
    std::vector<std::string> keys;
    for (auto it = params.begin(); it != params.end(); ++it)
        keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());

    std::string qs;
    for (auto &k : keys) {
        auto v = params[k].dump();
        // strip quotes
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        // filter !'()*
        std::string filtered;
        for (char c : v)
            if (c != '!' && c != '\'' && c != '(' && c != ')' && c != '*')
                filtered += c;
        if (!qs.empty()) qs += "&";
        qs += k + "=" + url_encode(filtered);
    }

    params["w_rid"] = md5_hex(qs + mixed);
    return qs + "&w_rid=" + params["w_rid"].get<std::string>();
}

// ── API methods ──

ApiResult BilibiliApi::get_passport_qrcode()
{
    return do_get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate");
}

ApiResult BilibiliApi::poll_passport_qrcode(const std::string &qrcode_key)
{
    // Need a separate call that also returns cookies
    CURL *curl = curl_easy_init();
    std::string body;
    std::unordered_map<std::string, std::string> resp_cookies;

    std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" + url_encode(qrcode_key);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_cookies);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, "user-agent: " BILI_USER_AGENT);
    h = curl_slist_append(h, "referer: https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);

    ApiResult res;
    CURLcode cc = curl_easy_perform(curl);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);

    if (cc != CURLE_OK) {
        res.msg = curl_easy_strerror(cc);
        return res;
    }

    try {
        auto j = json::parse(body);
        res.ok = true;
        res.data = j;
        res.response_cookies = resp_cookies;
    } catch (...) {
        res.msg = "JSON decode error";
    }
    return res;
}

ApiResult BilibiliApi::get_user_info()
{
    return do_get("https://api.bilibili.com/x/web-interface/nav");
}

ApiResult BilibiliApi::get_user_stat()
{
    return do_get("https://api.bilibili.com/x/web-interface/nav/stat");
}

ApiResult BilibiliApi::get_room_id_by_uid(const std::string &uid)
{
    return do_get("https://api.live.bilibili.com/room/v2/Room/room_id_by_uid",
                   json{{"uid", uid}});
}

ApiResult BilibiliApi::get_room_info(const std::string &room_id)
{
    return do_get("https://api.live.bilibili.com/room/v1/Room/get_info",
                   json{{"room_id", room_id}});
}

ApiResult BilibiliApi::get_area_list()
{
    return do_get("https://api.live.bilibili.com/room/v1/Area/getList",
                   json{{"show_pinyin", 1}});
}

ApiResult BilibiliApi::update_title(const std::string &room_id, const std::string &title)
{
    std::string csrf;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        csrf = csrf_;
    }
    return do_post("https://api.live.bilibili.com/room/v1/Room/update",
                    json{{"room_id", room_id}, {"platform", "pc_link"},
                          {"title", title}, {"csrf_token", csrf}, {"csrf", csrf}});
}

ApiResult BilibiliApi::update_area(const std::string &room_id, const std::string &area_id)
{
    std::string csrf;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        csrf = csrf_;
    }
    return do_post("https://api.live.bilibili.com/room/v1/Room/update",
                    json{{"room_id", room_id}, {"area_id", area_id},
                          {"platform", "pc_link"},
                          {"csrf_token", csrf}, {"csrf", csrf}});
}

ApiResult BilibiliApi::start_live(const std::string &room_id, const std::string &area_id)
{
    blog(LOG_INFO, "[bilibili-live-obs] start_live room=%s area=%s",
         room_id.c_str(), area_id.c_str());

    auto ts_res = do_get("https://api.bilibili.com/x/report/click/now");
    if (!ts_res.ok || ts_res.code != 0) {
        blog(LOG_WARNING, "[bilibili-live-obs] start_live: get_time failed code=%d msg=%s",
             ts_res.code, ts_res.msg.c_str());
        return ts_res;
    }

    std::string ts = json_value_raw(ts_res.data["data"]["now"]);

    auto v_params = appsign(json{{"system_version", 2}, {"ts", ts}});
    auto v_res = do_get("https://api.live.bilibili.com/xlive/app-blink/v1/liveVersionInfo/getHomePageLiveVersion",
                         v_params);
    if (!v_res.ok || v_res.code != 0) {
        blog(LOG_WARNING, "[bilibili-live-obs] start_live: get_version failed code=%d msg=%s",
             v_res.code, v_res.msg.c_str());
        return v_res;
    }

    std::string csrf;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        csrf = csrf_;
    }

    json post_data = appsign(json{
        {"room_id", room_id}, {"platform", "pc_link"},
        {"area_v2", area_id}, {"backup_stream", "0"},
        {"csrf_token", csrf}, {"csrf", csrf},
        {"build", v_res.data["data"]["build"]},
        {"version", v_res.data["data"]["curr_version"]},
        {"ts", ts}
    });
    auto result = do_post("https://api.live.bilibili.com/room/v1/Room/startLive", post_data);
    blog(LOG_INFO, "[bilibili-live-obs] start_live result: ok=%d code=%d msg=%s",
         result.ok, result.code, result.msg.c_str());
    return result;
}

ApiResult BilibiliApi::stop_live(const std::string &room_id)
{
    std::string csrf;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        csrf = csrf_;
    }
    return do_post("https://api.live.bilibili.com/room/v1/Room/stopLive",
                    json{{"room_id", room_id}, {"platform", "pc_link"},
                          {"csrf_token", csrf}, {"csrf", csrf}});
}

ApiResult BilibiliApi::get_server_time()
{
    return do_get("https://api.bilibili.com/x/report/click/now");
}

ApiResult BilibiliApi::logout_session()
{
    // passport 注销接口：携带 cookie（SESSDATA）+ biliCSRF，使会话服务端失效
    std::string csrf;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        csrf = csrf_;
    }
    json data = {{"biliCSRF", csrf}};
    return do_post("https://passport.bilibili.com/login/exit/v2", data);
}
