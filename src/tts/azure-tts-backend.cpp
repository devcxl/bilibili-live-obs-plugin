#include "azure-tts-backend.h"
#include <QNetworkRequest>
#include <QUrl>
#include <obs-module.h>

AzureTtsBackend::AzureTtsBackend(const TtsConfig &config, QObject *parent)
    : ITtsBackend(parent), config_(config)
{
}

AzureTtsBackend::~AzureTtsBackend()
{
    cancel();
}

void AzureTtsBackend::update_config(const TtsConfig &config)
{
    config_ = config;
}

QString AzureTtsBackend::escape_xml(const QString &input)
{
    QString escaped = input;
    escaped.replace("&", "&amp;");
    escaped.replace("<", "&lt;");
    escaped.replace(">", "&gt;");
    escaped.replace("\"", "&quot;");
    escaped.replace("'", "&apos;");
    return escaped;
}

QString AzureTtsBackend::build_ssml(const QString &text, const QString &voice,
                                   const QString &rate, const QString &pitch)
{
    QString v = voice.trimmed().isEmpty() ? "zh-CN-XiaoxiaoNeural" : voice.trimmed();
    return QString(
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='zh-CN'>\n"
        "  <voice name='%1'>\n"
        "    <prosody rate='%2' pitch='%3'>\n"
        "      %4\n"
        "    </prosody>\n"
        "  </voice>\n"
        "</speak>"
    ).arg(v, rate, pitch, escape_xml(text));
}

void AzureTtsBackend::cancel()
{
    if (current_reply_) {
        current_reply_->abort();
        current_reply_->deleteLater();
        current_reply_ = nullptr;
    }
}

void AzureTtsBackend::synthesize(const QString &text)
{
    cancel();

    if (config_.key.trimmed().isEmpty()) {
        blog(LOG_WARNING, "[TTS] Azure Subscription Key 为空，无法发起合成");
        emit error_occurred("Azure Subscription Key 为空", 0);
        emit finished(false, QByteArray(), TtsMetrics{0, 0, 0, 0.0, 0.0, 0, "Empty API Key"});
        return;
    }

    QString region = config_.region.trimmed().isEmpty() ? "eastasia" : config_.region.trimmed();
    QString endpoint_url = QString("https://%1.tts.speech.microsoft.com/cognitiveservices/v1").arg(region);

    QNetworkRequest request{QUrl(endpoint_url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/ssml+xml");
    request.setRawHeader("Ocp-Apim-Subscription-Key", config_.key.trimmed().toUtf8());
    request.setRawHeader("X-Microsoft-OutputFormat", config_.format.toUtf8());
    request.setRawHeader("User-Agent", "bilibili-live-obs-plugin/1.0");

    QString ssml = build_ssml(text, config_.voice, config_.rate, config_.pitch);
    QByteArray ssml_bytes = ssml.toUtf8();

    pcm_buffer_.clear();
    first_chunk_received_ = false;
    ttfb_ms_ = 0;
    timer_.start();

    current_reply_ = net_mgr_.post(request, ssml_bytes);

    connect(current_reply_, &QNetworkReply::readyRead, this, &AzureTtsBackend::on_ready_read);
    connect(current_reply_, &QNetworkReply::finished, this, &AzureTtsBackend::on_reply_finished);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(current_reply_, &QNetworkReply::errorOccurred, this, &AzureTtsBackend::on_reply_error);
#endif
}

void AzureTtsBackend::on_ready_read()
{
    if (!current_reply_) return;

    if (!first_chunk_received_) {
        first_chunk_received_ = true;
        ttfb_ms_ = timer_.elapsed();
    }

    QByteArray chunk = current_reply_->readAll();
    pcm_buffer_.append(chunk);
    emit chunk_received(chunk);
}

void AzureTtsBackend::on_reply_error(QNetworkReply::NetworkError code)
{
    Q_UNUSED(code);
    if (!current_reply_) return;

    int status = current_reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString err_str = current_reply_->errorString();
    blog(LOG_WARNING, "[TTS] 网络请求错误: %s (HTTP %d)", err_str.toUtf8().constData(), status);
    emit error_occurred(err_str, status);
}

void AzureTtsBackend::on_reply_finished()
{
    if (!current_reply_) return;

    qint64 total_time_ms = timer_.elapsed();
    int status = current_reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool success = (current_reply_->error() == QNetworkReply::NoError && status == 200);

    QByteArray last_chunk;
    if (current_reply_->isOpen() && current_reply_->isReadable()) {
        last_chunk = current_reply_->readAll();
    }
    if (!last_chunk.isEmpty()) {
        pcm_buffer_.append(last_chunk);
        if (success) {
            emit chunk_received(last_chunk);
        }
    }

    QString error_detail;
    if (!success) {
        error_detail = QString::fromUtf8(pcm_buffer_);
        if (error_detail.trimmed().isEmpty()) {
            error_detail = current_reply_->errorString();
        }
        blog(LOG_WARNING, "[TTS] 合成失败: %s (HTTP %d)", error_detail.toUtf8().constData(), status);
    }

    TtsMetrics metrics;
    metrics.ttfb_ms = first_chunk_received_ ? ttfb_ms_ : total_time_ms;
    metrics.total_request_ms = total_time_ms;
    metrics.pcm_bytes = success ? pcm_buffer_.size() : 0;
    metrics.http_status = status;
    metrics.error_string = error_detail;

    int bytes_per_sec = config_.sample_rate() * 2;
    if (bytes_per_sec > 0 && success) {
        metrics.audio_duration_sec = static_cast<double>(metrics.pcm_bytes) / bytes_per_sec;
    }
    if (metrics.audio_duration_sec > 0.001) {
        metrics.rtf = (metrics.total_request_ms / 1000.0) / metrics.audio_duration_sec;
    }

    current_reply_->deleteLater();
    current_reply_ = nullptr;

    emit finished(success, success ? pcm_buffer_ : QByteArray(), metrics);
}
