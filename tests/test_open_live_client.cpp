#include <QtTest/QtTest>
#include "danmaku/open-live-client.h"
#include "support/fake_network_access_manager.h"
#include <nlohmann/json.hpp>

class TestOpenLiveClient : public QObject {
    Q_OBJECT
private slots:
    void testParseStartResponseSuccess();
    void testParseStartResponseError();
    void testParseStartResponseMalformed();
    void testParseAck();
    void testPostStartRequestStructure();
    void testPostHeartbeatAndEnd();
};

void TestOpenLiveClient::testParseStartResponseSuccess()
{
    QByteArray body = R"({
        "code": 0,
        "message": "ok",
        "data": {
            "game_info": {"game_id": "game_12345"},
            "anchor_info": {"room_id": 99999, "uname": "TestAnchor"},
            "websocket_info": {
                "auth_body": "{\"room_id\":99999,\"protover\":3}",
                "wss_link": [
                    "wss://broadcastlv.chat.bilibili.com/sub",
                    "wss://broadcastlv2.chat.bilibili.com/sub"
                ]
            }
        }
    })";

    auto res = danmaku::OpenLiveClient::parse_start_response(body);
    QVERIFY(res.ok);
    QCOMPARE(res.code, 0);
    QCOMPARE(res.game_id, std::string("game_12345"));
    QCOMPARE(res.room_id, 99999);
    QCOMPARE(res.anchor_name, std::string("TestAnchor"));
    QCOMPARE(res.auth_body, std::string("{\"room_id\":99999,\"protover\":3}"));
    QCOMPARE(res.wss_links.size(), static_cast<size_t>(2));
    QCOMPARE(res.wss_links[0], std::string("wss://broadcastlv.chat.bilibili.com/sub"));
    QCOMPARE(res.wss_links[1], std::string("wss://broadcastlv2.chat.bilibili.com/sub"));
}

void TestOpenLiveClient::testParseStartResponseError()
{
    QByteArray body = R"({
        "code": 7001,
        "message": "项目已存在或冷却中"
    })";

    auto res = danmaku::OpenLiveClient::parse_start_response(body);
    QVERIFY(!res.ok);
    QCOMPARE(res.code, 7001);
    QCOMPARE(res.msg, std::string("项目已存在或冷却中"));
    QVERIFY(res.game_id.empty());
    QVERIFY(res.wss_links.empty());
}

void TestOpenLiveClient::testParseStartResponseMalformed()
{
    QByteArray body = "not a valid json {";
    auto res = danmaku::OpenLiveClient::parse_start_response(body);
    QVERIFY(!res.ok);
    QCOMPARE(res.code, -1);
}

void TestOpenLiveClient::testParseAck()
{
    // Success ack
    {
        QByteArray body = R"({"code": 0, "message": "success"})";
        auto ack = danmaku::OpenLiveClient::parse_ack(body);
        QVERIFY(ack.ok);
        QCOMPARE(ack.code, 0);
        QCOMPARE(ack.msg, std::string("success"));
    }
    // Rejected ack
    {
        QByteArray body = R"({"code": 5000, "msg": "game not found"})";
        auto ack = danmaku::OpenLiveClient::parse_ack(body);
        QVERIFY(!ack.ok);
        QCOMPARE(ack.code, 5000);
        QCOMPARE(ack.msg, std::string("game not found"));
    }
    // Malformed ack
    {
        QByteArray body = "invalid";
        auto ack = danmaku::OpenLiveClient::parse_ack(body);
        QVERIFY(!ack.ok);
        QCOMPARE(ack.code, -1);
    }
}

void TestOpenLiveClient::testPostStartRequestStructure()
{
    FakeNetworkAccessManager nam;
    danmaku::OpenLiveCredentials creds;
    creds.app_id = 123456;
    creds.access_key = "test_ak";
    creds.access_secret = "test_sk";
    creds.code = "test_identity_code";

    QNetworkReply *reply = danmaku::OpenLiveClient::post_start(nam, creds);
    QVERIFY(reply != nullptr);
    QCOMPARE(nam.requests().size(), static_cast<size_t>(1));

    const auto &req = nam.requests()[0];
    QCOMPARE(req.request.url().toString(), QStringLiteral("https://live-open.biliapi.com/v2/app/start"));
    QCOMPARE(req.request.header(QNetworkRequest::ContentTypeHeader).toString(), QStringLiteral("application/json"));
    QVERIFY(req.request.hasRawHeader("x-bili-accesskeyid"));
    QCOMPARE(req.request.rawHeader("x-bili-accesskeyid"), QByteArray("test_ak"));
    QVERIFY(req.request.hasRawHeader("x-bili-content-md5"));
    QVERIFY(req.request.hasRawHeader("Authorization"));

    auto j = nlohmann::json::parse(req.body.toStdString(), nullptr, false);
    QVERIFY(!j.is_discarded());
    QCOMPARE(j["app_id"].get<int64_t>(), 123456);
    QCOMPARE(j["code"].get<std::string>(), std::string("test_identity_code"));
}

void TestOpenLiveClient::testPostHeartbeatAndEnd()
{
    FakeNetworkAccessManager nam;
    danmaku::OpenLiveCredentials creds;
    creds.app_id = 888;
    creds.access_key = "ak";
    creds.access_secret = "sk";

    danmaku::OpenLiveClient::post_heartbeat(nam, "game_001", creds);
    danmaku::OpenLiveClient::post_end(nam, "game_001", creds);

    QCOMPARE(nam.requests().size(), static_cast<size_t>(2));

    QCOMPARE(nam.requests()[0].request.url().toString(), QStringLiteral("https://live-open.biliapi.com/v2/app/heartbeat"));
    auto hb_json = nlohmann::json::parse(nam.requests()[0].body.toStdString());
    QCOMPARE(hb_json["game_id"].get<std::string>(), std::string("game_001"));

    QCOMPARE(nam.requests()[1].request.url().toString(), QStringLiteral("https://live-open.biliapi.com/v2/app/end"));
    auto end_json = nlohmann::json::parse(nam.requests()[1].body.toStdString());
    QCOMPARE(end_json["game_id"].get<std::string>(), std::string("game_001"));
    QCOMPARE(end_json["app_id"].get<int64_t>(), 888);
}

QTEST_MAIN(TestOpenLiveClient)
#include "test_open_live_client.moc"
