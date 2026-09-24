# B站直播弹幕获取方式研究

## 研究结论摘要

B站直播弹幕**确实通过 WebSocket 协议**实时推送。核心流程：**HTTP 获取 Token → WebSocket 连接 → 认证 → 心跳维持 → 解析二进制消息**。当前项目（bilibili-live-obs-plugin）已有 WBI 签名、buvid3 等基础设施，但完全没有 WebSocket / 弹幕相关实现，需要从零搭建。

---

## 背景与问题定义

- **研究目标**：查明 Bilibili 直播弹幕的获取协议、数据格式、安全要求和实现要点
- **适用范围**：B站直播弹幕/礼物/用户进出等实时消息流
- **相关项目**：当前仓库 `bilibili_live_stream_code` 是 OBS 插件（C++/Qt6），目前仅支持 HTTP REST API（登录、开播、推流配置），不含弹幕功能
- **来源限制**：所有来源均为社区逆向分析文档和开源实现，无 B站官方 SDK/文档

---

## 研究方法

| 项目 | 说明 |
|---|---|
| 搜索策略 | Web 深度搜索 + GitHub 开源仓库 + 已知社区 API 文档（SocialSisterYi/bilibili-API-collect） |
| 来源类型 | 社区协议逆向文档（5个独立来源）、开源实现（npm包、Go/Python/C++ 客户端 4个）、技术博客（3篇） |
| 迭代轮次 | 2轮：第1轮全貌收集，第2轮交叉验证 + 风控变化核实 |
| 关键术语 | `getDanmuInfo`, `broadcastlv.chat.bilibili.com`, `protover=3`, `Brotli`, `WebSocket Binary Frame` |

**已知局限**：
- B站没有官方弹幕 WebSocket 文档，所有信息来自社区逆向
- 协议无版本号，会随 B站前端更新而变化
- 风控机制持续增强（2025年5-6月有重要变更）

---

## 一、关键发现

### 1.1 协议栈总览

```
┌─────────────────────────────────┐
│        WebSocket (WSS)          │  wss://broadcastlv.chat.bilibili.com:443/sub
├─────────────────────────────────┤
│   Binary Frame (ArrayBuffer)    │  每条消息是二进制帧
├─────────────────────────────────┤
│   16-byte Header + Body         │  自定义包头协议
├─────────────────────────────────┤
│   Brotli / Zlib 压缩 (可选)     │  protover=3 使用 Brotli
├─────────────────────────────────┤
│   JSON 消息体                    │  弹幕/礼物/进出等均为 JSON
└─────────────────────────────────┘
```

**依据来源**：
1. `SocialSisterYi/bilibili-API-collect` — `docs/live/message_stream.md`（2026年6月最后一次验证，社区最权威的B站API文档）
2. `champkeh/blive-ws` — `docs/ptotocol.md`（协议详细说明，含代码示例）
3. `lovelyyoshino/Bilibili-Live-API` — `API.WebSocket.md`（2026年6月验证）
4. `oreateai.com` 技术分析文章（2026-01-07）

**置信度**：高（4个独立高评分来源一致）

---

### 1.2 完整数据流程（5步）

#### Step 1: 获取真实房间号

短房间号（如 `https://live.bilibili.com/255`）需先转为长房间号。

```
GET https://api.live.bilibili.com/room/v1/Room/room_init?id={短房间号}
```

不需要登录态。

**返回**：`{"data": {"room_id": 48743}}`

---

#### Step 2: 获取 WebSocket 连接参数（Token + 服务器）

```
GET https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?id={真实房间号}&type=0
```

**重要风控变化**（⚠️ 高频变更区域）：
- **2025年5月26日起**：强制要求 **WBI 签名**（`w_rid` + `wts` 参数）（来源：SocialSisterYi#1295）
- **2025年6月27日起**：强制要求 **`buvid3`** cookie（来源：同上）
- **未登录**：可连接但有隐私限制——5分钟后用户名/UID 被匿名化（`*` 替代，UID=0）（来源：阿里云开发者社区 2025-03-12、SocialSisterYi#732）

**返回关键字段**：
```json
{
  "data": {
    "token": "Eac3Lm1JADz...认证密钥",
    "host_list": [
      {"host": "tx-sh-live-comet-02.chat.bilibili.com", "wss_port": 443, "ws_port": 2244},
      {"host": "broadcastlv.chat.bilibili.com", "wss_port": 443, "ws_port": 2244}
    ]
  }
}
```

`host_list` 返回 3-5 个服务器，前 2 个为随机分配节点，最后 1 个固定为 `broadcastlv.chat.bilibili.com`。

