---
name: "task-05-integration"
depends_on: ["task-01-cmake-and-structs", "task-03-danmaku-ws", "task-04-danmaku-display"]
labels: ["frontend", "backend"]
worktree_root: ".worktree/task-05-integration/"
test_commands: []
verify_commands:
  - "cmake --build build --target bilibili-live-obs 2>&1 | tail -5"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "plugin-main.cpp 创建 DanmakuWebSocket 实例，传递到 BiliDock"
    verification_type: manual
  - criteria: "bili-dock.h 新增 danmaku_ws_ 和 danmaku_display_ 成员 + set_danmaku_ws() 方法"
    verification_type: manual
  - criteria: "bili-dock.cpp init_ui() 在推流线路下方插入 DanmakuDisplay（默认隐藏）"
    verification_type: manual
  - criteria: "开播成功后自动连接弹幕 WebSocket + 显示弹幕面板"
    verification_type: manual
  - criteria: "停播后自动断开弹幕 WebSocket + 隐藏弹幕面板"
    verification_type: manual
  - criteria: "set_danmaku_ws() 中完成 4 个信号绑定（danmaku/gift/sc/connection_state）"
    verification_type: manual
  - criteria: "编译通过，完整插件可加载"
    verification_type: lint
---

# Task 5: plugin-main.cpp + BiliDock 总装集成

## 目标概述

将所有模块串联起来：在 `plugin-main.cpp` 创建 `DanmakuWebSocket` 实例，注入到 `BiliDock`；在 `BiliDock` 中集成 `DanmakuDisplay` UI 组件，绑定信号/槽，实现开播/停播联动。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `src/plugin-main.cpp` | 新增 `DanmakuWebSocket` 全局变量、创建/销毁、传递到 BiliDock | ~10 行 |
| `src/bili-dock.h` | 新增 `danmaku_ws_` / `danmaku_display_` 成员 + `set_danmaku_ws()` 声明 | ~5 行 |
| `src/bili-dock.cpp` | `init_ui()` 添加 DanmakuDisplay + `set_danmaku_ws()` 实现 + 开播/停播联动 | ~40 行 |

## 实现步骤

### 步骤 1：plugin-main.cpp — 新增 include + 全局变量

在文件头部新增 include：

```cpp
#include "danmaku-ws.h"
```

在全局变量区域（`static AuthService *s_auth = nullptr;` 之后）新增：

```cpp
static DanmakuWebSocket *s_danmaku_ws = nullptr;
```

### 步骤 2：plugin-main.cpp — init_services() 新增初始化

在 `s_auth = new AuthService(...)` 之后添加：

```cpp
s_danmaku_ws = new DanmakuWebSocket();
s_danmaku_ws->set_api(s_api);
```

### 步骤 3：plugin-main.cpp — destroy_services() 新增销毁

在 `delete s_api;` 之前添加：

```cpp
delete s_danmaku_ws; s_danmaku_ws = nullptr;
```

### 步骤 4：plugin-main.cpp — dock_load() 传递 WS 到 BiliDock

在 `s_dock->set_services(s_auth, s_live, s_user, s_cfg);` 之后添加：

```cpp
s_dock->set_danmaku_ws(s_danmaku_ws);
```

### 步骤 5：bili-dock.h — 新增成员 + 方法声明

在文件头部新增 include：

```cpp
#include "danmaku-display.h"
```

在 `BiliDock` 类的 public 区域新增方法声明：

```cpp
void set_danmaku_ws(DanmakuWebSocket *ws);
```

在 `BiliDock` 类的 private 区域新增成员变量：

```cpp
DanmakuWebSocket *danmaku_ws_ = nullptr;
DanmakuDisplay *danmaku_display_ = nullptr;
```

### 步骤 6：bili-dock.cpp init_ui() — 添加 DanmakuDisplay UI

在 `init_ui()` 函数中，找到 `main->addWidget(stream_route_group_)` 这一行，在其后添加：

