#include "pcm-audio-sink.h"
#include <obs-module.h>
#include <algorithm>

PcmAudioSink::PcmAudioSink(int sample_rate, QObject *parent)
    : QObject(parent), sample_rate_(sample_rate)
{
    setup_audio_sink();
}

PcmAudioSink::~PcmAudioSink()
{
    stop();
}

void PcmAudioSink::setup_audio_sink()
{
    stop();

    QAudioFormat format;
    format.setSampleRate(sample_rate_);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice target_device = QMediaDevices::defaultAudioOutput();
    if (!device_id_.isEmpty()) {
        const auto devices = QMediaDevices::audioOutputs();
        bool found = false;
        for (const auto &dev : devices) {
            if (QString::fromUtf8(dev.id()) == device_id_) {
                target_device = dev;
                found = true;
                break;
            }
        }
        if (!found) {
            blog(LOG_WARNING, "[TTS] 指定的音频输出设备未找到，已回退至系统默认设备: %s",
                 device_id_.toUtf8().constData());
        }
    }

    if (target_device.isNull()) {
        blog(LOG_WARNING, "[TTS] 未检测到系统音频输出设备");
        return;
    }

    audio_sink_ = std::make_unique<QAudioSink>(target_device, format, this);
    audio_sink_->setVolume(volume_);

    connect(audio_sink_.get(), &QAudioSink::stateChanged, this, &PcmAudioSink::on_state_changed);
}

void PcmAudioSink::set_device_id(const QString &device_id)
{
    if (device_id_ != device_id) {
        device_id_ = device_id;
        setup_audio_sink();
    }
}

void PcmAudioSink::set_sample_rate(int sample_rate)
{
    if (sample_rate_ != sample_rate) {
        sample_rate_ = sample_rate;
        setup_audio_sink();
    }
}

void PcmAudioSink::set_volume(float volume)
{
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (audio_sink_) {
        audio_sink_->setVolume(volume_);
    }
}

void PcmAudioSink::play(const QByteArray &pcm_data)
{
    stop();

    if (pcm_data.isEmpty()) {
        emit playback_finished();
        return;
    }

    if (!audio_sink_) {
        setup_audio_sink();
        if (!audio_sink_) {
            emit error_occurred("音频输出设备初始化失败");
            return;
        }
    }

    pcm_data_ = pcm_data;
    buffer_ = std::make_unique<QBuffer>(&pcm_data_);
    if (!buffer_->open(QIODevice::ReadOnly)) {
        emit error_occurred("无法打开音频内存缓冲区");
        return;
    }

    emit playback_started();
    audio_sink_->start(buffer_.get());
}

void PcmAudioSink::stop()
{
    if (audio_sink_) {
        audio_sink_->stop();
    }
    if (buffer_) {
        buffer_->close();
        buffer_.reset();
    }
    pcm_data_.clear();
}

bool PcmAudioSink::is_playing() const
{
    if (!audio_sink_) return false;
    return audio_sink_->state() == QAudio::ActiveState;
}

void PcmAudioSink::on_state_changed(QAudio::State state)
{
    if (state == QAudio::IdleState) {
        stop();
        emit playback_finished();
    } else if (state == QAudio::StoppedState) {
        if (audio_sink_ && audio_sink_->error() != QAudio::NoError) {
            blog(LOG_WARNING, "[TTS] QAudioSink 播放异常，代码: %d", static_cast<int>(audio_sink_->error()));
        }
    }
}