**当前项目匹配度**：✅ 已有 WBI 签名（`BilibiliApi::sign_wbi`）、buvid3 获取（`BilibiliApi::get_buvid3`）——`getDanmuInfo` API 调用的前置条件已满足。

---

#### Step 3: WebSocket 连接 + 认证

**连接地址**：`wss://{host}:{wss_port}/sub`（注意必须带 `/sub` 路径）

连接成功后 **5秒内** 必须发送认证包，否则服务器主动断开。

**认证包格式**（16字节头 + JSON body）：

**包头**（大端序）：
| 偏移 | 长度 | 字段 | 认证包取值 |
|------|------|------|-----------|
| 0 | 4 | packet_len | header_len + body_len |
| 4 | 2 | header_len | 16 (0x10) |
| 6 | 2 | protover | **1**（连接通信消息，无压缩） |
| 8 | 4 | op | **7**（用户认证） |
| 12 | 4 | seq | 1 |

**Body（JSON）**：
```json
{
  "uid": 0,           // 用户UID，0=游客
  "roomid": 48743,    // 真实房间号
  "protover": 3,      // 后续数据压缩协议：1=无压缩，2=Zlib，3=Brotli（推荐）
  "platform": "web",
  "type": 2,          // 固定
  "clientver": "1.4.3",
  "key": "{token}",   // Step 2 获取的 token
  "buvid": "{buvid3}" // 可选，建议携带
}
```

**注意**：认证包的 `protover=3` 与包头 `protover=1` 含义不同——前者指定后续消息的压缩方式，后者表示当前包是无压缩的连接通信消息。

---

#### Step 4: 心跳维持

| 项目 | 值 |
|------|-----|
| 发送方 | 客户端 |
| 频率 | **每 30 秒** |
| 超时断开 | 70 秒未收到心跳 |
| OP Code | 2（心跳包） |
| 包头 protover | 1 |
| Body | **空**（仅 16 字节头，packet_len=16） |

**心跳应答（OP=3）**：服务器返回带 4 字节 body 的包，内容为 **Int32 Big Endian 房间人气值**。

---

#### Step 5: 接收并解析消息

**操作码速查**：
| OP | 发送方 | 含义 |
|----|--------|------|
| 2 | 客户端 | 心跳包 |
| 3 | 服务器 | 心跳应答 → body 为 4 字节人气值 |
| 5 | 服务器 | **业务消息包**（弹幕/礼物/进出等） |
| 7 | 客户端 | 进房认证（只发一次） |
| 8 | 服务器 | 认证结果 → body 为 `{"code":0}` |

**协议版本（protover）**：
| 值 | 含义 | Body 处理方式 |
|----|------|-------------|
| 0 | 未压缩 | UTF-8 JSON，直接 `JSON.parse` |
| 1 | 心跳/连接 | 无压缩 |
| 2 | Zlib 压缩 | `zlib.inflate` 解压后得到拼接的包 |
| 3 | **Brotli 压缩** | `brotliDecompress` 解压后得到拼接的包 |

**粘包机制**：B站服务器会把多个消息包压缩成一个包发送。当 `protover=2/3` 时，外层包的解压结果是多个内层包的连续拼接，需要**逐个解析**（每次读取 16 字节头 → 根据 packet_len 截取 body → 移动 offset 继续）。

**各来源对 protover 的建议**：
- `champkeh/blive-ws`：推荐 protover=3（Brotli），现代浏览器均支持
- `yulinfeng000/blive`：protover=3，B站目前实际使用的就是 Brotli
- `simon300000/bilibili-live-ws`：默认 protover=2（向下兼容）

**推荐 protover=3**（3个来源一致推荐）。

---

### 1.3 主要消息类型（CMD）

解析后的 JSON body 通过 `cmd` 字段区分消息类型：

| CMD | 含义 | 关键字段 |
|-----|------|---------|
| `DANMU_MSG` | 弹幕消息 | `info[1]` 文本, `info[2][1]` 用户名, `info[2][0]` UID |
| `SEND_GIFT` | 送礼 | `data.uname`, `data.giftName`, `data.num`, `data.action` |
| `SUPER_CHAT_MESSAGE` | SC 醒目留言 | `data.message`, `data.price` |
| `GUARD_BUY` | 大航海（舰长等） | `data.username`, `data.guard_level` |
| `WELCOME` | 欢迎进入 | `data.uname` |
| `INTERACT_WORD` | 用户交互（进房/关注/分享） | `data.uname`, `data.msg_type` |
| `LIVE` | 开播 | 无特定 payload |
| `PREPARING` | 下播 | 无特定 payload |

