#include "config-manager.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

// ── base64 helpers ──

static std::string base64_encode(const unsigned char *data, size_t len)
{
    BIO *bio = BIO_new(BIO_s_mem());
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_push(b64, bio);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM *buf;
    BIO_get_mem_ptr(b64, &buf);
    std::string ret(buf->data, buf->length);
    BIO_free_all(b64);
    return ret;
}

static std::vector<unsigned char> base64_decode(const std::string &in)
{
    size_t len = in.size();
    std::vector<unsigned char> out(len);
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bio = BIO_new_mem_buf(in.data(), static_cast<int>(len));
    BIO_push(b64, bio);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    int n = BIO_read(b64, out.data(), static_cast<int>(len));
    out.resize(std::max(0, n));
    BIO_free_all(b64);
    return out;
}

// ── Fernet-like AES-128-CBC + HMAC-SHA256 ──

static const int SALT_LEN = 16;
static const int IV_LEN = 16;
static const int KEY_LEN = 16;  // AES-128
static const int HMAC_KEY_LEN = 16;
static const int TOTAL_KEY_LEN = 32; // encryption_key(16) + hmac_key(16)

static std::vector<unsigned char> derive_key(const std::string &base_key)
{
    // 使用 PBKDF2-HMAC-SHA1 派生 32 字节密钥（前 16 字节 AES-128 加密，后 16 字节 HMAC-SHA256 签名）
    const unsigned char salt[] = "bilibili-live-obs-salt";
    std::vector<unsigned char> derived(TOTAL_KEY_LEN);
    PKCS5_PBKDF2_HMAC_SHA1(
        base_key.data(), static_cast<int>(base_key.size()),
        salt, sizeof(salt) - 1, 100000,
        TOTAL_KEY_LEN, derived.data());
    return derived;
}

// ── UserData ──

json UserData::to_json() const
{
    return {
        {"uid", uid}, {"uname", uname}, {"face", face},
        {"cookie", cookie},
        {"roomId", roomId}, {"csrf", csrf},
        {"level", level}, {"current_exp", current_exp},
        {"next_exp", next_exp}, {"money", money},
        {"bcoin", bcoin}, {"following", following},
        {"follower", follower}, {"dynamic_count", dynamic_count},
        {"last_title", last_title}, {"last_area_id", last_area_id},
        {"last_area_name", last_area_name}
    };
}

UserData UserData::from_json(const json &j)
{
    UserData u;
    u.uid = j.value("uid", "");
    u.uname = j.value("uname", "");
    u.face = j.value("face", "");
    u.cookie = j.value("cookie", "");
    u.roomId = j.value("roomId", "");
    u.csrf = j.value("csrf", "");
    u.level = j.value("level", 0);
    u.current_exp = j.value("current_exp", 0);
    u.next_exp = j.value("next_exp", 0);
    u.money = j.value("money", 0);
    u.bcoin = j.value("bcoin", 0);
    u.following = j.value("following", 0);
    u.follower = j.value("follower", 0);
    u.dynamic_count = j.value("dynamic_count", 0);
    u.last_title = j.value("last_title", "");
    u.last_area_id = j.value("last_area_id", "");
    u.last_area_name = j.value("last_area_name", std::vector<std::string>{});
    return u;
}

// ── ConfigManager ──

std::string ConfigManager::config_dir()
{
    const char *env = std::getenv("BILIBILI_LIVE_OBS_CONFIG_HOME");
    if (env) return env;

    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg) return std::string(xdg) + "/bilibili-live-obs";

    const char *home = std::getenv("HOME");
    if (home) return std::string(home) + "/.config/bilibili-live-obs";

    return ".";
}

std::string ConfigManager::config_path()
{
    return config_dir() + "/config.json";
}

std::string ConfigManager::key_path()
{
    return config_dir() + "/.cookie_key";
}

ConfigManager::ConfigManager()
{
    key_ = load_or_create_key();
    load();
}

