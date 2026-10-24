#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTimer>
#include <QPointer>

#include "danmaku-ws.h"
#include "config-manager.h"
#include "support/fake_network_access_manager.h"
#include "support/local_ws_server.h"

class TestDanmakuWsLifecycle : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void testIncompleteCredentials();
    void testSuccessfulConnectAndAuth();
    void testStartAppError7001Cooldown();
    void testDisconnectWhileStartPendingSendsEndAppOnStaleSuccess();
    void testSessionSnapshotProtectsEndAppFromConfigMutation();
    void testDestructorReturnsImmediatelyWithoutBlocking();

private:
    std::unique_ptr<QTemporaryDir> temp_dir_;
    std::unique_ptr<ConfigManager> cfg_;
};

void TestDanmakuWsLifecycle::init()
{
    temp_dir_ = std::make_unique<QTemporaryDir>();
    setenv("BILIBILI_LIVE_OBS_CONFIG_HOME", temp_dir_->path().toUtf8().constData(), 1);
    cfg_ = std::make_unique<ConfigManager>();
}

void TestDanmakuWsLifecycle::cleanup()
{
    cfg_.reset();
    temp_dir_.reset();
}

void TestDanmakuWsLifecycle::testIncompleteCredentials()
{
    DanmakuWebSocket ws;
    ws.set_config(cfg_.get()); // Default cfg has empty danmaku credentials

    FakeNetworkAccessManager fake_nam;
    ws.set_network_access_manager(&fake_nam);

    QSignalSpy state_spy(&ws, &DanmakuWebSocket::connection_state_changed);
    ws.connect_to_room("12345");

    QCOMPARE(fake_nam.requests().size(), static_cast<size_t>(0));
    QCOMPARE(ws.state(), DanmakuWebSocket::State::Closed);
}

void TestDanmakuWsLifecycle::testSuccessfulConnectAndAuth()
{
    cfg_->danmaku.open_live_app_id = 1001;
    cfg_->danmaku.open_live_access_key = "ak_1001";
    cfg_->danmaku.open_live_secret = "sec_1001";
    cfg_->danmaku.open_live_code = "code_1001";

    LocalWsServer ws_server(LocalWsServer::AuthMode::Accept);
    QVERIFY(ws_server.listen());

    FakeNetworkAccessManager fake_nam;
    fake_nam.set_handler([&ws_server](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            QByteArray resp = QString(R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_test_1"},
                    "anchor_info": {"room_id": 12345, "uname": "Anchor"},
                    "websocket_info": {
                        "auth_body": "{\"test_auth\":1}",
                        "wss_link": ["%1"]
                    }
                }
            })").arg(ws_server.url().toString()).toUtf8();

            QPointer<FakeNetworkReply> p = req.reply;
            QTimer::singleShot(0, [p, resp]() {
                if (p && !p->isFinished()) p->finish(resp);
            });
        }
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    QSignalSpy state_spy(&ws, &DanmakuWebSocket::connection_state_changed);
    ws.connect_to_room("12345");

    QTRY_COMPARE_WITH_TIMEOUT(ws.state(), DanmakuWebSocket::State::Connected, 3000);
    QVERIFY(ws.is_connected());
    QCOMPARE(ws_server.connection_count(), 1);
    QCOMPARE(ws_server.received_auth_bodies().size(), static_cast<size_t>(1));
    QCOMPARE(ws_server.received_auth_bodies()[0], QByteArray("{\"test_auth\":1}"));

    ws.disconnect_from_room();
    QCOMPARE(ws.state(), DanmakuWebSocket::State::Closed);
}