```cpp
// ── Danmaku display ──
danmaku_display_ = new DanmakuDisplay();
danmaku_display_->hide();
main->addWidget(danmaku_display_);
```

### 步骤 7：bili-dock.cpp — 实现 set_danmaku_ws()

新增函数：

```cpp
void BiliDock::set_danmaku_ws(DanmakuWebSocket *ws)
{
    danmaku_ws_ = ws;
    if (!ws || !danmaku_display_) return;

    connect(ws, &DanmakuWebSocket::danmaku_received,
            danmaku_display_, &DanmakuDisplay::append_danmaku);
    connect(ws, &DanmakuWebSocket::gift_received,
            danmaku_display_, &DanmakuDisplay::append_gift);
    connect(ws, &DanmakuWebSocket::super_chat_received,
            danmaku_display_, &DanmakuDisplay::append_super_chat);
    connect(ws, &DanmakuWebSocket::connection_state_changed,
            this, [this](bool connected, int popularity) {
        danmaku_display_->set_connected(connected);
        danmaku_display_->set_popularity(popularity);
    });
}
```

### 步骤 8：bili-dock.cpp do_start_live() — 开播联动

在 `do_start_live()` 函数中，找到 `bili_live_started_ = true` 之后的成功分支，添加：

```cpp
if (danmaku_ws_ && !state_->room_id.empty()) {
    danmaku_ws_->connect_to_room(state_->room_id);
    danmaku_display_->clear_all();
    danmaku_display_->show();
}
```

### 步骤 9：bili-dock.cpp do_stop_live() — 停播联动

在 `do_stop_live()` 函数中，找到成功停播分支，添加：

```cpp
if (danmaku_ws_) {
    danmaku_ws_->disconnect_from_room();
    danmaku_display_->hide();
}
```

### 步骤 10：验证编译

```bash
cmake --build build --target bilibili-live-obs 2>&1 | tail -5
```

预期：编译完全通过，无 undefined symbol 错误，生成 `libbilibili-live-obs.so`。

## 完整 `plugin-main.cpp` 变更后的全局变量区域（供参考）

```cpp
static BiliDock *s_dock = nullptr;
static BilibiliApi *s_api = nullptr;
static ConfigManager *s_cfg = nullptr;
static SessionState *s_state = nullptr;
static UserService *s_user = nullptr;
static LiveService *s_live = nullptr;
static AuthService *s_auth = nullptr;
static DanmakuWebSocket *s_danmaku_ws = nullptr;
```

## 集成时序

```
用户点击"开始直播"
  → do_start_live()
    → live_->start_live() HTTP 请求
      → 成功后 state_->is_live = true
        → obs 推流开始
        → danmaku_ws_->connect_to_room(room_id)
          → HTTP getDanmuInfo → WSS 连接 → 认证 → 心跳
            → 弹幕消息通过 signal 到达 DanmakuDisplay
        → danmaku_display_->show()

用户点击"停止直播"
  → do_stop_live()
    → live_->stop_live() HTTP 请求
      → 成功后 state_->is_live = false
        → obs 推流停止
        → danmaku_ws_->disconnect_from_room()
          → WSS close → 停止心跳/重连
        → danmaku_display_->hide()
```

## 注意事项

1. **生命周期**：`DanmakuWebSocket` 在 `init_services()` 创建，`destroy_services()` 销毁，与 `BilibiliApi` 等全局服务一致
2. **Q_ARG 注册**：消息结构体通过 `Q_DECLARE_METATYPE` 注册（Task 1），跨线程信号/槽值传递安全
3. **空指针保护**：`set_danmaku_ws()` 中检查 `ws` 和 `danmaku_display_` 非空
4. **开播前清除**：`do_start_live()` 中调用 `clear_all()` 清除上次直播的弹幕
5. **错误隔离**：弹幕功能异常不影响开播/停播流程（已在 DanmakuWebSocket 内部处理）

## 验收

参考 frontmatter `acceptance` 字段中的 7 条验收标准。
