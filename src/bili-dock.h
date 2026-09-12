#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <obs.h>

#include "bilibili-api.h"
#include "config-manager.h"
#include "auth-service.h"
#include "danmaku-display.h"
#include "tts/tts-manager.h"

class BiliDock : public QWidget {
    Q_OBJECT

public:
    explicit BiliDock(QWidget *parent = nullptr);
    ~BiliDock() override;

    void set_services(AuthService *auth, LiveService *live,
                      UserService *user, ConfigManager *cfg);
    void set_danmaku_ws(DanmakuWebSocket *ws);
    void set_tts_manager(TtsManager *tts);

public slots:
    void on_login_done(const QJsonObject &data);
    void on_obs_streaming_started();
    void on_obs_streaming_stopped();
    void refresh_account_info();
    void restore_live_state();

private slots:
    void start_login();
    void poll_login();
    void do_logout();
    void on_parent_area_changed(const QString &name);
    void on_sub_area_changed(const QString &name);
    void on_title_edited();
    void on_face_reply(QNetworkReply *reply);
    void on_stream_route_changed(int index);
    void do_start_live();
    void do_stop_live();
    void open_tts_settings();
    void open_danmaku_settings();

private:
    void init_ui();
    int reset_upper_height();    // 内容变化后重新锁定上部设置区高度，返回应用后的固定高度
    void set_logged_out();
    void hide_login_ui();
    void set_login_error(const QString &msg);
    void show_qr_in_label(QLabel *label, const QString &url);
    void show_verify_qr(const QString &url);
    void handle_poll_login_result(const json &result);
    void load_partitions();
    bool do_refresh_account();   // 刷新当前用户信息，返回是否成功（自动/手动刷新共用）
    void update_user_display(const QJsonObject &data);
    bool configure_obs_stream(const std::string &server, const std::string &key);
    void apply_pending_stream_route();
    void restore_obs_stream_service();
    void start_danmaku();
    void stop_danmaku();

    AuthService *auth_ = nullptr;
    LiveService *live_ = nullptr;
    UserService *user_ = nullptr;
    ConfigManager *cfg_ = nullptr;

    DanmakuWebSocket *danmaku_ws_ = nullptr;
    DanmakuDisplay *danmaku_display_ = nullptr;
    QPushButton *btn_danmaku_settings_ = nullptr;

    TtsManager *tts_manager_ = nullptr;
    QCheckBox *tts_toggle_ = nullptr;
    QPushButton *btn_tts_settings_ = nullptr;

    QTimer *poll_timer_;
    QString qrcode_key_;
    int poll_count_ = 0;
    bool polling_in_flight_ = false;

    QNetworkAccessManager *net_;

    // UI elements
    QPushButton *btn_login_;
    QLabel *login_qr_;
    QLabel *login_status_;
    QLabel *user_face_label_;
    QLabel *user_info_label_;
    QLabel *user_detail_label_;
    QProgressBar *level_progress_ = nullptr;
    QPushButton *btn_refresh_user_ = nullptr;
    QPushButton *btn_header_logout_ = nullptr;

    QGroupBox *verify_group_;
    QLabel *verify_qr_;
    QLabel *verify_hint_;

    QLineEdit *title_edit_;
    QComboBox *parent_combo_;
    QComboBox *sub_combo_;
    QPushButton *btn_start_;
    QPushButton *btn_stop_;
    QLabel *stream_status_;

    QGroupBox *stream_route_group_;
    QComboBox *stream_route_combo_;

    QLabel *status_bar_;

    QWidget *upper_ = nullptr;   // 上部设置区容器（固定高度，不随 QSplitter 拖拽变化）

    std::unordered_map<std::string, std::vector<std::string>> partition_cache_;
    std::string primary_rtmp_addr_;
    std::string primary_rtmp_code_;
    std::string backup_rtmp_addr_;
    std::string backup_rtmp_code_;
    bool restoring_ = false;
    bool bili_live_started_ = false;
    bool obs_service_overridden_ = false;
    bool restore_obs_service_pending_ = false;
    int active_stream_route_ = 0;
    int pending_stream_route_ = -1;
    obs_service_t *previous_obs_service_ = nullptr;
};
