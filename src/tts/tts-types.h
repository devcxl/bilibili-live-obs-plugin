#pragma once

#include <QString>
#include <QByteArray>
#include <cstdint>

enum class TtsOutputChannel : uint32_t {
    None    = 0,
    Device  = 1 << 0, // 本机硬件音频设备播放（主播耳机/音箱）
    Obs     = 1 << 1, // OBS 混音器推流源（直播间观众）
    Both    = Device | Obs
};

struct TtsRoutingRules {
    bool danmaku = true;   // 普通弹幕
    bool gift = true;      // 礼物送礼
    bool sc = true;        // SC 醒目留言
    bool guard = true;     // 大航海上舰
    bool entry = true;     // 进房欢迎
    bool like = false;     // 点赞感谢
};

enum class TtsPriority {
    Like = 0,     // 点赞（最低优先级）
    Entry = 1,    // 进房欢迎
    Normal = 2,   // 普通弹幕
    Gift = 3,     // 礼物消息
    SuperChat = 4,// SC 醒目留言
    Guard = 5     // 大航海开通/上舰（最高优先级）
};

enum class TtsEntryFilter {
    All = 0,       // 全部观众进房
    WithMedal = 1, // 仅佩戴粉丝勋章的观众
    GuardOnly = 2  // 仅大航海（舰长/提督/总督）
};

struct TtsMessage {
    uint64_t id = 0;
    TtsPriority priority = TtsPriority::Normal;
    uint32_t target_channels = static_cast<uint32_t>(TtsOutputChannel::Both);
    QString text;
    QString sender;
    int64_t timestamp_ms = 0;

    // 比较运算符：用于优先队列排序（SC > Gift > Normal；同优先级时间先者先播）
    bool operator<(const TtsMessage &other) const {
        if (priority != other.priority) {
            return static_cast<int>(priority) < static_cast<int>(other.priority);
        }
        return timestamp_ms > other.timestamp_ms;
    }
};

struct TtsConfig {
    bool enabled = false;
    QString key;
    QString region = "eastasia";
    QString voice = "zh-CN-XiaoxiaoNeural";
    QString rate = "+0%";
    QString pitch = "+0%";
    int volume = 100; // 0 ~ 100
    TtsEntryFilter entry_filter = TtsEntryFilter::All;
    bool merge_enabled = true;

    // 通道 A：本机物理设备
    bool device_enabled = true;
    QString audio_device_id;
    int device_volume = 80; // 0 ~ 100，主播耳机/设备独立音量
    TtsRoutingRules to_device;

    // 通道 B：OBS 混音推流源
    bool obs_enabled = true;
    TtsRoutingRules to_obs;

    QString format = "raw-24khz-16bit-mono-pcm";

    uint32_t calculate_channels(TtsPriority priority) const {
        if (!enabled) return static_cast<uint32_t>(TtsOutputChannel::None);
        uint32_t mask = 0;
        auto match = [](const TtsRoutingRules &r, TtsPriority p) {
            switch (p) {
                case TtsPriority::Normal: return r.danmaku;
                case TtsPriority::Gift: return r.gift;
                case TtsPriority::SuperChat: return r.sc;
                case TtsPriority::Guard: return r.guard;
                case TtsPriority::Entry: return r.entry;
                case TtsPriority::Like: return r.like;
                default: return false;
            }
        };
        if (device_enabled && match(to_device, priority)) {
            mask |= static_cast<uint32_t>(TtsOutputChannel::Device);
        }
        if (obs_enabled && match(to_obs, priority)) {
            mask |= static_cast<uint32_t>(TtsOutputChannel::Obs);
        }
        return mask;
    }

    int sample_rate() const {
        if (format.contains("16khz")) return 16000;
        if (format.contains("24khz")) return 24000;
        if (format.contains("48khz")) return 48000;
        return 24000;
    }
};

struct TtsMetrics {
    qint64 ttfb_ms = 0;
    qint64 total_request_ms = 0;
    qint64 pcm_bytes = 0;
    double audio_duration_sec = 0.0;
    double rtf = 0.0;
    int http_status = 0;
    QString error_string;
};
