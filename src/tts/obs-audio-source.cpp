#include "obs-audio-source.h"
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>

struct TtsSourceContext {
    obs_source_t *source = nullptr;
};

static const char *tts_source_get_name(void *)
{
    return "B站弹幕机语音 (Danmaku TTS)";
}

static void *tts_source_create(obs_data_t *, obs_source_t *source)
{
    auto *ctx = new TtsSourceContext();
    ctx->source = source;
    ObsAudioSourceRegistry::instance().add_source(source);
    blog(LOG_INFO, "[TTS-OBS] 创建 OBS 弹幕语音源实例: %p", source);
    return ctx;
}

static void tts_source_destroy(void *data)
{
    auto *ctx = static_cast<TtsSourceContext *>(data);
    if (!ctx) return;
    ObsAudioSourceRegistry::instance().remove_source(ctx->source);
    blog(LOG_INFO, "[TTS-OBS] 销毁 OBS 弹幕语音源实例: %p", ctx->source);
    delete ctx;
}

static void tts_source_activate(void *data)
{
    auto *ctx = static_cast<TtsSourceContext *>(data);
    if (ctx) {
        ObsAudioSourceRegistry::instance().set_active(ctx->source, true);
        blog(LOG_INFO, "[TTS-OBS] OBS 弹幕语音源已激活: %p", ctx->source);
    }
}

static void tts_source_deactivate(void *data)
{
    auto *ctx = static_cast<TtsSourceContext *>(data);
    if (ctx) {
        ObsAudioSourceRegistry::instance().set_active(ctx->source, false);
        blog(LOG_INFO, "[TTS-OBS] OBS 弹幕语音源已停用: %p", ctx->source);
    }
}

void register_obs_tts_source()
{
    static bool registered = false;
    if (registered) return;

    struct obs_source_info info = {};
    info.id = "bilibili_live_tts_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = tts_source_get_name;
    info.create = tts_source_create;
    info.destroy = tts_source_destroy;
    info.activate = tts_source_activate;
    info.deactivate = tts_source_deactivate;

    obs_register_source(&info);
    registered = true;
    blog(LOG_INFO, "[TTS-OBS] 成功向 OBS 注册 bilibili_live_tts_source 音频源");
}

ObsAudioSourceRegistry &ObsAudioSourceRegistry::instance()
{
    static ObsAudioSourceRegistry reg;
    return reg;
}

void ObsAudioSourceRegistry::add_source(obs_source_t *source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &item : sources_) {
        if (item.source == source) return;
    }
    sources_.push_back({source, false});
}

void ObsAudioSourceRegistry::remove_source(obs_source_t *source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.erase(
        std::remove_if(sources_.begin(), sources_.end(),
                       [source](const SourceItem &item) { return item.source == source; }),
        sources_.end()
    );
}

void ObsAudioSourceRegistry::set_active(obs_source_t *source, bool active)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &item : sources_) {
        if (item.source == source) {
            item.active = active;
            break;
        }
    }
}

bool ObsAudioSourceRegistry::has_active_sources()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &item : sources_) {
        if (item.active) return true;
    }
    return false;
}

std::vector<obs_source_t *> ObsAudioSourceRegistry::get_active_sources()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<obs_source_t *> result;
    for (const auto &item : sources_) {
        if (item.active && item.source) {
            result.push_back(item.source);
        }
    }
    return result;
}

// ---------------- ObsAudioSourceSink ----------------

ObsAudioSourceSink::ObsAudioSourceSink(QObject *parent)
    : QObject(parent)
{
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &ObsAudioSourceSink::on_pump_tick);
}

ObsAudioSourceSink::~ObsAudioSourceSink()
{
    stop();
}

void ObsAudioSourceSink::play(const QByteArray &pcm_data, int sample_rate)
{
    stop();

    if (pcm_data.isEmpty()) {
        emit playback_finished();
        return;
    }

    pcm_data_ = pcm_data;
    sample_rate_ = sample_rate > 0 ? sample_rate : 24000;
    offset_ = 0;

    // 20ms 一帧
    chunk_frames_ = static_cast<uint32_t>(sample_rate_ * 20 / 1000);
    chunk_bytes_ = static_cast<int>(chunk_frames_ * sizeof(int16_t));
    frame_interval_ns_ = 20ULL * 1000000ULL;
    current_timestamp_ns_ = os_gettime_ns();

    emit playback_started();
    timer_.start(20);
}

void ObsAudioSourceSink::stop()
{
    if (timer_.isActive()) {
        timer_.stop();
    }
    pcm_data_.clear();
    offset_ = 0;
}

bool ObsAudioSourceSink::is_playing() const
{
    return timer_.isActive();
}

void ObsAudioSourceSink::on_pump_tick()
{
    if (offset_ >= pcm_data_.size()) {
        stop();
        emit playback_finished();
        return;
    }

    int remaining = pcm_data_.size() - offset_;
    int send_bytes = std::min(chunk_bytes_, remaining);
    uint32_t send_frames = static_cast<uint32_t>(send_bytes / sizeof(int16_t));

    if (send_frames > 0) {
        auto active_sources = ObsAudioSourceRegistry::instance().get_active_sources();
        if (!active_sources.empty()) {
            struct obs_source_audio audio = {};
            audio.speakers = SPEAKERS_MONO;
            audio.format = AUDIO_FORMAT_16BIT;
            audio.samples_per_sec = static_cast<uint32_t>(sample_rate_);
            audio.data[0] = reinterpret_cast<const uint8_t *>(pcm_data_.constData() + offset_);
            audio.frames = send_frames;
            audio.timestamp = current_timestamp_ns_;

            for (auto *source : active_sources) {
                obs_source_output_audio(source, &audio);
            }
        }
    }

    offset_ += send_bytes;
    current_timestamp_ns_ += frame_interval_ns_;

    if (offset_ >= pcm_data_.size()) {
        stop();
        emit playback_finished();
    }
}