void TestDanmakuWsLifecycle::testStartAppError7001Cooldown()
{
    cfg_->danmaku.open_live_app_id = 1001;
    cfg_->danmaku.open_live_access_key = "ak";
    cfg_->danmaku.open_live_secret = "sk";
    cfg_->danmaku.open_live_code = "code";

    FakeNetworkAccessManager fake_nam;
    fake_nam.set_handler([](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            QByteArray resp = R"({"code": 7001, "message": "冷却中"})";
            QPointer<FakeNetworkReply> p = req.reply;
            QTimer::singleShot(0, [p, resp]() {
                if (p && !p->isFinished()) p->finish(resp);
            });
        }
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    ws.connect_to_room("12345");

    // After 7001, reconnect timer is started with 60000 ms
    QTimer *reconnect_timer = ws.findChild<QTimer *>("reconnect_timer");
    QVERIFY(reconnect_timer != nullptr);
    QTRY_VERIFY(reconnect_timer->isActive());
    QCOMPARE(reconnect_timer->interval(), 60000);
}

void TestDanmakuWsLifecycle::testDisconnectWhileStartPendingSendsEndAppOnStaleSuccess()
{
    cfg_->danmaku.open_live_app_id = 1001;
    cfg_->danmaku.open_live_access_key = "ak_original";
    cfg_->danmaku.open_live_secret = "sec_original";
    cfg_->danmaku.open_live_code = "code_original";

    FakeNetworkAccessManager fake_nam;
    FakeNetworkReply *start_reply = nullptr;

    fake_nam.set_handler([&start_reply](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            start_reply = req.reply;
        } else if (req.request.url().path() == "/v2/app/end") {
            QPointer<FakeNetworkReply> p = req.reply;
            QTimer::singleShot(0, [p]() {
                if (p && !p->isFinished()) {
                    p->finish(R"({"code":0,"message":"ok"})");
                }
            });
        }
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    ws.connect_to_room("12345");
    QVERIFY(start_reply != nullptr);

    // User immediately closes interaction before start_app returned
    ws.disconnect_from_room();
    QCOMPARE(ws.state(), DanmakuWebSocket::State::Closed);

    // Now server returns success for the in-flight start_app
    QByteArray start_resp = R"({
        "code": 0,
        "data": {
            "game_info": {"game_id": "game_stale_999"},
            "anchor_info": {"room_id": 12345, "uname": "Anchor"},
            "websocket_info": {
                "auth_body": "{}",
                "wss_link": ["ws://127.0.0.1:9999"]
            }
        }
    })";
    start_reply->finish(start_resp);

    // DanmakuWebSocket must detect that this success is stale, and immediately post_end_app
    QTRY_VERIFY_WITH_TIMEOUT(fake_nam.requests().size() >= 2, 1000);
    const auto &end_req = fake_nam.requests().back();
    QCOMPARE(end_req.request.url().path(), QStringLiteral("/v2/app/end"));
    auto end_j = nlohmann::json::parse(end_req.body.toStdString());
    QCOMPARE(end_j["game_id"].get<std::string>(), std::string("game_stale_999"));
    QCOMPARE(end_j["app_id"].get<int64_t>(), 1001);
    QCOMPARE(end_req.request.rawHeader("x-bili-accesskeyid"), QByteArray("ak_original"));

    QTest::qWait(50);
}

void TestDanmakuWsLifecycle::testSessionSnapshotProtectsEndAppFromConfigMutation()
{
    cfg_->danmaku.open_live_app_id = 2002;
    cfg_->danmaku.open_live_access_key = "ak_session";
    cfg_->danmaku.open_live_secret = "sec_session";
    cfg_->danmaku.open_live_code = "code_session";

    FakeNetworkAccessManager fake_nam;
    fake_nam.set_handler([](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            QByteArray resp = R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_snap_1"},
                    "anchor_info": {"room_id": 12345, "uname": "Anchor"},
                    "websocket_info": {
                        "auth_body": "{}",
                        "wss_link": []
                    }
                }
            })";
            QPointer<FakeNetworkReply> p = req.reply;
            QTimer::singleShot(0, [p, resp]() {
                if (p && !p->isFinished()) p->finish(resp);
            });
        } else if (req.request.url().path() == "/v2/app/end") {
            QPointer<FakeNetworkReply> p = req.reply;
            QTimer::singleShot(0, [p]() {
                if (p && !p->isFinished()) p->finish(R"({"code":0})");
            });
        }
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    ws.connect_to_room("12345");

    // Empty wss_link triggers release_open_project which calls post_end_app
    // Even if config is mutated right after:
    cfg_->danmaku.open_live_access_key = "MUTATED_AK";
    cfg_->danmaku.open_live_app_id = 999999;

    QTRY_VERIFY_WITH_TIMEOUT(fake_nam.requests().size() >= 2, 1000);
    const auto &end_req = fake_nam.requests()[1];
    QCOMPARE(end_req.request.url().path(), QStringLiteral("/v2/app/end"));
    QCOMPARE(end_req.request.rawHeader("x-bili-accesskeyid"), QByteArray("ak_session"));
    auto end_j = nlohmann::json::parse(end_req.body.toStdString());
    QCOMPARE(end_j["app_id"].get<int64_t>(), 2002);

    QTest::qWait(50);
}

void TestDanmakuWsLifecycle::testDestructorReturnsImmediatelyWithoutBlocking()
{
    cfg_->danmaku.open_live_app_id = 3003;
    cfg_->danmaku.open_live_access_key = "ak";
    cfg_->danmaku.open_live_secret = "sk";
    cfg_->danmaku.open_live_code = "code";

    FakeNetworkAccessManager fake_nam;
    // Don't finish start_app reply (simulate hanging network)

    QElapsedTimer timer;
    timer.start();

    {
        DanmakuWebSocket ws;
        ws.set_config(cfg_.get());
        ws.set_network_access_manager(&fake_nam);
        ws.connect_to_room("12345");
        // ws destructs here while start_app is still pending
    }

    qint64 elapsed = timer.elapsed();
    // Destructor must NOT block 1500ms or 10000ms
    QVERIFY2(elapsed < 200, qPrintable(QString("Destructor took %1 ms, expected < 200 ms").arg(elapsed)));
}

QTEST_MAIN(TestDanmakuWsLifecycle)
#include "test_danmaku_ws_lifecycle.moc"
