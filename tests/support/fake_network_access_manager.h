#pragma once

#include <functional>
#include <vector>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>

#include "fake_network_reply.h"

struct RecordedRequest {
    QNetworkAccessManager::Operation op;
    QNetworkRequest request;
    QByteArray body;
    FakeNetworkReply *reply = nullptr;
};

class FakeNetworkAccessManager : public QNetworkAccessManager {
    Q_OBJECT
public:
    using RequestHandler = std::function<void(RecordedRequest &req)>;

    explicit FakeNetworkAccessManager(QObject *parent = nullptr);

    const std::vector<RecordedRequest> &requests() const { return requests_; }
    void clear_requests() { requests_.clear(); }
    void set_handler(RequestHandler handler) { handler_ = std::move(handler); }

protected:
    QNetworkReply *createRequest(Operation op,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData) override;

private:
    std::vector<RecordedRequest> requests_;
    RequestHandler handler_;
};
