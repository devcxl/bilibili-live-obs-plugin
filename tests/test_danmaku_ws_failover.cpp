#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTimer>
#include <QPointer>

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
    void testProjectHeartbeatIndependence();
    void testHandshakeTimeoutTriggersClusterFailover();

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

// 项目心跳按「官方场次」计时：20s 固定节拍；WSS 断线重连既不重启也不改变周期。
void TestDanmakuWsFailover::testProjectHeartbeatIndependence()
{
    LocalWsServer serverA(LocalWsServer::AuthMode::Accept);
    QVERIFY(serverA.listen());

    LocalWsServer serverB(LocalWsServer::AuthMode::Accept);
    QVERIFY(serverB.listen());

    FakeNetworkAccessManager fake_nam;
    fake_nam.set_handler([&serverA, &serverB](RecordedRequest &req) {
        const QString path = req.request.url().path();
        QByteArray resp;
        if (path == "/v2/app/start") {
            resp = QString(R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_heartbeat_1"},
                    "anchor_info": {"room_id": 8888, "uname": "Anchor"},
                    "websocket_info": {
                        "auth_body": "{\"auth\":\"ok\"}",
                        "wss_link": ["%1", "%2"]
                    }
                }
            })").arg(serverA.url().toString(), serverB.url().toString()).toUtf8();
        } else {
            resp = R"({"code":0})";
        }
        QPointer<FakeNetworkReply> p = req.reply;
        QTimer::singleShot(0, [p, resp]() {
            if (p && !p->isFinished()) p->finish(resp);
        });
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    QTimer *open_heartbeat_timer = ws.findChild<QTimer *>("open_heartbeat_timer");
    QVERIFY(open_heartbeat_timer != nullptr);
    QVERIFY(!open_heartbeat_timer->isActive());
    QVERIFY(!open_heartbeat_timer->isSingleShot());

    ws.connect_to_room("8888");
    QTRY_COMPARE_WITH_TIMEOUT(ws.state(), DanmakuWebSocket::State::Connected, 4000);
    QCOMPARE(serverA.connection_count(), 1);

    // start_app 成功后启动 20s 周期定时器
    QVERIFY(open_heartbeat_timer->isActive());
    QCOMPARE(open_heartbeat_timer->interval(), 20000);
    QVERIFY(!open_heartbeat_timer->isSingleShot());

    // 让它跑一段，记录剩余时间，用于证明重连没有重启定时器
    QTest::qWait(1000);
    const int remaining_before = open_heartbeat_timer->remainingTime();
    QVERIFY(remaining_before > 0 && remaining_before < 20000);

    // 对端断开 -> 自动重连（同一链接失败后切换集群）
    serverA.close_all();
    QTRY_VERIFY_WITH_TIMEOUT(serverB.connection_count() >= 1, 12000);
    QTRY_COMPARE_WITH_TIMEOUT(ws.state(), DanmakuWebSocket::State::Connected, 4000);

    // 重连过程中定时器未被重启：剩余时间只会继续减少
    QVERIFY(open_heartbeat_timer->isActive());
    QCOMPARE(open_heartbeat_timer->interval(), 20000);
    const int remaining_after = open_heartbeat_timer->remainingTime();
    QVERIFY2(remaining_after > 0,
             qPrintable(QString("heartbeat timer stopped on reconnect (remaining=%1)")
                            .arg(remaining_after)));
    QVERIFY2(remaining_after < remaining_before,
             qPrintable(QString("heartbeat timer was restarted on reconnect (%1 -> %2)")
                            .arg(remaining_before).arg(remaining_after)));

    ws.disconnect_from_room();
    QVERIFY(!open_heartbeat_timer->isActive());
}

// 握手+鉴权超时属于传输层失败：中止当前连接并切换集群（与显式拒绝区分）。
void TestDanmakuWsFailover::testHandshakeTimeoutTriggersClusterFailover()
{
    LocalWsServer serverA(LocalWsServer::AuthMode::Hang);
    QVERIFY(serverA.listen());

    LocalWsServer serverB(LocalWsServer::AuthMode::Accept);
    QVERIFY(serverB.listen());

    FakeNetworkAccessManager fake_nam;
    fake_nam.set_handler([&serverA, &serverB](RecordedRequest &req) {
        const QString path = req.request.url().path();
        QByteArray resp;
        if (path == "/v2/app/start") {
            resp = QString(R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_timeout_1"},
                    "anchor_info": {"room_id": 6666, "uname": "Anchor"},
                    "websocket_info": {
                        "auth_body": "{\"auth\":\"hang\"}",
                        "wss_link": ["%1", "%2"]
                    }
                }
            })").arg(serverA.url().toString(), serverB.url().toString()).toUtf8();
        } else {
            resp = R"({"code":0})";
        }
        QPointer<FakeNetworkReply> p = req.reply;
        QTimer::singleShot(0, [p, resp]() {
            if (p && !p->isFinished()) p->finish(resp);
        });
    });

    DanmakuWebSocket ws;
    ws.set_config(cfg_.get());
    ws.set_network_access_manager(&fake_nam);

    ws.connect_to_room("6666");

    // Server A 接受连接但不回 AuthReply；客户端已发出鉴权包
    QTRY_VERIFY_WITH_TIMEOUT(serverA.connection_count() >= 1, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(ws.state(), DanmakuWebSocket::State::Connecting, 4000);
    QCOMPARE(serverA.received_auth_bodies().size(), static_cast<size_t>(1));

    // 触发握手/鉴权超时
    QVERIFY(QMetaObject::invokeMethod(&ws, "on_auth_timeout", Qt::DirectConnection));

    // 当前连接被中止，进入重连计划（尚未 Connected）
    QCOMPARE(ws.state(), DanmakuWebSocket::State::Connecting);

    // 重连时跳过已冷却的 A，切到 Server B
    QTRY_COMPARE_WITH_TIMEOUT(ws.state(), DanmakuWebSocket::State::Connected, 8000);
    QCOMPARE(serverA.connection_count(), 1);   // A 处于冷却期，未被热重试
    QCOMPARE(serverB.connection_count(), 1);
    QCOMPARE(serverB.received_auth_bodies().size(), static_cast<size_t>(1));

    ws.disconnect_from_room();
}

QTEST_MAIN(TestDanmakuWsFailover)
#include "test_danmaku_ws_failover.moc"
