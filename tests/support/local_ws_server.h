#pragma once

#include <vector>
#include <QByteArray>
#include <QObject>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

class LocalWsServer : public QObject {
    Q_OBJECT
public:
    enum class AuthMode {
        Accept,
        Reject,
        DropBeforeAuth,
        Hang
    };

    explicit LocalWsServer(AuthMode mode = AuthMode::Accept, QObject *parent = nullptr);
    ~LocalWsServer() override;

    bool listen();
    QUrl url() const;
    void set_auth_mode(AuthMode mode) { mode_ = mode; }
    int connection_count() const { return connection_count_; }
    const std::vector<QByteArray> &received_auth_bodies() const { return received_auth_bodies_; }
    void close_all();

private slots:
    void on_new_connection();
    void on_binary_message(const QByteArray &data);

private:
    QWebSocketServer *server_ = nullptr;
    AuthMode mode_ = AuthMode::Accept;
    int connection_count_ = 0;
    std::vector<QByteArray> received_auth_bodies_;
    std::vector<QWebSocket *> clients_;
};
