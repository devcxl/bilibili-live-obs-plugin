#include "auth-service.h"

#include <obs-module.h>

#include <regex>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <ctime>

// ── helpers ──

static std::string json_to_string(const json &v)
{
    if (v.is_number())
        return std::to_string(v.get<int64_t>());
    if (v.is_string())
        return v.get<std::string>();
    return v.dump();
}

static std::string mask_string(const std::string &s, int head = 2, int tail = 2)
{
    if (s.empty()) return "";
    if ((int)s.size() <= head + tail) return std::string(s.size(), '*');
    return s.substr(0, head) + "*****" + s.substr(s.size() - tail);
}

static std::string cookies_to_str(const std::unordered_map<std::string, std::string> &cookies)
{
    std::string result;
    for (auto &[k, v] : cookies) {
        if (!result.empty()) result += "; ";
        result += k + "=" + v;
    }
    return result;
}

// ── UserService ──

UserService::UserService(BilibiliApi *api, ConfigManager *cfg, SessionState *state)
    : api_(api), cfg_(cfg), state_(state) {}

json UserService::strip_sensitive(const json &data)
{
    json j = data;
    j.erase("cookie");
    j.erase("csrf");
    return j;
}

bool UserService::has_valid_session() const
{
    return !state_->room_id.empty() && !state_->csrf.empty();
}

void UserService::init_current_user()
{
    auto uid = cfg_->current_uid;
    if (uid.empty()) {
        blog(LOG_INFO, "[bilibili-live-obs] init_current_user: no current_uid");
        state_->clear();
        return;
    }
    auto it = cfg_->users.find(uid);
    if (it == cfg_->users.end()) {
        blog(LOG_WARNING, "[bilibili-live-obs] init_current_user: uid=%s not in users", uid.c_str());
        state_->clear();
        return;
    }

    auto &u = it->second;
    blog(LOG_INFO, "[bilibili-live-obs] init_current_user: uid=%s cookie_len=%zu",
         uid.c_str(), u.cookie.size());

    state_->clear();

    // parse cookies
    std::unordered_map<std::string, std::string> cookies;
    std::istringstream stream(u.cookie);
    std::string pair;
    while (std::getline(stream, pair, ';')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = pair.substr(0, eq);
            std::string v = pair.substr(eq + 1);
            // trim
            k.erase(0, k.find_first_not_of(" \t"));
            k.erase(k.find_last_not_of(" \t") + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t") + 1);
            cookies[k] = v;
        }
    }

    if (cookies.find("SESSDATA") == cookies.end()) {
        blog(LOG_WARNING, "[bilibili-live-obs] init_current_user: SESSDATA not found in cookie, login invalid");
        api_->update_cookies({});
        return;
    }

    blog(LOG_INFO, "[bilibili-live-obs] init_current_user: parsed %zu cookies, SESSDATA ok", cookies.size());
    api_->update_cookies(cookies);
    state_->uid = uid;
    state_->room_id = u.roomId;
    state_->csrf = u.csrf;
    state_->current_area_id = u.last_area_id;
    state_->current_area_names = u.last_area_name;
}

json UserService::save_user_data(const std::string &uid, const json &full_data,
                                  const std::string &cookie_str, const std::string &room_id,
                                  const std::string &csrf)
{
    blog(LOG_INFO, "[bilibili-live-obs] save_user_data: uid=%s cookie_len=%zu room=%s",
         uid.c_str(), cookie_str.size(), room_id.c_str());

    auto &users = cfg_->users;
    auto old_it = users.find(uid);
    UserData old_data;
    if (old_it != users.end()) old_data = old_it->second;

    UserData u;
    u.uid = uid;
    u.uname = full_data.value("uname", "未知用户");
    u.face = full_data.value("face", "");
    u.cookie = cookie_str;
    u.roomId = room_id;
    u.csrf = csrf;

    if (full_data.contains("level_info")) {
        u.level = full_data["level_info"].value("current_level", 0);
        u.current_exp = full_data["level_info"].value("current_exp", 0);
        u.next_exp = full_data["level_info"].value("next_exp", 0);
    }
    u.money = full_data.value("money", 0);
    if (full_data.contains("wallet")) {
        u.bcoin = full_data["wallet"].value("bcoin_balance", 0);
    }
    if (full_data.contains("stat")) {
        u.following = full_data["stat"].value("following", 0);
        u.follower = full_data["stat"].value("follower", 0);
        u.dynamic_count = full_data["stat"].value("dynamic_count", 0);
    }
    u.last_title = old_data.last_title;
    u.last_area_id = old_data.last_area_id;
    u.last_area_name = old_data.last_area_name;

    users[uid] = u;
    cfg_->current_uid = uid;
    cfg_->save();

    state_->uid = uid;
    state_->room_id = room_id;
    state_->csrf = csrf;
    state_->current_area_id = u.last_area_id;
    state_->current_area_names = u.last_area_name;

    return u.to_json();
}

