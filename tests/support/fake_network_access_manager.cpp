#include "fake_network_access_manager.h"
#include <QIODevice>

FakeNetworkAccessManager::FakeNetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
}

QNetworkReply *FakeNetworkAccessManager::createRequest(Operation op,
                                                       const QNetworkRequest &request,
                                                       QIODevice *outgoingData)
{
    auto *reply = new FakeNetworkReply(this);
    RecordedRequest rec;
    rec.op = op;
    rec.request = request;
    if (outgoingData) {
        rec.body = outgoingData->readAll();
    }
    rec.reply = reply;
    requests_.push_back(rec);

    if (handler_) {
        handler_(requests_.back());
    }

    return reply;
}
