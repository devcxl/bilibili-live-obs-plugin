---
name: "task-01-cmake-and-structs"
depends_on: []
labels: ["backend"]
worktree_root: ".worktree/task-01-cmake-and-structs/"
test_commands: []
verify_commands:
  - "cmake --build build --target bilibili-live-obs 2>&1 | tail -5"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "CMakeLists.txt 新增 libbrotli 依赖，cmake 配置成功"
    verification_type: manual
  - criteria: "danmaku-ws.h 定义 DanmakuMessage / GiftMessage / SuperChatMessage 结构体 + Q_DECLARE_METATYPE"
    verification_type: manual
  - criteria: "编译通过（链接期可能因缺少 .cpp 实现报 undefined symbol，属正常）"
    verification_type: lint
---

# Task 1: CMakeLists.txt 变更 + 公共数据结构定义

## 目标概述

为弹幕功能准备好编译基础设施（libbrotli 依赖 + 新源文件注册）和跨模块共享的消息数据结构定义。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `CMakeLists.txt` | 新增 libbrotli pkg-config 依赖、2 个新源文件、link/includes | ~15 行 |
| `src/danmaku-ws.h`（新建） | 消息结构体定义（DanmakuMessage, GiftMessage, SuperChatMessage）+ Q_DECLARE_METATYPE | ~40 行 |

## 实现步骤

### 步骤 1：CMakeLists.txt — 新增 libbrotli 依赖

在 `pkg_check_modules(OPENSSL REQUIRED openssl)` 之后添加：

```cmake
pkg_check_modules(LIBBROTLI REQUIRED libbrotlienc libbrotlidec)
```

### 步骤 2：CMakeLists.txt — 注册新源文件

修改 `SOURCES` 列表，追加 2 个新文件：

```cmake
set(SOURCES
  src/plugin-main.cpp
  src/config-manager.cpp
  src/bilibili-api.cpp
  src/auth-service.cpp
  src/bili-dock.cpp
  src/danmaku-ws.cpp          # 新增
  src/danmaku-display.cpp     # 新增
)
```

修改 `HEADERS` 列表，追加 2 个新文件：

```cmake
set(HEADERS
  src/config-manager.h
  src/bilibili-api.h
  src/auth-service.h
  src/bili-dock.h
  src/danmaku-ws.h            # 新增
  src/danmaku-display.h       # 新增
)
```

### 步骤 3：CMakeLists.txt — 新增 include 路径

在 `target_include_directories` 块末尾添加：

```cmake
${LIBBROTLI_INCLUDE_DIRS}
```

### 步骤 4：CMakeLists.txt — 新增 link 库

在 `target_link_libraries` 块末尾添加：

```cmake
${LIBBROTLI_LIBRARIES}
```

### 步骤 5：CMakeLists.txt — 新增 CPACK 运行时依赖

修改 `CPACK_DEBIAN_PACKAGE_DEPENDS`，追加 `libbrotli1`：

```cmake
set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio (>= 30.0), libqt6widgets6, libqt6network6, libcurl4, libqrencode4, libssl3, libbrotli1")
```

### 步骤 6：src/danmaku-ws.h（新建）— 数据结构定义

创建文件 `src/danmaku-ws.h`，内容如下（**仅结构体和 Q_DECLARE_METATYPE，不含类定义**）：

```cpp
#pragma once

#include <string>
#include <QMetaType>

// ─── 弹幕消息 ───
struct DanmakuMessage {
    std::string cmd;           // "DANMU_MSG"
    std::string username;      // 发送者昵称
    std::string uid;           // 发送者 UID
    std::string message;       // 弹幕文本
    std::string fan_badge;     // 粉丝勋章名（可选）
    int fan_badge_level = 0;   // 粉丝勋章等级
};

// ─── 礼物消息 ───
struct GiftMessage {
    std::string username;      // 送礼者昵称
    std::string gift_name;     // 礼物名称
    int num = 0;               // 数量
    int combo_num = 0;         // 连送次数（combo）
    std::string action;        // "赠送" / "续费"
};

// ─── SC 醒目留言 ───
struct SuperChatMessage {
    std::string username;      // 发送者昵称
    std::string message;       // SC 内容
    int price = 0;             // 金额（人民币，单位：分）
};

// 注册到 Qt 元对象系统（支持跨线程信号/槽值类型传递）
Q_DECLARE_METATYPE(DanmakuMessage)
Q_DECLARE_METATYPE(GiftMessage)
Q_DECLARE_METATYPE(SuperChatMessage)
```

**注意**：Task 3 将在此基础上补充 `DanmakuWebSocket` 类的完整定义。

### 步骤 7：CMakeLists.txt — 新增 LIBBROTLI 编译定义

在 `target_compile_definitions` 块末尾添加：

```cmake
${LIBBROTLI_CFLAGS_OTHER}
```

### 步骤 8：验证编译

```bash
cmake --build build --target bilibili-live-obs 2>&1 | tail -5
```

预期：cmake 配置成功，编译通过。可能 linker 报 undefined symbol（因为 danmaku-ws.cpp 和 danmaku-display.cpp 尚无实现），属于预期行为 — Task 3/4 完成后解决。

## 验收

参考 frontmatter `acceptance` 字段中的 3 条验收标准。
