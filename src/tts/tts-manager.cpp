#include "tts-manager.h"
#include "azure-tts-backend.h"
#include "tts-cleaner.h"
#include <obs-module.h>
#include <QDateTime>
#include <algorithm>

TtsManager::TtsManager(QObject *parent)
    : QObject(parent)
{
    backend_ = std::make_unique<AzureTtsBackend>(config_, this);
    audio_sink_ = std::make_unique<PcmAudioSink>(config_.sample_rate(), this);
    obs_sink_ = std::make_unique<ObsAudioSourceSink>(this);

    connect(backend_.get(), &ITtsBackend::finished, this, &TtsManager::on_backend_finished);
    connect(backend_.get(), &ITtsBackend::error_occurred, this, [this](const QString &err, int status) {
        emit error_occurred(QString("%1 (HTTP %2)").arg(err).arg(status));
    });

    connect(audio_sink_.get(), &PcmAudioSink::playback_started, this, [this]() {
        emit tts_state_changed(true);
    });
    connect(audio_sink_.get(), &PcmAudioSink::playback_finished, this, [this]() {
        on_channel_playback_finished(TtsOutputChannel::Device);
    });

    connect(obs_sink_.get(), &ObsAudioSourceSink::playback_started, this, [this]() {
        emit tts_state_changed(true);
    });
    connect(obs_sink_.get(), &ObsAudioSourceSink::playback_finished, this, [this]() {
        on_channel_playback_finished(TtsOutputChannel::Obs);
    });
}

TtsManager::~TtsManager()
{
    stop_and_clear();
}

bool TtsManager::is_busy() const
{
    bool sink_playing = audio_sink_ && audio_sink_->is_playing();
    bool obs_playing = obs_sink_ && obs_sink_->is_playing();
    return in_flight_ || sink_playing || obs_playing;
}

void TtsManager::update_config(const TtsConfig &config)
{
    config_ = config;
    if (backend_) {
        backend_->update_config(config_);
    }
    if (audio_sink_) {
        audio_sink_->set_sample_rate(config_.sample_rate());
        audio_sink_->set_device_id(config_.audio_device_id);
        audio_sink_->set_volume(config_.volume / 100.0f);
    }
    if (!config_.enabled) {
        stop_and_clear();
    }
}

void TtsManager::set_enabled(bool enabled)
{
    config_.enabled = enabled;
    if (!enabled) {
        stop_and_clear();
    }
}

void TtsManager::stop_and_clear()
{
    if (backend_) {
        backend_->cancel();
    }
    if (audio_sink_) {
        audio_sink_->stop();
    }
    if (obs_sink_) {
        obs_sink_->stop();
    }
    in_flight_ = false;
    current_msg_channels_ = 0;
    active_playback_channels_ = 0;
    queue_.clear();
    emit queue_size_changed(0);
    emit tts_state_changed(false);
}

void TtsManager::test_speak(const QString &text)
{
    if (text.trimmed().isEmpty()) return;

    uint32_t channels = config_.calculate_channels(TtsPriority::SuperChat);
    if (channels == 0) {
        // 若两路均未勾选，默认试听两路或开启的一路
        channels = static_cast<uint32_t>(TtsOutputChannel::Both);
    }

    TtsMessage msg;
    msg.id = next_id_++;
    msg.priority = TtsPriority::SuperChat; // 试听使用最高优先级
    msg.target_channels = channels;
    msg.text = text.trimmed();
    msg.sender = "系统测试";
    msg.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(msg);
}

