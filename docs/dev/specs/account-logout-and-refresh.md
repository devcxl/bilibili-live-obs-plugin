# 技术方案：账号退出按钮 + OBS 启动时自动刷新账户信息

> **2026-08 更新**：本插件**不做多账号**，`account_combo_` 下拉框及相关服务（`get_account_list` / `switch_account`）已删除，仅支持单一登录态。本文中所有涉及多账号切换的设计均已过时，仅作历史参考。

## 元信息
- **PRD**: `docs/prd/account-logout-and-refresh.md`
- **Issue**: [#1](https://github.com/devcxl/bilibili-live-obs-plugin/issues/1)
- **版本**: v1.0
- **状态**: 设计完成

---

## 1. 功能 1：右上角退出登录按钮

### 1.1 布局方案

**当前结构**（bili-dock.cpp:180-183）：
```
QVBoxLayout (main)
├── QLabel "B站直播工具 - OBS 插件"  ← 独立 widget，居中对齐
├── QGroupBox "账号"
│   ...
```

**目标结构**：
```
QVBoxLayout (main)
├── QHBoxLayout (title_row)                    ← 新增容器
│   ├── QLabel "B站直播工具 - OBS 插件"        ← 保留，左对齐
│   ├── stretch                              ← 将按钮推到右侧
│   └── QPushButton "退出登录"                ← 新增，右对齐
├── QGroupBox "账号"
│   ...
```

### 1.2 实现要点

| 组件 | 变更内容 |
|------|----------|
| `bili-dock.h` | 新增成员 `QPushButton *btn_header_logout_` |
| `bili-dock.cpp:init_ui()` | 用 `QHBoxLayout` 包裹 title label + stretch + 退出按钮 |
| `bili-dock.cpp:on_login_done()` | 调用 `btn_header_logout_->show()` |
| `bili-dock.cpp:set_logged_out()` | 调用 `btn_header_logout_->hide()` |
| `bili-dock.cpp:do_logout()` | uid 获取从 `account_combo_->currentData()` 改为优先 `cfg_->current_uid` |
| `bili-dock.cpp:refresh_account_list()` | 移除 `btn_logout_` 的显示逻辑（原 btn_logout_ 废弃） |

### 1.3 旧按钮处理

**决策：移除 `btn_logout_`（bili-dock.h:80），保留 `account_combo_`。**

理由：
- 右上角按钮覆盖单/多账号全部场景，旧按钮功能冗余
- `account_combo_` 保留用于多账号切换，但不显示配套的登出按钮
- 旧按钮的 visibility 逻辑（仅 `count() > 1` 显示）不再适用

### 1.4 按钮样式

```cpp
btn_header_logout_->setStyleSheet(
    "QPushButton { color:#FFB74D; background:transparent; border:1px solid #FFB74D; "
    "border-radius:4px; padding:2px 12px; font-size:12px; }"
    "QPushButton:hover { background:#FFB74D; color:#1e1e1e; }");
btn_header_logout_->setFixedHeight(24);
```

遵循 OBS 深色主题（背景色 `#1e1e1e`）。橙色边框+文字，hover 反色。

### 1.5 `do_logout()` 改动

当前 `do_logout()` 通过 `account_combo_->currentData()` 获取 uid。改为通过 `cfg_->current_uid` 获取：

```cpp
void BiliDock::do_logout()
{
    std::string uid = cfg_->current_uid;  // 优先使用配置层
    if (uid.empty() || !user_) return;

    user_->logout(uid);
    // 仅当 header 按钮触发时，btn_logout_ 已不存在，统一处理
    account_combo_->setVisible(account_combo_->count() > 0);
    set_logged_out();
    btn_login_->show();
    status_bar_->setText("已登出");
}
```

兼容性：`account_combo_->currentData()` 和 `cfg_->current_uid` 在登录态下等价，后者在 combo 隐藏时依然有效。

---

## 2. 功能 2：OBS 启动时自动刷新账户信息

### 2.1 时序设计

```
OBS_FRONTEND_EVENT_FINISHED_LOADING
    │
    ▼
obs_queue_task(OBS_TASK_UI, ui_dock_load)    ← 非阻塞大队列
    │
    ▼
dock_load()
    ├── init_services()
    │     └── s_user->init_current_user()     ← 从本地缓存恢复 session
    ├── s_dock = new BiliDock()
    ├── s_dock->set_services(...)
    ├── on_login_done(cached_data)            ← UI 立即展示缓存数据
    ├── obs_frontend_add_dock_by_id()         ← dock 挂载到 OBS
    ├── check_live_status()
    └── QTimer::singleShot(500ms, refresh)    ← ✨ 延迟触发刷新
            │
            ▼
        BiliDock::refresh_account_info()
            ├── user_->refresh_current_user()  ← 同步 HTTP 调用
            ├── update_user_display(new_data)  ← 更新 UI
            └── status_bar_->setText("账户信息已更新")
```

### 2.2 新增接口

#### `BiliDock::refresh_account_info()`（新 slot）

```cpp
// bili-dock.h (private slots)
void refresh_account_info();

// bili-dock.cpp
void BiliDock::refresh_account_info()
{
    if (!user_ || !user_->has_valid_session()) return;

    auto result = user_->refresh_current_user();
    if (result["code"] == 0 && result.contains("data")) {
        QJsonObject data = QJsonDocument::fromJson(
            QByteArray::fromStdString(result["data"].dump())).object();
        update_user_display(data);
        status_bar_->setText("账户信息已更新");
    }
    // code != 0: 静默降级，不更新 UI，不弹错误提示
}
```

#### 调用点（`plugin-main.cpp:dock_load()`）

在 `check_live_status()` 调用之后添加：

```cpp
// 异步刷新账户信息（延迟触发，不阻塞 dock 加载）
if (s_user->has_valid_session()) {
    QTimer::singleShot(500, s_dock, &BiliDock::refresh_account_info);
}
```

### 2.3 异步策略选择

| 方案 | 异步程度 | 复杂度 | 线程安全 | 推荐 |
|------|----------|--------|----------|------|
| **方案 A: QTimer::singleShot 延迟同步** | 延迟非阻塞 | 低 | 安全（UI 线程） | ✅ |
| 方案 B: std::async + 轮询 future | 真正异步 | 中 | 需处理竞态 | 备选 |
| 方案 C: QNetworkAccessManager 替代 libcurl | 真正异步 | 高（破坏性重构） | 安全 | ❌ |

**选择方案 A**。理由：
- `refresh_current_user()` 的 HTTP 调用耗时约 1-2 秒，延迟到 dock 完全挂载后执行
- 500ms 延迟确保 dock 和 OBS 界面已完全渲染，满足 PRD "不阻塞 dock 加载和 OBS 界面渲染"
- 无需引入线程基础设施，代码改动最小（约 15 行净增）
- libcurl 的同步调用在后台线程仍有竞态风险（`api_` 状态非线程安全），延迟同步避免此问题

备选：如果后续网络延迟成为用户体验问题，可升级为方案 B。

### 2.4 失败降级策略

| 失败场景 | 用户感知 |
|----------|----------|
| 网络不通 / API 超时 | UI 保持旧缓存数据，无错误提示 |
| API 返回错误码 | UI 保持旧缓存数据，无错误提示 |
| 本地缓存不存在 | `has_valid_session() == false`，直接跳过刷新 |
| 用户数据字段缺失 | `update_user_display()` 中已有空值处理（显示 0 或空字符串） |

降级原则：**刷新是增强操作，不是关键路径**。缓存数据即可用，网络的不确定性不应影响主功能。

---

## 3. 数据流

### 3.1 退出按钮数据流

```
用户点击 btn_header_logout_
    │
    ▼
BiliDock::do_logout()
    ├── cfg_->current_uid → uid
    ├── UserService::logout(uid)
    │     ├── cfg_->users.erase(uid)
    │     ├── cfg_->current_uid.clear()
    │     ├── state_->clear()
    │     ├── api_->update_cookies({})
    │     └── cfg_->save()
    ├── BiliDock::set_logged_out()
    │     ├── hide all login-state widgets
    │     └── btn_header_logout_->hide()
    ├── btn_login_->show()
    └── status_bar_->setText("已登出")
```

### 3.2 刷新数据流

```
QTimer::singleShot(500ms)
    │
    ▼
BiliDock::refresh_account_info()
    ├── UserService::has_valid_session()  ← 超时期间用户可能已登出
    │
    ▼ (valid session)
UserService::refresh_current_user()
    ├── fetch_full_user_data()
    │     ├── api_->get_user_info()      ← GET https://api.bilibili.com/x/web-interface/nav
    │     └── api_->get_user_stat()      ← GET https://api.bilibili.com/x/web-interface/card
    ├── save_user_data(uid, full_data, cookie, room_id, csrf)
    │     ├── cfg_->users[uid] = user_data   ← 更新内存
    │     └── cfg_->save()                    ← 持久化到磁盘
    └── return {code: 0, data: strip_sensitive(saved)}
    │
    ▼ (code == 0)
BiliDock::update_user_display(new_data)
    ├── user_info_label_   ← 昵称 + 等级
    ├── user_detail_label_ ← UID + 关注/粉丝/硬币/B币
    └── user_face_label_   ← 头像图片（异步加载）
```

---

## 4. 影响范围

### 4.1 文件变更清单

| 文件 | 变更量 | 说明 |
|------|--------|------|
| `src/bili-dock.h` | +2 行 | 新增 `btn_header_logout_` 成员 + `refresh_account_info()` 声明 |
| `src/bili-dock.cpp` | ~40 行净增 | init_ui 重构标题行、refresh_account_info 实现、do_logout 调整、refresh_account_list 删旧逻辑 |
| `src/plugin-main.cpp` | +4 行 | dock_load 末尾添加刷新触发 |

**总计：~50 行变更，3 个文件**。

### 4.2 不影响的模块

- `src/bilibili-api.{h,cpp}` — 无变更
- `src/auth-service.{h,cpp}` — 无变更（`refresh_current_user()` 已存在，仅新增调用点）
- `src/config-manager.{h,cpp}` — 无变更
- `CMakeLists.txt` — 无变更（无新依赖）

### 4.3 兼容性

- ✅ `set_logged_out()` 行为不变，仅新增 `btn_header_logout_->hide()`
- ✅ `refresh_current_user()` 无变更
- ✅ `update_user_display()` 无变更，可复用
- ✅ `UserService::logout()` 无变更
- ✅ Qt6 信号/槽机制不变
- ✅ OBS API 调用点不变

---

## 5. 风险评估

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| 刷新 HTTP 调用阻塞 UI 线程 | 必然（1-2s） | 低 — 发生在 dock 已加载后 | 500ms 延迟确保界面先渲染；后续可升级为 std::async |
| 刷新时用户已手动登出 | 低 | 低 — `has_valid_session()` 提前检查 | slot 入口双重检查，安全跳过 |
| 旧 btn_logout_ 移除影响多账号切换 | 无 | 无 | 仅移除登出按钮，account_combo_ 保留 |
| 网络刷新失败导致 status_bar_ 显示过时信息 | 低 | 极低 | 刷新失败不改变 status_bar_ 文案（登录时显示"已登录 — 就绪"） |
| 多次 OBS 事件导致重复刷新 | 极低 | 低 | `OBS_FRONTEND_EVENT_FINISHED_LOADING` 仅触发一次 |

---

## 6. 验收要点

### 功能 1
1. 登录后右上角显示"退出登录"按钮，未登录时不显示
2. 单账号和多账号均显示该按钮
3. 点击后：cookie 清除、UI 回到未登录、status_bar_ = "已登出"
4. 按钮 hover 有视觉反馈，与深色主题协调

### 功能 2
1. OBS 启动后 1-2 秒内用户头像/昵称/等级等自动刷新
2. 刷新期间 dock 已可见且展示缓存数据
3. 网络异常时无弹窗、无错误提示
4. 刷新成功后 status_bar_ 显示"账户信息已更新"