json UserService::fetch_full_user_data()
{
    auto nav = api_->get_user_info();
    if (!nav.ok || nav.code != 0)
        return json{{"code", -1}, {"msg", nav.msg}};

    auto stat = api_->get_user_stat();
    json full = nav.data["data"];
    if (stat.ok && stat.code == 0)
        full["stat"] = stat.data["data"];
    else
        full["stat"] = json::object();

    return json{{"code", 0}, {"data", full}};
}

std::string UserService::fetch_room_id(const std::unordered_map<std::string, std::string> &cookies_dict)
{
    auto uid_it = cookies_dict.find("DedeUserID");
    if (uid_it != cookies_dict.end()) {
        auto res = api_->get_room_id_by_uid(uid_it->second);
        if (res.ok) {
            if (res.code == 0 && res.data.contains("data")) {
                auto &room_id_val = res.data["data"]["room_id"];
                if (room_id_val.is_number())
                    return std::to_string(room_id_val.get<uint64_t>());
                if (room_id_val.is_string() && !room_id_val.get<std::string>().empty())
                    return room_id_val.get<std::string>();
            }
            if (res.code == 404)
                return "";
        }
    }

    auto nav = api_->get_user_info();
    if (nav.ok && nav.code == 0) {
        auto live_room = nav.data["data"]["live_room"];
        if (!live_room.is_null()) {
            auto &rid_val = live_room["roomid"];
            std::string rid;
            if (rid_val.is_number())
                rid = std::to_string(rid_val.get<uint64_t>());
            else if (rid_val.is_string())
                rid = rid_val.get<std::string>();
            if (!rid.empty() && rid != "0") return rid;
        }
    }
    return "";
}

json UserService::load_saved_config()
{
    auto it = cfg_->users.find(cfg_->current_uid);
    if (it != cfg_->users.end())
        return json{{"code", 0}, {"data", strip_sensitive(it->second.to_json())}};
    return json{{"code", 0}, {"data", json::object()}};
}

json UserService::refresh_current_user()
{
    if (cfg_->current_uid.empty())
        return json{{"code", -1}, {"msg", "未登录"}};

    auto result = fetch_full_user_data();
    if (result["code"] != 0) return result;

    auto it = cfg_->users.find(cfg_->current_uid);
    if (it == cfg_->users.end())
        return json{{"code", -1}, {"msg", "用户不存在"}};

    auto saved = save_user_data(cfg_->current_uid, result["data"],
                                 it->second.cookie, it->second.roomId, it->second.csrf);
    return json{{"code", 0}, {"data", strip_sensitive(saved)}};
}

json UserService::logout(const std::string &uid)
{
    if (cfg_->users.find(uid) == cfg_->users.end())
        return json{{"code", -1}, {"msg", "账户不存在"}};

    // 先调用 B 站注销接口使 SESSDATA 服务端失效（凭据不可被重新利用），
    // 接口失败不阻止本地登出，但通过返回值告知 UI 提示用户
    bool server_logout_ok = false;
    if (uid == cfg_->current_uid && !state_->csrf.empty()) {
        // 确保 api 携带该账号的 cookie（与 csrf 匹配）
        api_->set_csrf(state_->csrf);
        ApiResult res = api_->logout_session();
        server_logout_ok = res.ok && res.code == 0;
        blog(LOG_INFO, "[bilibili-live-obs] logout session server result: ok=%d code=%d msg=%s",
             server_logout_ok, res.code, res.msg.c_str());
    }

    if (cfg_->users.erase(uid)) {
        if (cfg_->current_uid == uid) {
            cfg_->current_uid.clear();
            state_->clear();
            api_->update_cookies({});
        }
        cfg_->save();
        json result = {{"code", 0}, {"server_logout", server_logout_ok}};
        if (!server_logout_ok) {
            result["msg"] = "本地已登出，但 B 站会话注销接口调用失败，SESSDATA 可能仍有效";
        }
        return result;
    }
    return json{{"code", -1}, {"msg", "账户不存在"}};
}

// ── LiveService ──

LiveService::LiveService(BilibiliApi *api, ConfigManager *cfg, SessionState *state)
    : api_(api), cfg_(cfg), state_(state) {}