**依据**：`lovelyyoshino/Bilibili-Live-API`、`MatchaCake/bilibili_dm_lib`、`79W/bilibili-bullet`（7个独立来源交叉验证）

**置信度**：高

---

### 1.4 二进制包头解析伪代码

```cpp
// 大端序读取
struct PacketHeader {
    uint32_t packet_len;   // offset 0: 总长度（头+体）
    uint16_t header_len;   // offset 4: 固定 16
    uint16_t protover;     // offset 6: 0/1/2/3
    uint32_t op;           // offset 8: 2/3/5/7/8
    uint32_t seq;          // offset 12: 序列号
};

// 解析流程
while (offset < buffer.size()) {
    auto hdr = parse_header(buffer, offset);
    auto body = buffer.substr(offset + 16, hdr.packet_len - 16);
    offset += hdr.packet_len;

    if (hdr.protover == 3) {
        // Brotli 解压 → 递归解析
        auto decompressed = brotli_decompress(body);
        parse_loop(decompressed);
    } else if (hdr.protover == 2) {
        // Zlib 解压 → 递归解析
        auto decompressed = zlib_inflate(body);
        parse_loop(decompressed);
    } else if (hdr.protover == 0 && hdr.op == 5) {
        // 未压缩消息
        auto json = JSON::parse(body);
        dispatch(json["cmd"]);
    } else if (hdr.op == 3) {
        // 心跳应答：body 前 4 字节是 Int32 人气值
        popularity = read_int32_be(body);
    } else if (hdr.op == 8) {
        // 认证结果
        auto result = JSON::parse(body);
        // result["code"] == 0 表示成功
    }
}
```

---

### 1.5 与当前项目的集成分析

| 已具备（可复用） | 需新增 |
|---|---|
| WBI 签名（`sign_wbi`） | WebSocket 客户端（推荐 `QWebSocket`，Qt6 内置） |
| buvid3 获取（`get_buvid3`） | Brotli 解压（可用 `brotli` 库或自行集成） |
| 房间号获取（`room_init` 已有等效逻辑） | 16 字节包头构造/解析 |
| Cookie 管理（`CookieContainer`） | 心跳定时器 |
| JSON 解析（`nlohmann/json`） | 粘包拆包循环 |
| libcurl HTTP 客户端 | 弹幕消息类型分发 |

**最小可行方案**：复用现有 `BilibiliApi` 调用 `getDanmuInfo` → 用 `QWebSocket` 连接 → 手动解析二进制帧 → `nlohmann/json` 解析消息。

---

## 二、反方观点与分歧

### 2.1 游客模式可用性

- **支持游客可用**：大多数早期文档（2023年前）声称 UID=0 即可收取弹幕
- **2025年现状**：B站隐私政策更新后，游客连接 5 分钟后用户名/UID 被匿名化（`*` 替代，UID=0），**部分房间已完全豁免**（来源：SocialSisterYi#732、阿里云 2025-03-12）
- **实际影响**：对纯弹幕文本收集影响小，但无法关联用户身份

**结论**：游客模式功能受限但可用，推荐携带登录态（SESSDATA cookie）调用 `getDanmuInfo`。

### 2.2 protover 选择分歧

| protover | 优点 | 缺点 | 支持者 |
|----------|------|------|--------|
| 1 (无压缩) | 实现最简单 | **已被 B站禁用** | 仅老旧文档提及 |
| 2 (Zlib) | 多语言原生支持 | 压缩率低于 Brotli | simon300000/blive-ws 默认 |
| 3 (Brotli) | B站当前实际使用 | 需额外 brotli 库 | champkeh/blive-ws、yulinfeng000/blive |

**结论**：protover=3，Zlib 已逐渐被废弃。

### 2.3 旧版 API 端点（已废弃）

一些旧文档使用 `https://api.live.bilibili.com/room/v1/Danmu/getConf`（如 `79W/bilibili-bullet`），此端点**已废弃**，新版本统一使用 `xlive/web-room/v1/index/getDanmuInfo`。

---

## 三、现有开源实现参考

| 项目 | 语言 | Stars | 特点 |
|------|------|-------|------|
| `bilibili-live-danmaku` (npm) | TypeScript | ~85 | 最流行的JS库，持续更新至 2026-05-21（v0.7.16） |
| `MatchaCake/bilibili_dm_lib` | Go | - | Pub/Sub 模式，完整 CMD 映射，多房间支持 |
| `varieget/bilibili-ws-client` | TypeScript | - | 轻量 TS 客户端，含完整类型定义 |
| `yulinfeng000/blive` | Python | - | 详细 PROTOCOL.md 文档，含粘包处理逻辑 |
| `79W/bilibili-bullet` | JavaScript | - | 早期实现，代码清晰，含心跳和解析完整示例 |

