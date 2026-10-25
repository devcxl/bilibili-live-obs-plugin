#pragma once

#include <QObject>
#include <QByteArray>
#include <QTimer>
#include <vector>
#include <mutex>

typedef struct obs_source obs_source_t;

// 注册 OBS 原生 TTS 音频源类型（应在 obs_module_load 中调用）
void register_obs_tts_source();

// 管理当前活跃的 OBS TTS 音频源实例
class ObsAudioSourceRegistry {
public:
    static ObsAudioSourceRegistry &instance();

    void add_source(obs_source_t *source);
    void remove_source(obs_source_t *source);
    void set_active(obs_source_t *source, bool active);

    bool has_active_sources();
    std::vector<obs_source_t *> get_active_sources();

private:
    ObsAudioSourceRegistry() = default;
    std::mutex mutex_;
    struct SourceItem {
        obs_source_t *source = nullptr;
        bool active = false;
    };
    std::vector<SourceItem> sources_;
};

// 负责将合成的 PCM 音频以 20ms 匀速泵送给 OBS 音频引擎
class ObsAudioSourceSink : public QObject {
    Q_OBJECT

public:
    explicit ObsAudioSourceSink(QObject *parent = nullptr);
    ~ObsAudioSourceSink() override;

    void play(const QByteArray &pcm_data, int sample_rate);
    void stop();
    bool is_playing() const;

signals:
    void playback_started();
    void playback_finished();

private slots:
    void on_pump_tick();

private:
    QTimer timer_;
    QByteArray pcm_data_;
    int sample_rate_ = 24000;
    int offset_ = 0;
    uint64_t current_timestamp_ns_ = 0;
    uint64_t frame_interval_ns_ = 0;
    int chunk_bytes_ = 0;
    uint32_t chunk_frames_ = 0;
};
