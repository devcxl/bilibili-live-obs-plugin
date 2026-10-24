#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTimer>

#include "danmaku-ws.h"
#include "config-manager.h"
#include "support/fake_network_access_manager.h"
#include "support/local_ws_server.h"

class TestDanmakuWsFailover : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void testClusterFailoverOnConnectionDrop();
    void testExplicitAuthRejectDoesNotRotateClusters();

private:
    std::unique_ptr<QTemporaryDir> temp_dir_;
    std::unique_ptr<ConfigManager> cfg_;
};

void TestDanmakuWsFailover::init()
{
    temp_dir_ = std::make_unique<QTemporaryDir>();
    setenv("BILIBILI_LIVE_OBS_CONFIG_HOME", temp_dir_->path().toUtf8().constData(), 1);
    cfg_ = std::make_unique<ConfigManager>();
    cfg_->danmaku.open_live_app_id = 5005;
    cfg_->danmaku.open_live_access_key = "ak_failover";
    cfg_->danmaku.open_live_secret = "sec_failover";
    cfg_->danmaku.open_live_code = "code_failover";
}

void TestDanmakuWsFailover::cleanup()
{
    cfg_.reset();
    temp_dir_.reset();
}

void TestDanmakuWsFailover::testClusterFailoverOnConnectionDrop()
{
    LocalWsServer serverA(LocalWsServer::AuthMode::DropBeforeAuth);
    QVERIFY(serverA.listen());

    LocalWsServer serverB(LocalWsServer::AuthMode::Accept);
    QVERIFY(serverB.listen());

    FakeNetworkAccessManager fake_nam;
    fake_nam.set_handler([&serverA, &serverB](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            QByteArray resp = QString(R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_failover_1"},
                    "anchor_info": {"room_id": 8888, "uname": "Anchor"},
                    "websocket_info": {
                        "auth_body": "{\"auth\":\"ok\"}",
                        "wss_link": ["%1", "%2"]
                    }
                }
            })").arg(serverA.url().toString(), serverB.url().toString()).toUtf8();

            QTimer::singleShot(0, [reply = req.reply, resp]() {
                reply->finish(resp);
            });
        }
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    ws.connect_to_room("8888");

    // Server A drops connection -> client switches cluster to server B on reconnect
    // With reconnect base delay 2000ms, it should connect to B within ~4-5s
    QTRY_COMPARE_WITH_TIMEOUT(ws.state(), DanmakuWebSocket::State::Connected, 6000);
    QCOMPARE(serverA.connection_count(), 1);
    QCOMPARE(serverB.connection_count(), 1);
    QCOMPARE(serverB.received_auth_bodies().size(), static_cast<size_t>(1));

    ws.disconnect_from_room();
}

void TestDanmakuWsFailover::testExplicitAuthRejectDoesNotRotateClusters()
{
    LocalWsServer serverA(LocalWsServer::AuthMode::Reject);
    QVERIFY(serverA.listen());

    LocalWsServer serverB(LocalWsServer::AuthMode::Accept);
    QVERIFY(serverB.listen());

    FakeNetworkAccessManager fake_nam;
    int start_count = 0;
    int end_count = 0;

    fake_nam.set_handler([&serverA, &serverB, &start_count, &end_count](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            ++start_count;
            QByteArray resp = QString(R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_reject_test"},
                    "anchor_info": {"room_id": 7777, "uname": "Anchor"},
                    "websocket_info": {
                        "auth_body": "{\"auth\":\"bad\"}",
                        "wss_link": ["%1", "%2"]
                    }
                }
            })").arg(serverA.url().toString(), serverB.url().toString()).toUtf8();

            QTimer::singleShot(0, [reply = req.reply, resp]() {
                reply->finish(resp);
            });
        } else if (req.request.url().path() == "/v2/app/end") {
            ++end_count;
            QTimer::singleShot(0, [reply = req.reply]() {
                reply->finish(R"({"code":0})");
            });
        }
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    ws.connect_to_room("7777");

    // Server A explicitly rejects auth
    // DanmakuWebSocket must release the game (call end_app) and NOT try server B
    QTRY_VERIFY_WITH_TIMEOUT(serverA.connection_count() >= 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(end_count, 1, 3000);

    // Verify Server B was never attempted for this game
    QCOMPARE(serverB.connection_count(), 0);

    ws.disconnect_from_room();
}

QTEST_MAIN(TestDanmakuWsFailover)
#include "test_danmaku_ws_failover.moc"
