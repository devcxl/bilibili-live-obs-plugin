#pragma once

#include <functional>

#include <QByteArray>
#include <QIODevice>
#include <QNetworkReply>

class FakeNetworkReply : public QNetworkReply {
    Q_OBJECT
public:
    explicit FakeNetworkReply(QObject *parent = nullptr);

    // 测试钩子：在 abort() 同步发出 finished 之前回调，便于检查取消瞬间的状态。
    using AbortHook = std::function<void()>;
    void set_abort_hook(AbortHook hook) { abort_hook_ = std::move(hook); }

    bool aborted() const { return aborted_; }

    void abort() override;
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }

    void finish(const QByteArray &data,
                QNetworkReply::NetworkError error = QNetworkReply::NoError,
                int httpStatus = 200);

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override { Q_UNUSED(data); Q_UNUSED(len); return -1; }

private:
    QByteArray buffer_;
    qint64 offset_ = 0;
    AbortHook abort_hook_;
    bool aborted_ = false;
};