void LiveService::refresh_partitions()
{
    auto res = api_->get_area_list();
    if (!res.ok || res.code != 0) return;

    partition_map_.clear();
    for (auto &p : res.data["data"]) {
        std::string pname = p["name"].get<std::string>();
        std::unordered_map<std::string, std::string> subs;
        for (auto &s : p["list"])
            subs[s["name"].get<std::string>()] = json_to_string(s["id"]);
        partition_map_[pname] = subs;
    }
}

std::vector<std::string> LiveService::get_names_by_id(const std::string &area_id)
{
    if (partition_map_.empty()) refresh_partitions();
    for (auto &[p_name, subs] : partition_map_) {
        for (auto &[s_name, aid] : subs) {
            if (aid == area_id)
                return {p_name, s_name};
        }
    }
    return {};
}

std::string LiveService::get_area_id_for_update(const std::string &p_name, const std::string &s_name)
{
    if (partition_map_.empty()) refresh_partitions();
    if (!p_name.empty() && !s_name.empty()) {
        auto pit = partition_map_.find(p_name);
        if (pit != partition_map_.end()) {
            auto sit = pit->second.find(s_name);
            if (sit != pit->second.end())
                return sit->second;
        }
    }
    return "";
}

json LiveService::get_partitions()
{
    if (partition_map_.empty()) refresh_partitions();
    json data;
    for (auto &[p_name, subs] : partition_map_) {
        json arr = json::array();
        for (auto &[s_name, _] : subs)
            arr.push_back(s_name);
        data[p_name] = arr;
    }
    return json{{"code", 0}, {"data", data}};
}

json LiveService::update_title(const std::string &title)
{
    if (cfg_->current_uid.empty())
        return json{{"code", -1}, {"msg", "未登录"}};

    auto res = api_->update_title(state_->room_id, title);
    if (res.ok && res.code == 0) {
        auto it = cfg_->users.find(cfg_->current_uid);
        if (it != cfg_->users.end()) {
            it->second.last_title = title;
            cfg_->save();
        }
        return json{{"code", 0}};
    }
    return json{{"code", -1}, {"msg", res.msg}};
}

json LiveService::update_area(const std::string &p_name, const std::string &s_name)
{
    if (cfg_->current_uid.empty())
        return json{{"code", -1}, {"msg", "未登录"}};

    auto aid = get_area_id_for_update(p_name, s_name);
    if (aid.empty())
        return json{{"code", -1}, {"msg", "无效分区"}};

    auto res = api_->update_area(state_->room_id, aid);
    if (res.ok && res.code == 0) {
        state_->current_area_id = aid;
        state_->current_area_names = {p_name, s_name};

        auto it = cfg_->users.find(cfg_->current_uid);
        if (it != cfg_->users.end()) {
            it->second.last_area_id = aid;
            it->second.last_area_name = {p_name, s_name};
            cfg_->save();
        }
        return json{{"code", 0}};
    }
    return json{{"code", -1}, {"msg", res.msg}};
}

