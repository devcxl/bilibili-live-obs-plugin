#pragma once

#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "danmaku/open-live-client.h"

using json = nlohmann::json;

struct UserData {
    std::string uid;
    std::string uname;
    std::string face;
    std::string cookie;
    std::string roomId;
    std::string csrf;
    int level = 0;
    int current_exp = 0;
    int next_exp = 0;
    int money = 0;
    int bcoin = 0;
    int following = 0;
    int follower = 0;
    int dynamic_count = 0;
    std::string last_title;
    std::string last_area_id;
    std::vector<std::string> last_area_name;

    json to_json() const;
    static UserData from_json(const json &j);
};

struct TtsConfigData {
    bool enabled = false;
    std::string key;
    std::string region = "eastasia";
    std::string voice = "zh-CN-XiaoxiaoNeural";
    std::string rate = "+0%";
    std::string pitch = "+0%";
    int volume = 100;
    bool read_danmaku = true;
    bool read_gift = true;
    bool read_sc = true;
    bool read_guard = true;              // 是否开启大航海上舰播报 (默认开启)
    bool read_like = false;              // 是否开启点赞播报 (默认关闭，防高频打扰)
    bool read_entry = true;              // 是否开启进房播报 (默认开启)
    int entry_filter = 0;                // 进房播报范围: 0=全部观众, 1=仅粉丝勋章, 2=仅大航海(舰长/提督/总督)
    bool merge_enabled = true;
};

struct DanmakuConfigData {
    int64_t open_live_app_id = 0;          // 开放平台 App ID
    std::string open_live_access_key;      // 开放平台 AccessKey ID
    std::string open_live_secret;          // 开放平台 AccessKey Secret (加密落盘)
    std::string open_live_code;            // 主播身份码 (Code)
    bool show_fans_medal = true;           // 是否展示粉丝勋章
    bool show_guard_badge = true;          // 是否展示大航海标志
    int max_display_count = 1000;          // 弹幕最大保留条数
};

class ConfigManager {
public:
    ConfigManager();

    void load();
    void save();

    std::unordered_map<std::string, UserData> users;
    std::string current_uid;
    TtsConfigData tts;
    DanmakuConfigData danmaku;

    // 开放平台凭据快照：供异步请求按值捕获，避免回调读取可变的配置
    [[nodiscard]] danmaku::OpenLiveCredentials open_live_credentials() const;

    static std::string config_dir();
    static std::string config_path();

private:
    std::string key_;
    std::string key_path();

    std::string load_or_create_key();
    std::string encrypt(const std::string &plain) const;
    std::string decrypt(const std::string &cipher) const;

    void migrate_legacy(const json &legacy);
};
