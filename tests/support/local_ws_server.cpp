#include "local_ws_server.h"
#include "danmaku/danmaku-codec.h"

LocalWsServer::LocalWsServer(AuthMode mode, QObject *parent)
    : QObject(parent), mode_(mode)
{
    server_ = new QWebSocketServer(QStringLiteral("FakeLocalWsServer"),
                                   QWebSocketServer::NonSecureMode,
                                   this);
    connect(server_, &QWebSocketServer::newConnection,
            this, &LocalWsServer::on_new_connection);
}

LocalWsServer::~LocalWsServer()
{
    close_all();
}

bool LocalWsServer::listen()
{
    return server_->listen(QHostAddress::LocalHost, 0);
}

QUrl LocalWsServer::url() const
{
    return server_->serverUrl();
}

void LocalWsServer::close_all()
{
    for (QWebSocket *client : clients_) {
        if (client) {
            client->close();
            client->deleteLater();
        }
    }
    clients_.clear();
    if (server_) server_->close();
}

void LocalWsServer::on_new_connection()
{
    while (server_->hasPendingConnections()) {
        QWebSocket *socket = server_->nextPendingConnection();
        if (!socket) continue;
        clients_.push_back(socket);
        ++connection_count_;

        if (mode_ == AuthMode::DropBeforeAuth) {
            socket->abort();
            continue;
        }

        connect(socket, &QWebSocket::binaryMessageReceived,
                this, &LocalWsServer::on_binary_message);
        connect(socket, &QWebSocket::disconnected, this, [this, socket]() {
            auto it = std::find(clients_.begin(), clients_.end(), socket);
            if (it != clients_.end()) clients_.erase(it);
            socket->deleteLater();
        });
    }
}

void LocalWsServer::on_binary_message(const QByteArray &data)
{
    auto *socket = qobject_cast<QWebSocket *>(sender());
    if (!socket) return;

    if (mode_ == AuthMode::Hang) {
        return;
    }

    auto packets = danmaku::DanmakuCodec::decode_packets(data);
    for (const auto &pkt : packets) {
        if (pkt.op == static_cast<uint32_t>(danmaku::OpCode::Auth)) {
            received_auth_bodies_.push_back(pkt.body);

            if (mode_ == AuthMode::Accept) {
                QByteArray reply_body = R"({"code":0})";
                QByteArray packet = danmaku::DanmakuCodec::encode_packet(
                    danmaku::OpCode::AuthReply,
                    danmaku::ProtoVer::Normal,
                    reply_body,
                    1
                );
                socket->sendBinaryMessage(packet);
            } else if (mode_ == AuthMode::Reject) {
                QByteArray reply_body = R"({"code":-1,"msg":"invalid auth"})";
                QByteArray packet = danmaku::DanmakuCodec::encode_packet(
                    danmaku::OpCode::AuthReply,
                    danmaku::ProtoVer::Normal,
                    reply_body,
                    1
                );
                socket->sendBinaryMessage(packet);
                socket->close();
            }
        }
    }
}
