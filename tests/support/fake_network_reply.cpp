#include "fake_network_reply.h"
#include <cstring>

FakeNetworkReply::FakeNetworkReply(QObject *parent)
    : QNetworkReply(parent)
{
    open(QIODevice::ReadOnly);
}

void FakeNetworkReply::abort()
{
    if (isFinished()) return;
    aborted_ = true;
    // 在同步 finished 之前回调：调用方应先 disconnect 再 abort，避免重入
    // 正在析构/关闭的对象。
    if (abort_hook_) abort_hook_();
    setError(QNetworkReply::OperationCanceledError, "Operation canceled");
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 0);
    setFinished(true);
    emit errorOccurred(QNetworkReply::OperationCanceledError);
    emit finished();
}

qint64 FakeNetworkReply::bytesAvailable() const
{
    return (buffer_.size() - offset_) + QIODevice::bytesAvailable();
}

qint64 FakeNetworkReply::readData(char *data, qint64 maxlen)
{
    qint64 available = buffer_.size() - offset_;
    qint64 toRead = qMin(maxlen, available);
    if (toRead <= 0) return 0;
    std::memcpy(data, buffer_.constData() + offset_, static_cast<size_t>(toRead));
    offset_ += toRead;
    return toRead;
}

void FakeNetworkReply::finish(const QByteArray &data,
                              QNetworkReply::NetworkError err,
                              int httpStatus)
{
    if (isFinished()) return;
    buffer_ = data;
    offset_ = 0;
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, httpStatus);
    if (err != QNetworkReply::NoError) {
        setError(err, "Fake error");
        emit errorOccurred(err);
    }
    setFinished(true);
    emit finished();
}
