#include "tts-settings-dialog.h"
#include "obs-audio-source.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QMediaDevices>
#include <QAudioDevice>
#include <algorithm>

TtsSettingsDialog::TtsSettingsDialog(ConfigManager *cfg, TtsManager *tts_mgr, QWidget *parent)
    : QDialog(parent), cfg_(cfg), tts_mgr_(tts_mgr)
{
    setWindowTitle("弹幕语音播报 (TTS) 设置与多通道分流");
    setMinimumWidth(560);

    setup_ui();
    load_values();

    // 监听系统设备热插拔
    auto *mediaDevices = new QMediaDevices(this);
    connect(mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &TtsSettingsDialog::refresh_audio_devices);
}

void TtsSettingsDialog::setup_ui()
{
    auto *main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(12);

    // 1. 基础服务凭证与语音
    auto *grp_basic = new QGroupBox("Azure Speech 服务配置与音色");
    auto *form = new QFormLayout(grp_basic);

    auto *key_row = new QHBoxLayout();
    key_edit_ = new QLineEdit();
    key_edit_->setEchoMode(QLineEdit::Password);
    key_edit_->setPlaceholderText("填入 32 位 Azure Speech Key");
    auto *btn_toggle_echo = new QPushButton("👁");
    btn_toggle_echo->setFixedWidth(36);
    btn_toggle_echo->setToolTip("显示/隐藏 Key");
    connect(btn_toggle_echo, &QPushButton::clicked, [this]() {
        key_edit_->setEchoMode(key_edit_->echoMode() == QLineEdit::Password ? QLineEdit::Normal : QLineEdit::Password);
    });
    key_row->addWidget(key_edit_);
    key_row->addWidget(btn_toggle_echo);
    form->addRow("Azure Key:", key_row);

    region_combo_ = new QComboBox();
    region_combo_->setEditable(true);
    region_combo_->addItems({"eastasia", "southeastasia", "japaneast", "centralus", "eastus", "westus", "westeurope"});
    form->addRow("服务区域 (Region):", region_combo_);

    voice_combo_ = new QComboBox();
    struct VoiceItem { QString label; QString id; };
    std::vector<VoiceItem> voices = {
        {"晓晓 (活泼女声)", "zh-CN-XiaoxiaoNeural"},
        {"云希 (阳光男声)", "zh-CN-YunxiNeural"},
        {"云健 (沉稳男声)", "zh-CN-YunjianNeural"},
        {"晓伊 (温柔女声)", "zh-CN-XiaoyiNeural"},
        {"云夏 (少年男声)", "zh-CN-YunxiaNeural"},
        {"晓北 (东北话女声)", "zh-CN-liaoning-XiaobeiNeural"},
        {"晓妮 (陕西话女声)", "zh-CN-shaanxi-XiaoniNeural"},
        {"晓辰 (台湾国语)", "zh-TW-HsiaoChenNeural"},
        {"晓佳 (粤语女声)", "zh-HK-HiuGaaiNeural"}
    };
    for (const auto &v : voices) {
        voice_combo_->addItem(v.label, v.id);
    }
    form->addRow("音色选择 (Voice):", voice_combo_);

    auto *rate_layout = new QHBoxLayout();
    rate_slider_ = new QSlider(Qt::Horizontal);
    rate_slider_->setRange(-50, 50);
    rate_slider_->setSingleStep(5);
    rate_val_label_ = new QLabel("0%");
    rate_val_label_->setFixedWidth(45);
    connect(rate_slider_, &QSlider::valueChanged, [this](int val) {
        rate_val_label_->setText(QString("%1%").arg(val >= 0 ? QString("+%1").arg(val) : QString::number(val)));
    });
    rate_layout->addWidget(rate_slider_);
    rate_layout->addWidget(rate_val_label_);
    form->addRow("语速调节 (Rate):", rate_layout);

    auto *vol_layout = new QHBoxLayout();
    vol_slider_ = new QSlider(Qt::Horizontal);
    vol_slider_->setRange(0, 100);
    vol_slider_->setSingleStep(5);
    vol_val_label_ = new QLabel("100%");
    vol_val_label_->setFixedWidth(45);
    connect(vol_slider_, &QSlider::valueChanged, [this](int val) {
        vol_val_label_->setText(QString("%1%").arg(val));
    });
    vol_layout->addWidget(vol_slider_);
    vol_layout->addWidget(vol_val_label_);
    form->addRow("播报音量 (Volume):", vol_layout);
    main_layout->addWidget(grp_basic);

    // 2. 输出通道与设备设置
    auto *grp_channels = new QGroupBox("输出通道与硬件设备");
    auto *chan_layout = new QVBoxLayout(grp_channels);

    // 通道 A：本机设备
    auto *dev_row = new QHBoxLayout();
    chk_device_ = new QCheckBox("启用本机硬件播放 (主播耳机/音箱)");
    dev_row->addWidget(chk_device_);
    dev_row->addSpacing(12);
    combo_device_ = new QComboBox();
    combo_device_->setMinimumWidth(200);
    dev_row->addWidget(new QLabel("设备:"));
    dev_row->addWidget(combo_device_, 1);
    chan_layout->addLayout(dev_row);

    auto *dev_vol_row = new QHBoxLayout();
    dev_vol_row->addSpacing(24);
    dev_vol_row->addWidget(new QLabel("耳机/设备音量:"));
    dev_vol_slider_ = new QSlider(Qt::Horizontal);
    dev_vol_slider_->setRange(0, 100);
    dev_vol_slider_->setSingleStep(5);
    dev_vol_val_label_ = new QLabel("80%");
    dev_vol_val_label_->setFixedWidth(45);
    connect(dev_vol_slider_, &QSlider::valueChanged, [this](int val) {
        dev_vol_val_label_->setText(QString("%1%").arg(val));
    });
    dev_vol_row->addWidget(dev_vol_slider_);
    dev_vol_row->addWidget(dev_vol_val_label_);
    chan_layout->addLayout(dev_vol_row);

    // 通道 B：OBS 混音推流源
    auto *obs_row = new QHBoxLayout();
    chk_obs_ = new QCheckBox("启用 OBS 混音推流源 (独立推子，观众听到)");
    obs_status_label_ = new QLabel();
    bool has_active = ObsAudioSourceRegistry::instance().has_active_sources();
    if (has_active) {
        obs_status_label_->setText("<font color='green'>● OBS 源已在当前场景就绪</font>");
    } else {
        obs_status_label_->setText("<font color='gray'>提示: 请在 OBS 来源中添加“B站弹幕机语音”源</font>");
    }
    obs_row->addWidget(chk_obs_);
    obs_row->addSpacing(12);
    obs_row->addWidget(obs_status_label_, 1);
    chan_layout->addLayout(obs_row);

    main_layout->addWidget(grp_channels);

    // 3. 播报分流路由矩阵
    auto *grp_matrix = new QGroupBox("播报内容分流路由矩阵");
    auto *matrix_layout = new QGridLayout(grp_matrix);
    matrix_layout->setColumnStretch(0, 2);
    matrix_layout->setColumnStretch(1, 2);
    matrix_layout->setColumnStretch(2, 2);

    matrix_layout->addWidget(new QLabel("<b>事件类型</b>"), 0, 0);
    matrix_layout->addWidget(new QLabel("<b>本机设备播放 (主播)</b>"), 0, 1);
    matrix_layout->addWidget(new QLabel("<b>OBS 推流输出 (观众)</b>"), 0, 2);

    // 1. 普通弹幕
    matrix_layout->addWidget(new QLabel("普通弹幕"), 1, 0);
    chk_dev_danmaku_ = new QCheckBox("本机播放");
    chk_obs_danmaku_ = new QCheckBox("OBS 推流");
    matrix_layout->addWidget(chk_dev_danmaku_, 1, 1);
    matrix_layout->addWidget(chk_obs_danmaku_, 1, 2);

    // 2. 礼物送礼
    matrix_layout->addWidget(new QLabel("礼物送礼"), 2, 0);
    chk_dev_gift_ = new QCheckBox("本机播放");
    chk_obs_gift_ = new QCheckBox("OBS 推流");
    matrix_layout->addWidget(chk_dev_gift_, 2, 1);
    matrix_layout->addWidget(chk_obs_gift_, 2, 2);

    // 3. SC 醒目留言
    matrix_layout->addWidget(new QLabel("SC 醒目留言"), 3, 0);
    chk_dev_sc_ = new QCheckBox("本机播放");
    chk_obs_sc_ = new QCheckBox("OBS 推流");
    matrix_layout->addWidget(chk_dev_sc_, 3, 1);
    matrix_layout->addWidget(chk_obs_sc_, 3, 2);

    // 4. 大航海上舰
    matrix_layout->addWidget(new QLabel("大航海上舰/开通"), 4, 0);
    chk_dev_guard_ = new QCheckBox("本机播放");
    chk_obs_guard_ = new QCheckBox("OBS 推流");
    matrix_layout->addWidget(chk_dev_guard_, 4, 1);
    matrix_layout->addWidget(chk_obs_guard_, 4, 2);

    // 5. 进房欢迎 + 范围
    auto *entry_label_box = new QHBoxLayout();
    entry_label_box->addWidget(new QLabel("进房欢迎"));
    entry_filter_combo_ = new QComboBox();
    entry_filter_combo_->addItem("全部观众", 0);
    entry_filter_combo_->addItem("仅佩戴粉丝勋章", 1);
    entry_filter_combo_->addItem("仅大航海(舰长等)", 2);
    entry_label_box->addWidget(entry_filter_combo_);
    auto *entry_widget = new QWidget();
    entry_widget->setLayout(entry_label_box);
    matrix_layout->addWidget(entry_widget, 5, 0);

    chk_dev_entry_ = new QCheckBox("本机播放");
    chk_obs_entry_ = new QCheckBox("OBS 推流");
    matrix_layout->addWidget(chk_dev_entry_, 5, 1);
    matrix_layout->addWidget(chk_obs_entry_, 5, 2);

    // 6. 点赞感谢
    matrix_layout->addWidget(new QLabel("点赞感谢"), 6, 0);
    chk_dev_like_ = new QCheckBox("本机播放");
    chk_obs_like_ = new QCheckBox("OBS 推流");
    matrix_layout->addWidget(chk_dev_like_, 6, 1);
    matrix_layout->addWidget(chk_obs_like_, 6, 2);

    // 智能合并
    chk_merge_ = new QCheckBox("智能合并连续短弹幕/进房/点赞 (节省 Azure 额度与提升播报连贯度)");
    matrix_layout->addWidget(chk_merge_, 7, 0, 1, 3);

    main_layout->addWidget(grp_matrix);

    // 联动逻辑
    connect(chk_device_, &QCheckBox::toggled, [this](bool on) {
        combo_device_->setEnabled(on);
        dev_vol_slider_->setEnabled(on);
        chk_dev_danmaku_->setEnabled(on);
        chk_dev_gift_->setEnabled(on);
        chk_dev_sc_->setEnabled(on);
        chk_dev_guard_->setEnabled(on);
        chk_dev_entry_->setEnabled(on);
        chk_dev_like_->setEnabled(on);
    });
    connect(chk_obs_, &QCheckBox::toggled, [this](bool on) {
        chk_obs_danmaku_->setEnabled(on);
        chk_obs_gift_->setEnabled(on);
        chk_obs_sc_->setEnabled(on);
        chk_obs_guard_->setEnabled(on);
        chk_obs_entry_->setEnabled(on);
        chk_obs_like_->setEnabled(on);
    });

    // 4. 操作与测试按钮
    auto *act_row = new QHBoxLayout();
    auto *btn_test = new QPushButton("试听测试");
    auto *btn_clear = new QPushButton("停止并清空队列");
    connect(btn_test, &QPushButton::clicked, this, &TtsSettingsDialog::on_test_speak);
    connect(btn_clear, &QPushButton::clicked, this, &TtsSettingsDialog::on_clear_queue);
    act_row->addWidget(btn_test);
    act_row->addWidget(btn_clear);
    act_row->addStretch();
    main_layout->addLayout(act_row);

    auto *btn_box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(btn_box, &QDialogButtonBox::accepted, this, &TtsSettingsDialog::on_save);
    connect(btn_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    main_layout->addWidget(btn_box);
}

void TtsSettingsDialog::refresh_audio_devices()
{
    QString current_selected = combo_device_->currentData().toString();
    combo_device_->clear();
    combo_device_->addItem("系统默认输出设备", "");

    const auto devices = QMediaDevices::audioOutputs();
    int target_idx = 0;
    int idx = 1;
    for (const auto &dev : devices) {
        QString dev_id = QString::fromUtf8(dev.id());
        QString desc = dev.description();
        if (dev.isDefault()) {
            desc += " (系统默认)";
        }
        combo_device_->addItem(desc, dev_id);
        if (!current_selected.isEmpty() && dev_id == current_selected) {
            target_idx = idx;
        }
        idx++;
    }
    combo_device_->setCurrentIndex(target_idx);
}

void TtsSettingsDialog::load_values()
{
    if (!cfg_) return;

    key_edit_->setText(QString::fromStdString(cfg_->tts.key));

    int reg_idx = region_combo_->findText(QString::fromStdString(cfg_->tts.region));
    if (reg_idx >= 0) region_combo_->setCurrentIndex(reg_idx);
    else region_combo_->setEditText(QString::fromStdString(cfg_->tts.region));

    int voice_idx = voice_combo_->findData(QString::fromStdString(cfg_->tts.voice));
    if (voice_idx >= 0) voice_combo_->setCurrentIndex(voice_idx);

    int cur_rate = cfg_->tts.rate.empty() ? 0 : std::atoi(cfg_->tts.rate.c_str());
    rate_slider_->setValue(cur_rate);
    rate_val_label_->setText(QString("%1%").arg(cur_rate >= 0 ? QString("+%1").arg(cur_rate) : QString::number(cur_rate)));

    vol_slider_->setValue(cfg_->tts.volume);
    vol_val_label_->setText(QString("%1%").arg(cfg_->tts.volume));

    // 通道状态
    chk_device_->setChecked(cfg_->tts.device_enabled);
    refresh_audio_devices();
    int dev_idx = combo_device_->findData(QString::fromStdString(cfg_->tts.audio_device_id));
    if (dev_idx >= 0) combo_device_->setCurrentIndex(dev_idx);
    combo_device_->setEnabled(cfg_->tts.device_enabled);

    dev_vol_slider_->setValue(cfg_->tts.device_volume);
    dev_vol_val_label_->setText(QString("%1%").arg(cfg_->tts.device_volume));
    dev_vol_slider_->setEnabled(cfg_->tts.device_enabled);

    chk_obs_->setChecked(cfg_->tts.obs_enabled);

    // 路由矩阵 - 本机
    chk_dev_danmaku_->setChecked(cfg_->tts.to_device.danmaku);
    chk_dev_gift_->setChecked(cfg_->tts.to_device.gift);
    chk_dev_sc_->setChecked(cfg_->tts.to_device.sc);
    chk_dev_guard_->setChecked(cfg_->tts.to_device.guard);
    chk_dev_entry_->setChecked(cfg_->tts.to_device.entry);
    chk_dev_like_->setChecked(cfg_->tts.to_device.like);

    // 路由矩阵 - OBS
    chk_obs_danmaku_->setChecked(cfg_->tts.to_obs.danmaku);
    chk_obs_gift_->setChecked(cfg_->tts.to_obs.gift);
    chk_obs_sc_->setChecked(cfg_->tts.to_obs.sc);
    chk_obs_guard_->setChecked(cfg_->tts.to_obs.guard);
    chk_obs_entry_->setChecked(cfg_->tts.to_obs.entry);
    chk_obs_like_->setChecked(cfg_->tts.to_obs.like);

    entry_filter_combo_->setCurrentIndex(std::clamp(cfg_->tts.entry_filter, 0, 2));
    chk_merge_->setChecked(cfg_->tts.merge_enabled);

    // 触发联动状态
    combo_device_->setEnabled(chk_device_->isChecked());
    chk_dev_danmaku_->setEnabled(chk_device_->isChecked());
    chk_dev_gift_->setEnabled(chk_device_->isChecked());
    chk_dev_sc_->setEnabled(chk_device_->isChecked());
    chk_dev_guard_->setEnabled(chk_device_->isChecked());
    chk_dev_entry_->setEnabled(chk_device_->isChecked());
    chk_dev_like_->setEnabled(chk_device_->isChecked());

    chk_obs_danmaku_->setEnabled(chk_obs_->isChecked());
    chk_obs_gift_->setEnabled(chk_obs_->isChecked());
    chk_obs_sc_->setEnabled(chk_obs_->isChecked());
    chk_obs_guard_->setEnabled(chk_obs_->isChecked());
    chk_obs_entry_->setEnabled(chk_obs_->isChecked());
    chk_obs_like_->setEnabled(chk_obs_->isChecked());
}

void TtsSettingsDialog::on_save()
{
    if (!cfg_) {
        accept();
        return;
    }

    cfg_->tts.key = key_edit_->text().trimmed().toStdString();
    cfg_->tts.region = region_combo_->currentText().trimmed().toStdString();
    cfg_->tts.voice = voice_combo_->currentData().toString().toStdString();
    int r = rate_slider_->value();
    cfg_->tts.rate = QString("%1%").arg(r >= 0 ? QString("+%1").arg(r) : QString::number(r)).toStdString();
    cfg_->tts.volume = vol_slider_->value();

    cfg_->tts.device_enabled = chk_device_->isChecked();
    cfg_->tts.audio_device_id = combo_device_->currentData().toString().toStdString();
    cfg_->tts.device_volume = dev_vol_slider_->value();
    cfg_->tts.to_device.danmaku = chk_dev_danmaku_->isChecked();
    cfg_->tts.to_device.gift = chk_dev_gift_->isChecked();
    cfg_->tts.to_device.sc = chk_dev_sc_->isChecked();
    cfg_->tts.to_device.guard = chk_dev_guard_->isChecked();
    cfg_->tts.to_device.entry = chk_dev_entry_->isChecked();
    cfg_->tts.to_device.like = chk_dev_like_->isChecked();

    cfg_->tts.obs_enabled = chk_obs_->isChecked();
    cfg_->tts.to_obs.danmaku = chk_obs_danmaku_->isChecked();
    cfg_->tts.to_obs.gift = chk_obs_gift_->isChecked();
    cfg_->tts.to_obs.sc = chk_obs_sc_->isChecked();
    cfg_->tts.to_obs.guard = chk_obs_guard_->isChecked();
    cfg_->tts.to_obs.entry = chk_obs_entry_->isChecked();
    cfg_->tts.to_obs.like = chk_obs_like_->isChecked();

    cfg_->tts.entry_filter = entry_filter_combo_->currentData().toInt();
    cfg_->tts.merge_enabled = chk_merge_->isChecked();
    cfg_->save();

    if (tts_mgr_) {
        TtsConfig c;
        c.enabled = cfg_->tts.enabled;
        c.key = QString::fromStdString(cfg_->tts.key);
        c.region = QString::fromStdString(cfg_->tts.region);
        c.voice = QString::fromStdString(cfg_->tts.voice);
        c.rate = QString::fromStdString(cfg_->tts.rate);
        c.pitch = QString::fromStdString(cfg_->tts.pitch);
        c.volume = cfg_->tts.volume;

        c.device_enabled = cfg_->tts.device_enabled;
        c.audio_device_id = QString::fromStdString(cfg_->tts.audio_device_id);
        c.device_volume = cfg_->tts.device_volume;
        c.to_device.danmaku = cfg_->tts.to_device.danmaku;
        c.to_device.gift = cfg_->tts.to_device.gift;
        c.to_device.sc = cfg_->tts.to_device.sc;
        c.to_device.guard = cfg_->tts.to_device.guard;
        c.to_device.entry = cfg_->tts.to_device.entry;
        c.to_device.like = cfg_->tts.to_device.like;

        c.obs_enabled = cfg_->tts.obs_enabled;
        c.to_obs.danmaku = cfg_->tts.to_obs.danmaku;
        c.to_obs.gift = cfg_->tts.to_obs.gift;
        c.to_obs.sc = cfg_->tts.to_obs.sc;
        c.to_obs.guard = cfg_->tts.to_obs.guard;
        c.to_obs.entry = cfg_->tts.to_obs.entry;
        c.to_obs.like = cfg_->tts.to_obs.like;

        c.entry_filter = static_cast<TtsEntryFilter>(cfg_->tts.entry_filter);
        c.merge_enabled = cfg_->tts.merge_enabled;
        tts_mgr_->update_config(c);
    }

    accept();
}

void TtsSettingsDialog::on_test_speak()
{
    if (!tts_mgr_) return;

    TtsConfig temp_cfg;
    temp_cfg.enabled = true;
    temp_cfg.key = key_edit_->text().trimmed();
    temp_cfg.region = region_combo_->currentText().trimmed();
    temp_cfg.voice = voice_combo_->currentData().toString();
    int r = rate_slider_->value();
    temp_cfg.rate = QString("%1%").arg(r >= 0 ? QString("+%1").arg(r) : QString::number(r));
    temp_cfg.volume = vol_slider_->value();
    temp_cfg.device_enabled = chk_device_->isChecked();
    temp_cfg.audio_device_id = combo_device_->currentData().toString();
    temp_cfg.device_volume = dev_vol_slider_->value();
    temp_cfg.obs_enabled = chk_obs_->isChecked();

    tts_mgr_->update_config(temp_cfg);
    tts_mgr_->test_speak("感谢关注哔哩哔哩直播间，祝主播开播大吉！");
}

void TtsSettingsDialog::on_clear_queue()
{
    if (tts_mgr_) {
        tts_mgr_->stop_and_clear();
    }
}
