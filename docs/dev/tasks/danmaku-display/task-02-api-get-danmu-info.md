---
name: "task-02-api-get-danmu-info"
depends_on: ["task-01-cmake-and-structs"]
labels: ["backend"]
worktree_root: ".worktree/task-02-api-get-danmu-info/"
test_commands: []
verify_commands:
  - "cmake --build build --target bilibili-live-obs 2>&1 | tail -5"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "bilibili-api.h 新增 get_danmu_info(std::string room_id) 声明"
    verification_type: manual
  - criteria: "bilibili-api.cpp 实现 getDanmuInfo HTTP GET 调用（WBI 签名 + JSON 解析）"
    verification_type: manual
  - criteria: "返回结构 ApiResult，data 含 token 和 host_list 字段"
    verification_type: manual
  - criteria: "编译通过"
    verification_type: lint
---

# Task 2: BilibiliApi::get_danmu_info() HTTP API 实现

## 目标概述

在 `BilibiliApi` 类中新增 `get_danmu_info()` 方法，通过 HTTP GET 调用 B站弹幕信息 API，获取 WebSocket 连接所需的 token 和服务器列表。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `src/bilibili-api.h` | 新增 `get_danmu_info()` 方法声明 | +1 行 |
| `src/bilibili-api.cpp` | 新增实现（WBI 签名 + HTTP GET + 响应解析） | ~15 行 |

## 实现步骤

### 步骤 1：bilibili-api.h — 新增方法声明

在 `get_buvid3()` 声明之后添加：

```cpp
// Danmaku
ApiResult get_danmu_info(const std::string &room_id);
```

### 步骤 2：bilibili-api.cpp — 实现 get_danmu_info()

在文件末尾添加实现：

```cpp
ApiResult BilibiliApi::get_danmu_info(const std::string &room_id)
{
    json params = {{"id", room_id}, {"type", 0}};

    auto [img_key, sub_key] = get_wbi_keys();
    std::string w_rid = sign_wbi(params, img_key, sub_key);
    params["w_rid"] = w_rid;
    params["wts"] = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    std::string url = "https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo";

    // 确保 cookies 包含 buvid3
    if (cookie_str_.find("buvid3") == std::string::npos) {
        ApiResult buvid_res = get_buvid3();
        if (buvid_res.ok && buvid_res.data.contains("buvid3")) {
            std::string buvid3 = buvid_res.data["buvid3"].get<std::string>();
            if (!cookie_str_.empty()) cookie_str_ += "; ";
            cookie_str_ += "buvid3=" + buvid3;
        }
    }

    return do_get(url, params);
}
```

**注意**：`get_buvid3()` 已经在 `BilibiliApi` 中实现，返回 `ApiResult`，其 `data["buvid3"]` 包含 buvid3 字符串。

### 步骤 3：验证编译

```bash
cmake --build build --target bilibili-live-obs 2>&1 | tail -5
```

预期：编译通过（linker 可能因 danmaku-ws.cpp 和 danmaku-display.cpp 无实现报 undefined symbol，属预期）。

### 步骤 4：手动验证 API 调用（可选）

如果构建环境有网络，可编写临时测试验证 API 调用格式正确：

```cpp
// 临时验证代码（仅用于调试，不提交）
#include "bilibili-api.h"
// ...
BilibiliApi api;
api.update_cookies(existing_cookies);
auto res = api.get_danmu_info("12345");
// 预期 res.code == 0, res.data["token"] 非空字符串
```

## API 参考

**请求**: `GET https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?id={room_id}&type=0&w_rid={sign}&wts={timestamp}`

**响应**:
```json
{
  "code": 0,
  "data": {
    "token": "Eac3Lm1JADz...",
    "host_list": [
      {"host": "tx-sh-live-comet-02.chat.bilibili.com", "wss_port": 443, "ws_port": 2244}
    ]
  }
}
```

## 验收

参考 frontmatter `acceptance` 字段中的 4 条验收标准。