std::string ConfigManager::load_or_create_key()
{
    std::string kp = key_path();
    std::ifstream in(kp, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        auto s = ss.str();
        if (s.size() >= 16) return s;
    }
    // generate new key
    unsigned char buf[32];
    RAND_bytes(buf, sizeof(buf));
    std::string key(reinterpret_cast<char *>(buf), sizeof(buf));
    // ensure directory exists
    std::string dir = config_dir();
    mkdir(dir.c_str(), 0700);

    std::string tmp_kp = kp + ".tmp";
    {
        std::ofstream out(tmp_kp, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(key.data(), key.size());
            out.flush();
        }
    }
    chmod(tmp_kp.c_str(), S_IRUSR | S_IWUSR);
    std::rename(tmp_kp.c_str(), kp.c_str());
    return key;
}

std::string ConfigManager::encrypt(const std::string &plain) const
{
    if (plain.empty()) return "";

    auto derived = derive_key(key_);
    const unsigned char *enc_key = derived.data();
    const unsigned char *hmac_key = derived.data() + KEY_LEN;

    std::vector<unsigned char> iv(IV_LEN);
    RAND_bytes(iv.data(), IV_LEN);

    // encrypt
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, enc_key, iv.data());

    std::vector<unsigned char> cipher(plain.size() + 16);
    int outlen = 0, tmplen = 0;
    EVP_EncryptUpdate(ctx, cipher.data(), &outlen,
                      reinterpret_cast<const unsigned char *>(plain.data()),
                      static_cast<int>(plain.size()));
    EVP_EncryptFinal_ex(ctx, cipher.data() + outlen, &tmplen);
    outlen += tmplen;
    cipher.resize(outlen);
    EVP_CIPHER_CTX_free(ctx);

    // build payload: version(1) + iv(16) + cipher + hmac(32)
    std::vector<unsigned char> payload;
    payload.push_back(0x80); // version
    payload.insert(payload.end(), iv.begin(), iv.end());
    payload.insert(payload.end(), cipher.begin(), cipher.end());

    // HMAC-SHA256 over version + iv + cipher
    unsigned char hmac[32];
    unsigned int hlen = 0;
    HMAC(EVP_sha256(), hmac_key, HMAC_KEY_LEN,
         payload.data(), payload.size(), hmac, &hlen);
    payload.insert(payload.end(), hmac, hmac + hlen);

    return base64_encode(payload.data(), payload.size());
}

std::string ConfigManager::decrypt(const std::string &cipher) const
{
    if (cipher.empty()) return "";

    auto raw = base64_decode(cipher);
    // legacy plain cookie check: 版本字节 0x80 在解码后的 payload 首字节
    if (raw.size() < 1 + IV_LEN + 32 || raw[0] != 0x80) {
        // 不是加密格式 → 视为 legacy 明文 cookie，原样返回
        return cipher;
    }

    auto derived = derive_key(key_);
    const unsigned char *enc_key = derived.data();
    const unsigned char *hmac_key = derived.data() + KEY_LEN;

    // verify HMAC
    size_t hmac_start = raw.size() - 32;
    unsigned char computed[32];
    unsigned int hlen = 0;
    HMAC(EVP_sha256(), hmac_key, HMAC_KEY_LEN,
         raw.data(), hmac_start, computed, &hlen);
    if (hlen != 32 || memcmp(computed, raw.data() + hmac_start, 32) != 0)
        return "";

    const unsigned char *iv = raw.data() + 1;
    const unsigned char *enc = raw.data() + 1 + IV_LEN;
    int enclen = static_cast<int>(hmac_start - 1 - IV_LEN);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, enc_key, iv);

    std::vector<unsigned char> plain(enclen + 16);
    int outlen = 0, tmplen = 0;
    EVP_DecryptUpdate(ctx, plain.data(), &outlen, enc, enclen);
    int final_ok = EVP_DecryptFinal_ex(ctx, plain.data() + outlen, &tmplen);
    outlen += tmplen;
    plain.resize(outlen);
    EVP_CIPHER_CTX_free(ctx);

    if (final_ok <= 0) return "";

    return std::string(reinterpret_cast<char *>(plain.data()), plain.size());
}

void ConfigManager::migrate_legacy(const json &legacy)
{
    UserData u;
    u.uid = "default";
    u.uname = "Saved User";
    u.cookie = legacy.value("cookie", "");
    u.roomId = legacy.value("roomId", "");
    u.csrf = legacy.value("csrf", "");
    u.last_title = legacy.value("last_title", "");
    u.last_area_id = legacy.value("last_area_id", "");
    u.last_area_name = legacy.value("last_area_name", std::vector<std::string>{});
    users["default"] = u;
    current_uid = "default";
}