json LiveService::start_live(const std::string &p_name, const std::string &s_name, const std::string &title)
{
    if (state_->room_id.empty())
        return json{{"code", -1}, {"msg", "请先登录"}};

    // save title
    if (!title.empty()) {
        auto it = cfg_->users.find(cfg_->current_uid);
        if (it != cfg_->users.end()) {
            it->second.last_title = title;
            cfg_->save();
        }
    }

    // resolve area
    if (!p_name.empty() && !s_name.empty()) {
        auto aid = get_area_id_for_update(p_name, s_name);
        if (aid.empty()) {
            refresh_partitions();
            aid = get_area_id_for_update(p_name, s_name);
        }
        if (!aid.empty()) {
            state_->current_area_id = aid;
            state_->current_area_names = {p_name, s_name};
        } else {
            return json{{"code", -1}, {"msg", "无法识别分区: " + p_name + "-" + s_name}};
        }
    }

    if (state_->current_area_id.empty()) {
        auto it = cfg_->users.find(cfg_->current_uid);
        if (it != cfg_->users.end()) {
            state_->current_area_id = it->second.last_area_id.empty() ? "235" : it->second.last_area_id;
            state_->current_area_names = it->second.last_area_name;
        } else {
            state_->current_area_id = "235";
        }
    }

    auto res = api_->start_live(state_->room_id, state_->current_area_id);
    if (!res.ok)
        return json{{"code", -1}, {"msg", "网络错误"}};

    if (res.code == 0) {
        state_->is_live = true;

        auto found = get_names_by_id(state_->current_area_id);
        if (!found.empty()) state_->current_area_names = found;

        auto it = cfg_->users.find(cfg_->current_uid);
        if (it != cfg_->users.end()) {
            it->second.last_area_id = state_->current_area_id;
            it->second.last_area_name = state_->current_area_names;
            cfg_->save();
        }

        auto &data = res.data["data"];
        json rtmp1 = json::object();
        json rtmp2 = json::object();
        json srt_info = json::object();

        if (data.contains("rtmp")) {
            rtmp1["addr"] = data["rtmp"].value("addr", "");
            rtmp1["code"] = data["rtmp"].value("code", "");
        }
        if (data.contains("protocols")) {
            for (auto &p : data["protocols"]) {
                auto proto = p.value("protocol", "");
                if (proto == "rtmp" && p.contains("addr")) {
                    rtmp2["addr"] = p.value("addr", "");
                    rtmp2["code"] = p.value("code", "");
                } else if (proto == "srt" && p.contains("addr")) {
                    srt_info["addr"] = p.value("addr", "");
                    srt_info["code"] = p.value("code", "");
                }
            }
        }

        return json{{"code", 0}, {"data", {{"rtmp1", rtmp1}, {"rtmp2", rtmp2}, {"srt", srt_info}}}};
    }

    if (res.code == 60024) {
        std::string qr = res.data["data"].value("url", res.data["data"].value("qr", ""));
        return json{{"code", 60024}, {"qr", qr}};
    }

    if (res.code == 60043) {
        std::string qr = "https://www.bilibili.com/blackboard/live/face-auth-middle.html"
                         "?source_event=400&mid=" + state_->uid;
        return json{{"code", 60043}, {"qr", qr}};
    }

    return json{{"code", -1}, {"msg", res.msg}};
}

json LiveService::stop_live()
{
    auto res = api_->stop_live(state_->room_id);
    if (res.ok && res.code == 0) {
        state_->is_live = false;
        return json{{"code", 0}};
    }
    return json{{"code", -1}};
}

json LiveService::check_live_status()
{
    if (state_->room_id.empty())
        return json{{"code", -1}};

    auto res = api_->get_room_info(state_->room_id);
    if (res.ok && res.code == 0) {
        state_->is_live = (res.data["data"].value("live_status", 0) == 1);
        return json{{"code", 0}, {"is_live", state_->is_live}};
    }
    return json{{"code", -1}};
}

std::string LiveService::get_room_id() const
{
    return state_->room_id;
}

// ── AuthService ──

AuthService::AuthService(BilibiliApi *api, UserService *user_svc, LiveService *live_svc, SessionState *state)
    : api_(api), user_svc_(user_svc), live_svc_(live_svc), state_(state) {}

json AuthService::get_login_qrcode()
{
    auto res = api_->get_passport_qrcode();
    if (res.ok && res.code == 0)
        return json{{"code", 0}, {"data", res.data["data"]}};
    return json{{"code", -1}};
}

json AuthService::poll_login_status(const std::string &key)
{
    auto res = api_->poll_passport_qrcode(key);
    if (!res.ok)
        return json{{"code", -1}, {"msg", "网络请求失败"}};

    auto data = res.data.value("data", json::object());
    int code = data.value("code", -1);

    if (code == 0) {
        state_->clear();
        api_->update_cookies(res.response_cookies);

        blog(LOG_INFO, "[bilibili-live-obs] poll_login_status: login ok, %zu response cookies",
             res.response_cookies.size());

        std::string csrf;
        auto jct_it = res.response_cookies.find("bili_jct");
        if (jct_it != res.response_cookies.end()) csrf = jct_it->second;

        std::string room_id = user_svc_->fetch_room_id(res.response_cookies);
        if (room_id.empty())
            return json{{"code", -1}, {"msg", "获取直播间ID失败"}};

        auto full_result = user_svc_->fetch_full_user_data();
        if (full_result["code"] != 0)
            return json{{"code", -1}, {"msg", "获取用户信息失败"}};

        auto uid_it = res.response_cookies.find("DedeUserID");
        if (uid_it == res.response_cookies.end())
            return json{{"code", -1}, {"msg", "无法获取用户ID"}};

        std::string cookie_str = cookies_to_str(res.response_cookies);
        blog(LOG_INFO, "[bilibili-live-obs] poll_login_status: cookie_str_len=%zu", cookie_str.size());
        auto saved = user_svc_->save_user_data(uid_it->second, full_result["data"],
                                                cookie_str, room_id, csrf);
        live_svc_->refresh_partitions();
        return json{{"code", 0}, {"data", saved}};
    }

    return json{{"code", code}, {"msg", data.value("message", "")}};
}