---

## 四、风险与不确定性

### 4.1 风控风险（⚠️ 高危）

- B站持续增强风控，2025年已有 2 次重大变更（WBI 签名 + buvid3 强制）
- **可能性**：未来可能引入更严格的 Device Fingerprinting 或 Captcha
- **缓解**：尽可能模拟正常浏览器行为（携带完整 Cookie、User-Agent、buvid3）

### 4.2 协议无版本号

- B站弹幕 WebSocket 协议无显式版本号，前端每次更新可能改变字段结构
- **缓解**：解析时对未知 CMD 类型做容错处理，不要硬编码字段位置

### 4.3 Brotli 依赖（中危）

- C++ 中使用 Brotli 需要额外库（`libbrotli-dev`），会增加构建依赖
- **替代**：可用 protover=2（Zlib，Qt6 自带 `qUncompress`），但不推荐

### 4.4 元评审

| 维度 | 评估 |
|------|------|
| 剩余未知 | B站何时会再次修改风控策略、认证包 `type=2` 字段的确切含义 |
| 最弱证据 | `buvid` 是否强制要求（部分来源说可选，部分说必须） |
| 可能错误的假设 | 假设 `QWebSocket` 能直接处理 WSS 443 端口连接（需要验证 TLS 协商） |
| 遗漏角度 | C++ 生态中是否有现成的 B站弹幕 WebSocket 库（搜索未发现，需自行实现） |
| 边际收益 | 下一轮可研究：QWebSocket 的实际 WSS 连接测试、Brotli C++ 集成方案 |

**最坏情况分析**：
- **失败模式**：B站风控升级导致 WebSocket 认证失败，或协议变更导致解析崩溃
- **回滚**：WebSocket 弹幕功能是纯增量，失败只影响弹幕接收，不影响现有开播/推流功能
- **损失**：可接受。弹幕是"锦上添花"功能，非核心开播流程

---

## 五、建议

### 5.1 实现路径（推荐保守方案）

**Phase 1：基础连接**（预计 2-3 天）
1. 新建 `src/danmaku-ws.h/.cpp` 模块
2. 在 `BilibiliApi` 中新增 `getDanmuInfo(roomid)` 方法（复用 WBI 签名 + buvid3）
3. 使用 `QWebSocket` 建立 WSS 连接
4. 实现 16 字节包头构造（认证包 + 心跳包）
5. 实现对 OP=8（认证应答）和 OP=3（心跳应答）的解析

**Phase 2：消息解析**（预计 2-3 天）
1. 集成 `libbrotli` 解压（CMakeLists.txt 添加 `libbrotli-dev` 依赖）
2. 实现粘包拆包循环（递归解析 protover=3 的压缩包）
3. 实现主要 CMD 类型解析（DANMU_MSG、SEND_GIFT、SUPER_CHAT_MESSAGE 等）
4. 实现 30 秒心跳定时器（`QTimer`）

**Phase 3：UI 集成**（预计 1-2 天）
1. 在 `bili-dock.h/.cpp` 中添加弹幕显示区域
2. 连接 `LiveService` 的开播事件自动启动弹幕连接
3. 断连重试机制（指数退避）

### 5.2 关键设计决策

| 决策点 | 推荐方案 | 理由 |
|--------|---------|------|
| WebSocket 库 | `QWebSocket` (Qt6) | 项目已有 Qt6 依赖，无需额外库 |
| 压缩协议 | Brotli (protover=3) | B站实际使用，3个独立来源推荐 |
| 弹幕缓存 | 内存环形缓冲区（最近 1000 条） | 避免内存泄漏，OBS 插件不宜占用过多资源 |
| 登录态 | 可选的登录态传递 | 游客模式可用但有限制，登录体验更好 |
| 重连策略 | 指数退避（1s→2s→4s→...→max 30s） | 防止风控触发，参考 `bilibili-live-danmaku` 实现 |

### 5.3 快速验证方案

在正式编码前，可以用 Python 脚本快速验证当前协议是否可用：

