#pragma once

#include <QObject>
#include <deque>
#include <memory>
#include "tts-types.h"
#include "tts-backend.h"
#include "pcm-audio-sink.h"
#include "obs-audio-source.h"
#include "../danmaku-ws.h"

class TtsManager : public QObject {
    Q_OBJECT

public:
    explicit TtsManager(QObject *parent = nullptr);
    ~TtsManager() override;

    void update_config(const TtsConfig &config);
    const TtsConfig &config() const { return config_; }

    void set_enabled(bool enabled);
    bool is_enabled() const { return config_.enabled; }

    // 立即停止当前播放并清空排队队列
    void stop_and_clear();

    // 队列大小与状态查询
    size_t queue_size() const { return queue_.size(); }
    bool is_busy() const;

    // 单独测试/试听文本
    void test_speak(const QString &text);

public slots:
    // 连接到 DanmakuWebSocket 的信号
    void on_danmaku_received(const DanmakuMessage &msg);
    void on_gift_received(const GiftMessage &msg);
    void on_super_chat_received(const SuperChatMessage &msg);
    void on_guard_received(const GuardMessage &msg);
    void on_like_received(const LikeMessage &msg);
    void on_entry_received(const EntryMessage &msg);

signals:
    void queue_size_changed(size_t size);
    void tts_state_changed(bool is_speaking);
    void error_occurred(const QString &err);

private slots:
    void on_backend_finished(bool success, const QByteArray &pcm_data, const TtsMetrics &metrics);
    void on_channel_playback_finished(TtsOutputChannel channel);
    void process_queue();

private:
    void enqueue_message(const TtsMessage &msg);

    TtsConfig config_;
    std::unique_ptr<ITtsBackend> backend_;
    std::unique_ptr<PcmAudioSink> audio_sink_;
    std::unique_ptr<ObsAudioSourceSink> obs_sink_;

    std::deque<TtsMessage> queue_;
    static constexpr size_t MAX_QUEUE_SIZE = 1000;

    bool in_flight_ = false; // 是否有正在进行的网络合成请求
    uint32_t current_msg_channels_ = 0;
    uint32_t active_playback_channels_ = 0;
    uint64_t next_id_ = 1;
};
