#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include "tts-manager.h"
#include "../config-manager.h"

class TtsSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit TtsSettingsDialog(ConfigManager *cfg, TtsManager *tts_mgr, QWidget *parent = nullptr);
    ~TtsSettingsDialog() override = default;

private slots:
    void on_save();
    void on_test_speak();
    void on_clear_queue();
    void refresh_audio_devices();

private:
    void setup_ui();
    void load_values();

    ConfigManager *cfg_ = nullptr;
    TtsManager *tts_mgr_ = nullptr;

    // 凭证与语音
    QLineEdit *key_edit_ = nullptr;
    QComboBox *region_combo_ = nullptr;
    QComboBox *voice_combo_ = nullptr;
    QSlider *rate_slider_ = nullptr;
    QLabel *rate_val_label_ = nullptr;
    QSlider *vol_slider_ = nullptr;
    QLabel *vol_val_label_ = nullptr;

    // 输出通道
    QCheckBox *chk_device_ = nullptr;
    QComboBox *combo_device_ = nullptr;
    QSlider *dev_vol_slider_ = nullptr;
    QLabel *dev_vol_val_label_ = nullptr;
    QCheckBox *chk_obs_ = nullptr;
    QLabel *obs_status_label_ = nullptr;

    // 路由矩阵 - 本机设备
    QCheckBox *chk_dev_danmaku_ = nullptr;
    QCheckBox *chk_dev_gift_ = nullptr;
    QCheckBox *chk_dev_sc_ = nullptr;
    QCheckBox *chk_dev_guard_ = nullptr;
    QCheckBox *chk_dev_entry_ = nullptr;
    QCheckBox *chk_dev_like_ = nullptr;

    // 路由矩阵 - OBS 推流
    QCheckBox *chk_obs_danmaku_ = nullptr;
    QCheckBox *chk_obs_gift_ = nullptr;
    QCheckBox *chk_obs_sc_ = nullptr;
    QCheckBox *chk_obs_guard_ = nullptr;
    QCheckBox *chk_obs_entry_ = nullptr;
    QCheckBox *chk_obs_like_ = nullptr;

    // 过滤与合并
    QComboBox *entry_filter_combo_ = nullptr;
    QCheckBox *chk_merge_ = nullptr;
};
