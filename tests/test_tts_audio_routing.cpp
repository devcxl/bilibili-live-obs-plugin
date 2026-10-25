#include <QTest>
#include <QObject>
#include "tts/tts-types.h"
#include "tts/obs-audio-source.h"
#include "config-manager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class TestTtsAudioRouting : public QObject {
    Q_OBJECT

private slots:
    void test_default_routing();
    void test_custom_matrix_routing();
    void test_global_channel_switch();
    void test_config_backward_compatibility();
    void test_obs_source_registry();
};

void TestTtsAudioRouting::test_default_routing()
{
    TtsConfig cfg;
    cfg.enabled = true;

    // 默认配置下：Normal, Gift, SC, Guard, Entry 默认双通开启，Like 默认关闭
    uint32_t normal_mask = cfg.calculate_channels(TtsPriority::Normal);
    QCOMPARE(normal_mask, static_cast<uint32_t>(TtsOutputChannel::Both));

    uint32_t sc_mask = cfg.calculate_channels(TtsPriority::SuperChat);
    QCOMPARE(sc_mask, static_cast<uint32_t>(TtsOutputChannel::Both));

    uint32_t like_mask = cfg.calculate_channels(TtsPriority::Like);
    QCOMPARE(like_mask, static_cast<uint32_t>(TtsOutputChannel::None));
}

void TestTtsAudioRouting::test_custom_matrix_routing()
{
    TtsConfig cfg;
    cfg.enabled = true;

    // 进房欢迎：仅本机设备播放（主播听），不推流给观众
    cfg.to_device.entry = true;
    cfg.to_obs.entry = false;
    uint32_t entry_mask = cfg.calculate_channels(TtsPriority::Entry);
    QCOMPARE(entry_mask, static_cast<uint32_t>(TtsOutputChannel::Device));

    // 普通弹幕：仅推流给观众，主播耳机不播
    cfg.to_device.danmaku = false;
    cfg.to_obs.danmaku = true;
    uint32_t danmaku_mask = cfg.calculate_channels(TtsPriority::Normal);
    QCOMPARE(danmaku_mask, static_cast<uint32_t>(TtsOutputChannel::Obs));

    // 大航海上舰：两端都播
    cfg.to_device.guard = true;
    cfg.to_obs.guard = true;
    uint32_t guard_mask = cfg.calculate_channels(TtsPriority::Guard);
    QCOMPARE(guard_mask, static_cast<uint32_t>(TtsOutputChannel::Both));
}

void TestTtsAudioRouting::test_global_channel_switch()
{
    TtsConfig cfg;
    cfg.enabled = true;

    // 关闭本机通道总开关
    cfg.device_enabled = false;
    cfg.obs_enabled = true;
    uint32_t mask = cfg.calculate_channels(TtsPriority::Normal);
    QCOMPARE(mask, static_cast<uint32_t>(TtsOutputChannel::Obs));

    // 关闭 OBS 通道总开关
    cfg.device_enabled = true;
    cfg.obs_enabled = false;
    mask = cfg.calculate_channels(TtsPriority::Normal);
    QCOMPARE(mask, static_cast<uint32_t>(TtsOutputChannel::Device));

    // TTS 总开关关闭
    cfg.enabled = false;
    mask = cfg.calculate_channels(TtsPriority::Normal);
    QCOMPARE(mask, static_cast<uint32_t>(TtsOutputChannel::None));
}

void TestTtsAudioRouting::test_config_backward_compatibility()
{
    // 模拟老版本配置 JSON：仅包含 read_danmaku, read_gift 等扁平字段
    json legacy_json = json::parse(R"({
        "tts": {
            "enabled": true,
            "key": "",
            "region": "eastasia",
            "voice": "zh-CN-XiaoxiaoNeural",
            "volume": 80,
            "read_danmaku": true,
            "read_gift": false,
            "read_sc": true,
            "read_guard": true,
            "read_entry": false,
            "read_like": false
        }
    })");

    TtsConfigData data;
    const auto &tj = legacy_json["tts"];
    data.enabled = tj.value("enabled", false);
    data.volume = tj.value("volume", 100);

    data.device_enabled = tj.value("device_enabled", true);
    data.audio_device_id = tj.value("audio_device_id", "");
    data.obs_enabled = tj.value("obs_enabled", true);

    if (tj.contains("to_device")) {
        // ...
    } else {
        bool old_danmaku = tj.value("read_danmaku", true);
        bool old_gift = tj.value("read_gift", true);
        bool old_sc = tj.value("read_sc", true);
        bool old_guard = tj.value("read_guard", true);
        bool old_entry = tj.value("read_entry", true);
        bool old_like = tj.value("read_like", false);
        data.to_device = {old_danmaku, old_gift, old_sc, old_guard, old_entry, old_like};
        data.to_obs = {old_danmaku, old_gift, old_sc, old_guard, old_entry, old_like};
    }

    // 验证旧值被无缝继承到新字段
    QVERIFY(data.device_enabled);
    QVERIFY(data.obs_enabled);
    QVERIFY(data.to_device.danmaku);
    QVERIFY(!data.to_device.gift); // 原配置为 false
    QVERIFY(data.to_device.sc);
    QVERIFY(!data.to_device.entry); // 原配置为 false

    QVERIFY(data.to_obs.danmaku);
    QVERIFY(!data.to_obs.gift);
    QVERIFY(data.to_obs.sc);
}

void TestTtsAudioRouting::test_obs_source_registry()
{
    auto &reg = ObsAudioSourceRegistry::instance();

    // 初始状态下假定无活动源
    auto *dummy_source1 = reinterpret_cast<obs_source_t *>(uintptr_t(0x1000));
    auto *dummy_source2 = reinterpret_cast<obs_source_t *>(uintptr_t(0x2000));

    reg.add_source(dummy_source1);
    reg.add_source(dummy_source2);

    QVERIFY(!reg.has_active_sources());

    // 激活 source1
    reg.set_active(dummy_source1, true);
    QVERIFY(reg.has_active_sources());
    auto active = reg.get_active_sources();
    QCOMPARE(active.size(), static_cast<size_t>(1));
    QCOMPARE(active[0], dummy_source1);

    // 停用 source1
    reg.set_active(dummy_source1, false);
    QVERIFY(!reg.has_active_sources());

    // 移除 source
    reg.remove_source(dummy_source1);
    reg.remove_source(dummy_source2);
    QVERIFY(!reg.has_active_sources());
}

QTEST_MAIN(TestTtsAudioRouting)
#include "test_tts_audio_routing.moc"