void TtsManager::on_danmaku_received(const DanmakuMessage &msg)
{
    if (!config_.enabled) return;
    uint32_t channels = config_.calculate_channels(TtsPriority::Normal);
    if (channels == 0) return;

    QString cleaned = TtsCleaner::clean_danmaku(
        QString::fromStdString(msg.username),
        QString::fromStdString(msg.message)
    );
    if (cleaned.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Normal;
    item.target_channels = channels;
    item.text = cleaned;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_gift_received(const GiftMessage &msg)
{
    if (!config_.enabled) return;
    uint32_t channels = config_.calculate_channels(TtsPriority::Gift);
    if (channels == 0) return;

    QString formatted = TtsCleaner::format_gift(
        QString::fromStdString(msg.username),
        QString::fromStdString(msg.action),
        QString::fromStdString(msg.gift_name),
        msg.num,
        msg.combo_num
    );
    if (formatted.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Gift;
    item.target_channels = channels;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_super_chat_received(const SuperChatMessage &msg)
{
    if (!config_.enabled) return;
    uint32_t channels = config_.calculate_channels(TtsPriority::SuperChat);
    if (channels == 0) return;

    QString formatted = TtsCleaner::format_super_chat(
        QString::fromStdString(msg.username),
        msg.price,
        QString::fromStdString(msg.message)
    );
    if (formatted.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::SuperChat;
    item.target_channels = channels;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_guard_received(const GuardMessage &msg)
{
    if (!config_.enabled) return;
    uint32_t channels = config_.calculate_channels(TtsPriority::Guard);
    if (channels == 0) return;

    QString formatted = TtsCleaner::format_guard(
        QString::fromStdString(msg.username),
        msg.guard_level,
        msg.guard_num,
        QString::fromStdString(msg.guard_unit)
    );
    if (formatted.isEmpty()) return;

    blog(LOG_INFO, "[TTS-Guard] 大航海播报入队: user=%s guard=%d text=%s",
         msg.username.c_str(), msg.guard_level, formatted.toUtf8().constData());

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Guard;
    item.target_channels = channels;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_like_received(const LikeMessage &msg)
{
    if (!config_.enabled) return;
    uint32_t channels = config_.calculate_channels(TtsPriority::Like);
    if (channels == 0) return;

    QString formatted = TtsCleaner::format_like(
        QString::fromStdString(msg.username),
        msg.like_count
    );
    if (formatted.isEmpty()) return;

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Like;
    item.target_channels = channels;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::on_entry_received(const EntryMessage &msg)
{
    if (!config_.enabled) {
        blog(LOG_INFO, "[TTS-Entry] 忽略进房: TTS 功能未开启");
        return;
    }
    uint32_t channels = config_.calculate_channels(TtsPriority::Entry);
    if (channels == 0) {
        return;
    }

    // 过滤范围判断
    if (config_.entry_filter == TtsEntryFilter::GuardOnly) {
        if (msg.guard_level <= 0) {
            blog(LOG_INFO, "[TTS-Entry] 忽略进房: 当前设置仅大航海播报，用户 %s 不是大航海", msg.username.c_str());
            return;
        }
    } else if (config_.entry_filter == TtsEntryFilter::WithMedal) {
        if (msg.medal_name.empty() && msg.guard_level <= 0) {
            blog(LOG_INFO, "[TTS-Entry] 忽略进房: 当前设置仅勋章粉丝播报，用户 %s 无勋章", msg.username.c_str());
            return;
        }
    }

    QString formatted = TtsCleaner::format_entry(
        QString::fromStdString(msg.username),
        msg.guard_level,
        QString::fromStdString(msg.medal_name)
    );
    if (formatted.isEmpty()) return;

    blog(LOG_INFO, "[TTS-Entry] 进房播报入队: user=%s guard=%d text=%s",
         msg.username.c_str(), msg.guard_level, formatted.toUtf8().constData());

    TtsMessage item;
    item.id = next_id_++;
    item.priority = TtsPriority::Entry;
    item.target_channels = channels;
    item.text = formatted;
    item.sender = QString::fromStdString(msg.username);
    item.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    enqueue_message(item);
}

void TtsManager::enqueue_message(const TtsMessage &msg)
{
    if (queue_.size() >= MAX_QUEUE_SIZE) {
        // 队列达到 1000 上限时：优先淘汰最低优先级的项（Like -> Entry -> Normal）
        bool removed = false;
        for (int p = static_cast<int>(TtsPriority::Like); p <= static_cast<int>(TtsPriority::Normal); ++p) {
            for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
                if (static_cast<int>(it->priority) == p) {
                    queue_.erase(std::next(it).base());
                    removed = true;
                    break;
                }
            }
            if (removed) break;
        }
        if (!removed) {
            queue_.pop_front();
        }
    }

    // 按优先级插入：Guard (5) > SuperChat (4) > Gift (3) > Normal (2) > Entry (1) > Like (0)
    auto it = std::find_if(queue_.begin(), queue_.end(), [&](const TtsMessage &m) {
        return m.priority < msg.priority;
    });
    queue_.insert(it, msg);

    emit queue_size_changed(queue_.size());
    process_queue();
}

void TtsManager::process_queue()
{
    if (!config_.enabled) return;
    if (in_flight_ || is_busy()) return;
    if (queue_.empty()) {
        emit tts_state_changed(false);
        return;
    }

    // 取出待合成内容
    TtsMessage first = queue_.front();
    queue_.pop_front();

    current_msg_channels_ = first.target_channels;
    QString text_to_speak;

    if (config_.merge_enabled && !queue_.empty()) {
        if (first.priority == TtsPriority::Normal) {
            // 合并连续普通弹幕（最多 4 条，总长度 ≤ 100 字）
            std::vector<TtsMessage> batch;
            batch.push_back(first);
            while (!queue_.empty() && queue_.front().priority == TtsPriority::Normal && batch.size() < 4) {
                current_msg_channels_ |= queue_.front().target_channels;
                batch.push_back(queue_.front());
                queue_.pop_front();
            }
            text_to_speak = TtsCleaner::merge_messages(batch, 100);
        } else if (first.priority == TtsPriority::Entry) {
            // 合并连续进房消息（最多 3 条）
            std::vector<TtsMessage> batch;
            batch.push_back(first);
            while (!queue_.empty() && queue_.front().priority == TtsPriority::Entry && batch.size() < 3) {
                current_msg_channels_ |= queue_.front().target_channels;
                batch.push_back(queue_.front());
                queue_.pop_front();
            }
            text_to_speak = TtsCleaner::merge_messages(batch, 80);
        } else if (first.priority == TtsPriority::Like) {
            // 合并连续点赞消息（最多 4 条）
            std::vector<TtsMessage> batch;
            batch.push_back(first);
            while (!queue_.empty() && queue_.front().priority == TtsPriority::Like && batch.size() < 4) {
                current_msg_channels_ |= queue_.front().target_channels;
                batch.push_back(queue_.front());
                queue_.pop_front();
            }
            text_to_speak = TtsCleaner::merge_messages(batch, 60);
        } else {
            text_to_speak = first.text;
        }
    } else {
        text_to_speak = first.text;
    }

    emit queue_size_changed(queue_.size());

    if (text_to_speak.trimmed().isEmpty()) {
        // 若为空，继续下一轮
        process_queue();
        return;
    }

    in_flight_ = true;
    backend_->synthesize(text_to_speak);
}

void TtsManager::on_backend_finished(bool success, const QByteArray &pcm_data, const TtsMetrics &metrics)
{
    in_flight_ = false;

    if (success && !pcm_data.isEmpty()) {
        uint32_t to_play = current_msg_channels_;
        if (to_play == 0) {
            to_play = static_cast<uint32_t>(TtsOutputChannel::Both);
        }

        active_playback_channels_ = 0;
        if ((to_play & static_cast<uint32_t>(TtsOutputChannel::Device)) && config_.device_enabled) {
            active_playback_channels_ |= static_cast<uint32_t>(TtsOutputChannel::Device);
            audio_sink_->play(pcm_data);
        }
        if ((to_play & static_cast<uint32_t>(TtsOutputChannel::Obs)) && config_.obs_enabled) {
            active_playback_channels_ |= static_cast<uint32_t>(TtsOutputChannel::Obs);
            obs_sink_->play(pcm_data, config_.sample_rate());
        }

        if (active_playback_channels_ == 0) {
            // 未激活任何通道，继续下一项
            process_queue();
        }
    } else {
        blog(LOG_WARNING, "[TTS] 合成未产生有效音频，跳过并继续下一项: %s",
             metrics.error_string.toUtf8().constData());
        // 失败时继续调度下一项，防止队列停滞
        process_queue();
    }
}

void TtsManager::on_channel_playback_finished(TtsOutputChannel channel)
{
    active_playback_channels_ &= ~static_cast<uint32_t>(channel);
    if (active_playback_channels_ == 0) {
        process_queue();
    }
}
