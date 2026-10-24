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
    void testDestructorAbortsPendingStartWithoutCallbacks();
    void testDestructorAbortsPendingEndWithoutCallbacks();

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

    auto ws = std::make_unique<DanmakuWebSocket>();
    ws->set_config(cfg_.get());
    ws->set_network_access_manager(&fake_nam);
    ws->connect_to_room("12345");
    QVERIFY(!fake_nam.requests().empty());

    QElapsedTimer timer;
    timer.start();
    ws.reset();   // destructs while start_app is still pending

    qint64 elapsed = timer.elapsed();
    // 退出路径不得阻塞：不等待 1500ms/10000ms 的网络时限，也不 join 线程。
    QVERIFY2(elapsed < 50, qPrintable(QString("Destructor took %1 ms, expected < 50 ms").arg(elapsed)));
}

// 退出时在途 start_app 必须被取消，且在 abort() 同步发 finished 之前已解绑回调，
// 保证收尾阶段不会重入正在析构对象上的业务逻辑。
void TestDanmakuWsLifecycle::testDestructorAbortsPendingStartWithoutCallbacks()
{
    cfg_->danmaku.open_live_app_id = 4004;
    cfg_->danmaku.open_live_access_key = "ak_start";
    cfg_->danmaku.open_live_secret = "sk_start";
    cfg_->danmaku.open_live_code = "code_start";

    FakeNetworkAccessManager fake_nam;
    FakeNetworkReply *start_reply = nullptr;
    fake_nam.set_handler([&start_reply](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            start_reply = req.reply;   // 保持挂起：模拟网络未返回
        }
    });

    auto ws = std::make_unique<DanmakuWebSocket>();
    ws->set_config(cfg_.get());
    ws->set_network_access_manager(&fake_nam);
    ws->connect_to_room("12345");
    QVERIFY(start_reply != nullptr);

    // 取消瞬间检查：start_app 的 finished 回调应已被 abort_reply 解绑。
    bool abort_hook_called = false;
    bool socket_connected_at_abort = true;
    start_reply->set_abort_hook([&, reply = start_reply, raw_ws = ws.get()]() {
        abort_hook_called = true;
        socket_connected_at_abort =
            QObject::disconnect(reply, &QNetworkReply::finished, raw_ws, nullptr);
    });

    QElapsedTimer timer;
    timer.start();
    ws.reset();

    QVERIFY2(timer.elapsed() < 50,
             qPrintable(QString("Shutdown took %1 ms, expected < 50 ms").arg(timer.elapsed())));
    QVERIFY(abort_hook_called);
    QVERIFY(start_reply->aborted());
    QVERIFY(start_reply->isFinished());
    QVERIFY2(!socket_connected_at_abort,
             "start_app finished callback was still bound when abort() fired");
}

// 退出时在途 end_app 同样必须解绑后取消；abort_all_end_replies 不得为已关闭对象
// 留下回调触发机会，也不得再发起新的网络请求。
void TestDanmakuWsLifecycle::testDestructorAbortsPendingEndWithoutCallbacks()
{
    cfg_->danmaku.open_live_app_id = 5005;
    cfg_->danmaku.open_live_access_key = "ak_end";
    cfg_->danmaku.open_live_secret = "sk_end";
    cfg_->danmaku.open_live_code = "code_end";

    FakeNetworkAccessManager fake_nam;
    FakeNetworkReply *end_reply = nullptr;
    fake_nam.set_handler([&end_reply](RecordedRequest &req) {
        if (req.request.url().path() == "/v2/app/start") {
            QByteArray resp = R"({
                "code": 0,
                "data": {
                    "game_info": {"game_id": "game_pending_end"},
                    "anchor_info": {"room_id": 12345, "uname": "Anchor"},
                    "websocket_info": {"auth_body": "{}", "wss_link": []}
                }
            })";
            QPointer<FakeNetworkReply> p = req.reply;
            QTimer::singleShot(0, [p, resp]() {
                if (p && !p->isFinished()) p->finish(resp);
            });
        } else if (req.request.url().path() == "/v2/app/end") {
            end_reply = req.reply;   // 保持挂起：模拟 end_app 未返回
        }
    });

    auto ws = std::make_unique<DanmakuWebSocket>();
    ws->set_config(cfg_.get());
    ws->set_network_access_manager(&fake_nam);
    ws->connect_to_room("12345");

    // 空 wss_link → 立即释放场次并发起 end_app
    QTRY_VERIFY_WITH_TIMEOUT(end_reply != nullptr, 1000);
    const size_t requests_before_shutdown = fake_nam.requests().size();

    bool abort_hook_called = false;
    bool socket_connected_at_abort = true;
    end_reply->set_abort_hook([&, reply = end_reply, raw_ws = ws.get()]() {
        abort_hook_called = true;
        socket_connected_at_abort =
            QObject::disconnect(reply, &QNetworkReply::finished, raw_ws, nullptr);
    });

    QElapsedTimer timer;
    timer.start();
    ws.reset();

    QVERIFY2(timer.elapsed() < 50,
             qPrintable(QString("Shutdown took %1 ms, expected < 50 ms").arg(timer.elapsed())));
    QVERIFY(abort_hook_called);
    QVERIFY(end_reply->aborted());
    QVERIFY(end_reply->isFinished());
    QVERIFY2(!socket_connected_at_abort,
             "end_app finished callback was still bound when abort() fired");
    // 退出路径不发起新请求（不再补发 end_app / start_app）
    QCOMPARE(fake_nam.requests().size(), requests_before_shutdown);
}

QTEST_MAIN(TestDanmakuWsLifecycle)
#include "test_danmaku_ws_lifecycle.moc"