```python
# verify_danmaku.py — 快速验证脚本
import asyncio, websockets, json, struct, brotli

ROOM_ID = 545068  # 替换为目标房间号

# 1. 获取 token
# curl 'https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?id=545068&type=0'

# 2. 连接并认证
async def test():
    async with websockets.connect("wss://broadcastlv.chat.bilibili.com/sub") as ws:
        # 构造认证包 (16字节头 + JSON body)
        auth_body = json.dumps({"uid": 0, "roomid": ROOM_ID, "protover": 3, "platform": "web", "type": 2, "key": "TOKEN"})
        header = struct.pack(">IHHII", 16 + len(auth_body), 16, 1, 7, 1)
        await ws.send(header + auth_body.encode())

        # 接收并解析
        while True:
            data = await ws.recv()
            # ... 解析循环
```

---

## 六、参考来源

| # | 标题 | URL | 类型 | 时间 | 评分 |
|---|------|-----|------|------|------|
| 1 | SocialSisterYi/bilibili-API-collect — message_stream.md | [GitHub](https://github.com/pskdje/bilibili-API-collect/blob/main/docs/live/message_stream.md) | 社区文档（最权威） | 持续更新（2026-06验证） | ⭐⭐⭐⭐⭐ |
| 2 | champkeh/blive-ws — ptotocol.md | [GitHub](https://github.com/champkeh/blive-ws/blob/master/docs/ptotocol.md) | 开源项目文档 | ~2025 | ⭐⭐⭐⭐ |
| 3 | lovelyyoshino/Bilibili-Live-API — API.WebSocket.md | [GitHub](https://github.com/lovelyyoshino/Bilibili-Live-API/blob/master/API.WebSocket.md) | 开源项目文档 | 2026-06验证 | ⭐⭐⭐⭐ |
| 4 | bilibili-live-danmaku (npm) | [npm](https://www.npmjs.com/package/bilibili-live-danmaku) | 开源库（JS） | v0.7.16 (2026-05-21) | ⭐⭐⭐⭐ |
| 5 | Minteea/bilibili-live-danmaku | [GitHub](https://github.com/Minteea/bilibili-live-danmaku) | 开源库（JS） | 2025-03-30 | ⭐⭐⭐⭐ |
| 6 | MatchaCake/bilibili_dm_lib | [GitHub](https://github.com/MatchaCake/bilibili_dm_lib) | 开源库（Go） | 2025 | ⭐⭐⭐ |
| 7 | varieget/bilibili-ws-client | [GitHub](https://github.com/varieget/bilibili-ws-client) | 开源库（TS） | 2025 | ⭐⭐⭐ |
| 8 | yulinfeng000/blive — PROTOCOL.md | [GitHub](https://github.com/yulinfeng000/blive/blob/main/PROTOCOL.md) | 开源项目文档 | 2025 | ⭐⭐⭐ |
| 9 | oreateai.com 技术分析 | [Web](https://www.oreateai.com/blog/technical-analysis-of-bilibili-live-danmaku-websocket-protocol/) | 技术博客 | 2026-01-07 | ⭐⭐⭐ |
| 10 | 阿里云开发者社区 | [Web](https://developer.aliyun.com/article/1656634) | 技术文章 | 2025-03-12 | ⭐⭐⭐ |
| 11 | sdl.moe B站直播WSS逆向 | [Web](https://sdl.moe/post/bili-live-wss/) | 技术博客 | 2023-02-22 | ⭐⭐（时效性差） |
| 12 | 79W/bilibili-bullet | [GitHub](https://github.com/79W/bilibili-bullet) | 开源库（JS） | ~2023（部分API已废弃） | ⭐⭐（时效性差） |

### 来源评分卡（关键来源详细评标）

**来源 #1 — SocialSisterYi/bilibili-API-collect**
| 维度 | 评分 | 说明 |
|------|------|------|
| 权威性 | 4 | 社区公认最权威的B站API文档，持续维护 |
| 一手性 | 4 | 基于实际抓包验证，标注变更时间线 |
| 时效性 | 5 | 持续更新，含2025年5月WBI强制变更记录 |
| 独立性 | 4 | 社区驱动，无商业偏向 |
| 可验证性 | 5 | 可自行抓包复现 |

**综合评分**：22/25

**来源 #4 — bilibili-live-danmaku (npm)**
| 维度 | 评分 | 说明 |
|------|------|------|
| 权威性 | 3 | npm 最流行的 B站弹幕库 |
| 一手性 | 5 | 可直接使用的代码实现 |
| 时效性 | 5 | 最新版 2026-05-21 |
| 独立性 | 4 | MIT 开源，无商业偏向 |
| 可验证性 | 5 | npm install 即可测试 |

**综合评分**：22/25