void ConfigManager::load()
{
    users.clear();
    current_uid.clear();

    std::string dir = config_dir();
    mkdir(dir.c_str(), 0700);

    std::ifstream in(config_path());
    if (!in.is_open()) return;

    json j;
    try {
        in >> j;
    } catch (...) { return; }

    // legacy migration
    if (j.contains("cookie") && !j.contains("users")) {
        migrate_legacy(j);
        save();
        return;
    }

    current_uid = j.value("current_uid", "");
    if (j.contains("users")) {
        for (auto &[uid, uj] : j["users"].items()) {
            auto u = UserData::from_json(uj);
            u.cookie = decrypt(u.cookie);
            users[uid] = u;
        }
    }

    if (j.contains("tts")) {
        const auto &tj = j["tts"];
        tts.enabled = tj.value("enabled", false);
        tts.key = decrypt(tj.value("key", ""));
        tts.region = tj.value("region", "eastasia");
        tts.voice = tj.value("voice", "zh-CN-XiaoxiaoNeural");
        tts.rate = tj.value("rate", "+0%");
        tts.pitch = tj.value("pitch", "+0%");
        tts.volume = tj.value("volume", 100);
        tts.read_danmaku = tj.value("read_danmaku", true);
        tts.read_gift = tj.value("read_gift", true);
        tts.read_sc = tj.value("read_sc", true);
        tts.read_guard = tj.value("read_guard", true);
        tts.read_like = tj.value("read_like", false);
        tts.read_entry = tj.value("read_entry", true);
        tts.entry_filter = tj.value("entry_filter", 0);
        tts.merge_enabled = tj.value("merge_enabled", true);
    }

    if (j.contains("danmaku")) {
        const auto &dj = j["danmaku"];
        danmaku.open_live_app_id = dj.value("open_live_app_id", 0LL);
        danmaku.open_live_access_key = dj.value("open_live_access_key", "");
        danmaku.open_live_secret = decrypt(dj.value("open_live_secret", ""));
        danmaku.open_live_code = dj.value("open_live_code", "");
        danmaku.show_fans_medal = dj.value("show_fans_medal", true);
        danmaku.show_guard_badge = dj.value("show_guard_badge", true);
        danmaku.max_display_count = dj.value("max_display_count", 1000);
    }
}

void ConfigManager::save()
{
    json j;
    j["current_uid"] = current_uid;

    json uj;
    for (auto &[uid, u] : users) {
        auto copy = u;
        copy.cookie = encrypt(copy.cookie);
        uj[uid] = copy.to_json();
    }
    j["users"] = uj;

    json tj;
    tj["enabled"] = tts.enabled;
    tj["key"] = encrypt(tts.key);
    tj["region"] = tts.region;
    tj["voice"] = tts.voice;
    tj["rate"] = tts.rate;
    tj["pitch"] = tts.pitch;
    tj["volume"] = tts.volume;
    tj["read_danmaku"] = tts.read_danmaku;
    tj["read_gift"] = tts.read_gift;
    tj["read_sc"] = tts.read_sc;
    tj["read_guard"] = tts.read_guard;
    tj["read_like"] = tts.read_like;
    tj["read_entry"] = tts.read_entry;
    tj["entry_filter"] = tts.entry_filter;
    tj["merge_enabled"] = tts.merge_enabled;
    j["tts"] = tj;

    json dj;
    dj["open_live_app_id"] = danmaku.open_live_app_id;
    dj["open_live_access_key"] = danmaku.open_live_access_key;
    dj["open_live_secret"] = encrypt(danmaku.open_live_secret);
    dj["open_live_code"] = danmaku.open_live_code;
    dj["show_fans_medal"] = danmaku.show_fans_medal;
    dj["show_guard_badge"] = danmaku.show_guard_badge;
    dj["max_display_count"] = danmaku.max_display_count;
    j["danmaku"] = dj;

    std::string dir = config_dir();
    mkdir(dir.c_str(), 0700);

    std::string target_path = config_path();
    std::string tmp_path = target_path + ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) return;
        out << j.dump(2);
        out.flush();
    }

    // 原子替换，防止写入中断导致原配置文件损坏
    std::rename(tmp_path.c_str(), target_path.c_str());
}
