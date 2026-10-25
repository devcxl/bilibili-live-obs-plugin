#pragma once

#include <QObject>
#include <QByteArray>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QBuffer>
#include <memory>

class PcmAudioSink : public QObject {
    Q_OBJECT

public:
    explicit PcmAudioSink(int sample_rate = 24000, QObject *parent = nullptr);
    ~PcmAudioSink() override;

    void set_sample_rate(int sample_rate);
    int sample_rate() const { return sample_rate_; }

    void set_device_id(const QString &device_id);
    QString device_id() const { return device_id_; }

    void set_volume(float volume); // 0.0 ~ 1.0
    float volume() const { return volume_; }

    // 播放 PCM 数据
    void play(const QByteArray &pcm_data);

    // 立即停止 / 打断当前播放
    void stop();

    bool is_playing() const;

signals:
    void playback_started();
    void playback_finished();
    void error_occurred(const QString &err);

private slots:
    void on_state_changed(QAudio::State state);

private:
    void setup_audio_sink();

    int sample_rate_ = 24000;
    float volume_ = 1.0f;
    QString device_id_;
    std::unique_ptr<QAudioSink> audio_sink_;
    std::unique_ptr<QBuffer> buffer_;
    QByteArray pcm_data_;
};
