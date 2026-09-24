#include "azure_tts_client.h"
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>

AzureTtsClient::AzureTtsClient(const AzureTtsConfig &config, QObject *parent)
    : QObject(parent), config_(config)
{
}

void AzureTtsClient::update_config(const AzureTtsConfig &config)
{
    config_ = config;
}

QString AzureTtsClient::escape_xml(const QString &input)
{
    QString escaped = input;
    escaped.replace("&", "&amp;");
    escaped.replace("<", "&lt;");
    escaped.replace(">", "&gt;");
    escaped.replace("\"", "&quot;");
    escaped.replace("'", "&apos;");
    return escaped;
}

QString AzureTtsClient::build_ssml(const QString &text, const QString &voice,
                                   const QString &rate, const QString &pitch)
{
    return QString(
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='zh-CN'>\n"
        "  <voice name='%1'>\n"
        "    <prosody rate='%2' pitch='%3'>\n"
        "      %4\n"
        "    </prosody>\n"
        "  </voice>\n"
        "</speak>"
    ).arg(voice, rate, pitch, escape_xml(text));
}

void AzureTtsClient::cancel()
{
    if (current_reply_) {
        current_reply_->abort();
        current_reply_->deleteLater();
        current_reply_ = nullptr;
    }
}

void AzureTtsClient::synthesize(const QString &text)
{
    cancel();

    if (config_.key.trimmed().isEmpty()) {
        emit error_occurred("Azure Subscription Key 为空，请配置 Key 或使用 --mock 模式", 0);
        emit finished(false, QByteArray(), TtsMetrics{0, 0, 0, 0.0, 0.0, 0, "Empty API Key"});
        return;
    }

    if (config_.voice.trimmed().isEmpty()) {
        config_.voice = "zh-CN-XiaoxiaoNeural";
    }

    QString endpoint_url = QString("https://%1.tts.speech.microsoft.com/cognitiveservices/v1")
                               .arg(config_.region);

    QNetworkRequest request{QUrl(endpoint_url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/ssml+xml");
    request.setRawHeader("Ocp-Apim-Subscription-Key", config_.key.toUtf8());
    request.setRawHeader("X-Microsoft-OutputFormat", config_.format.toUtf8());
    request.setRawHeader("User-Agent", "bilibili-live-obs-plugin-poc/1.0");

    QString ssml = build_ssml(text, config_.voice, config_.rate, config_.pitch);
    QByteArray ssml_bytes = ssml.toUtf8();

    pcm_buffer_.clear();
    first_chunk_received_ = false;
    ttfb_ms_ = 0;
    timer_.start();

    current_reply_ = net_mgr_.post(request, ssml_bytes);

    connect(current_reply_, &QNetworkReply::readyRead, this, &AzureTtsClient::on_ready_read);
    connect(current_reply_, &QNetworkReply::finished, this, &AzureTtsClient::on_reply_finished);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(current_reply_, &QNetworkReply::errorOccurred, this, &AzureTtsClient::on_reply_error);
#endif
}

void AzureTtsClient::on_ready_read()
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

void AzureTtsClient::on_reply_error(QNetworkReply::NetworkError code)
{
    Q_UNUSED(code);
    if (!current_reply_) return;

    int status = current_reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString err_str = current_reply_->errorString();
    emit error_occurred(err_str, status);
}

void AzureTtsClient::on_reply_finished()
{
    if (!current_reply_) return;

    qint64 total_time_ms = timer_.elapsed();
    int status = current_reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool success = (current_reply_->error() == QNetworkReply::NoError && status == 200);

    // 读完最后剩余的数据
    QByteArray last_chunk = current_reply_->readAll();
    if (!last_chunk.isEmpty()) {
        pcm_buffer_.append(last_chunk);
        if (success) {
            emit chunk_received(last_chunk);
        }
    }

    QString error_detail;
    if (!success) {
        // HTTP 错误时，响应体通常包含 Azure 详细的错误诊断信息
        error_detail = QString::fromUtf8(pcm_buffer_);
        if (error_detail.trimmed().isEmpty()) {
            error_detail = current_reply_->errorString();
        }
    }

    TtsMetrics metrics;
    metrics.ttfb_ms = first_chunk_received_ ? ttfb_ms_ : total_time_ms;
    metrics.total_request_ms = total_time_ms;
    metrics.pcm_bytes = success ? pcm_buffer_.size() : 0;
    metrics.http_status = status;
    metrics.error_string = error_detail;

    // 计算音频理论时长：16-bit signed mono = 2 bytes per sample
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
