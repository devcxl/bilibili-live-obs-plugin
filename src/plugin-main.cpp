#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QMainWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
#include <QTimer>

#include "bili-dock.h"
#include "bilibili-api.h"
#include "config-manager.h"
#include "auth-service.h"
#include "danmaku-ws.h"
#include "tts/tts-manager.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("bilibili-live-obs", "en-US")

static BiliDock *s_dock = nullptr;
static BilibiliApi *s_api = nullptr;
static ConfigManager *s_cfg = nullptr;
static SessionState *s_state = nullptr;
static UserService *s_user = nullptr;
static LiveService *s_live = nullptr;
static AuthService *s_auth = nullptr;
static DanmakuWebSocket *s_danmaku_ws = nullptr;
static TtsManager *s_tts = nullptr;

static void init_services()
{
    if (s_api) return;
    s_api = new BilibiliApi();
    s_cfg = new ConfigManager();
    s_state = new SessionState();
    s_user = new UserService(s_api, s_cfg, s_state);
    s_live = new LiveService(s_api, s_cfg, s_state);
    s_auth = new AuthService(s_api, s_user, s_live, s_state);
    s_danmaku_ws = new DanmakuWebSocket();
    s_danmaku_ws->set_api(s_api);
    s_danmaku_ws->set_config(s_cfg);
    s_tts = new TtsManager();
    s_user->init_current_user();

    if (s_user->has_valid_session()) {
        blog(LOG_INFO, "[bili] session restored, uid=%s room=%s",
             s_state->uid.c_str(), s_state->room_id.c_str());
    } else {
        blog(LOG_WARNING, "[bili] no valid session, need re-login");
    }
}

static void destroy_services()
{
    delete s_tts;   s_tts = nullptr;
    delete s_auth;  s_auth = nullptr;
    delete s_danmaku_ws; s_danmaku_ws = nullptr;
    delete s_live;  s_live = nullptr;
    delete s_user;  s_user = nullptr;
    delete s_state; s_state = nullptr;
    delete s_cfg;   s_cfg = nullptr;
    delete s_api;   s_api = nullptr;
}

static void dock_load()
{
    init_services();

    auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    s_dock = new BiliDock(main);
    s_dock->set_services(s_auth, s_live, s_user, s_cfg);
    s_dock->set_danmaku_ws(s_danmaku_ws);
    s_dock->set_tts_manager(s_tts);

    if (s_user->has_valid_session()) {
        auto saved = s_user->load_saved_config();
        if (saved["code"] == 0 && !saved["data"].is_null() && !saved["data"].empty()) {
            auto data = saved["data"];
            s_dock->on_login_done(
                QJsonDocument::fromJson(QByteArray::fromStdString(data.dump())).object());
        }
    } else {
        blog(LOG_WARNING, "[bili] no valid session on startup, showing login UI");
    }

    // OBS 在 OBSBasic::OBSInit() 中就已调用 restoreState(DockState) 恢复布局，但插件是在
    // FINISHED_LOADING 才注册 dock，此时 Qt 只在布局里留下了同名占位符。而
    // obs_frontend_add_dock_by_id() 内部的 addDockWidget() 会主动删除该占位符，
    // 导致 dock 位置丢失（退回默认的右侧悬浮态）。
    // 因此在注册前后各保存/重放一次布局，让 dock 认领回自己的位置。
    QByteArray dock_layout;
    if (main)
        dock_layout = main->saveState();

    obs_frontend_add_dock_by_id(
        "bili_live_dock",
        "B站直播工具",
        s_dock);

    if (main && !dock_layout.isEmpty()) {
        if (!main->restoreState(dock_layout))
            blog(LOG_WARNING, "[bili] dock layout restore failed, dock may fall back to default position");
    }

    s_dock->restore_live_state();

    // 异步刷新账户信息（延迟触发，不阻塞 dock 加载）
    if (s_user->has_valid_session()) {
        QTimer::singleShot(500, s_dock, &BiliDock::refresh_account_info);
    }
}

static void dock_unload()
{
    if (s_cfg) s_cfg->save();
    if (s_dock) {
        delete s_dock;
        s_dock = nullptr;
    }
    destroy_services();
}

static void ui_dock_load(void *)
{
    dock_load();
}

static void ui_streaming_started(void *)
{
    if (s_dock) s_dock->on_obs_streaming_started();
}

static void ui_streaming_stopped(void *)
{
    if (s_dock) s_dock->on_obs_streaming_stopped();
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        obs_queue_task(OBS_TASK_UI, ui_dock_load, nullptr, false);
    }
    if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
        obs_queue_task(OBS_TASK_UI, ui_streaming_started, nullptr, false);
    }
    if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
        obs_queue_task(OBS_TASK_UI, ui_streaming_stopped, nullptr, false);
    }
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        dock_unload();
    }
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[bilibili-live-obs] version %s loaded", PROJECT_VERSION);

    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[bilibili-live-obs] unloaded");
}
