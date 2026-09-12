# ADR-002: 弹幕 WebSocket 客户端架构设计

## 状态

已提议

## 上下文

项目需要新增 B站直播弹幕实时显示功能。根据 `docs/research/bilibili-live-danmaku-websocket.md` 的调研结论，B站弹幕通过 WebSocket 协议推送，需要集成 Brotli 解压、二进制包头解析、心跳维持、断连重试等机制。项目已有 Qt6::Network（含 QWebSocket）、libcurl、nlohmann/json，但缺少 WebSocket 客户端和 Brotli 解压支持。

需要在以下方面做出架构决策：WebSocket 库选择、Brotli 集成方式、弹幕缓存策略、错误隔离与重连机制。

## 决定

### 决定 1：使用 Qt6 QWebSocket 作为 WebSocket 客户端

**选择**：使用 Qt6 内置的 `QWebSocket`（`Qt6::Network` 模块），不引入第三方 WebSocket 库。

**替代方案及排除理由**：

| 方案 | 排除理由 |
|------|----------|
| libwebsockets (C) | 新增 C 依赖，需要自定义事件循环集成到 Qt，回调风格与 Qt 信号/槽不匹配 |
| boost::beast | 引入整个 Boost 依赖链，OBS 插件二进制体积和编译时间显著增加 |
| ixwebsocket | 较新的 C++ 库，生态成熟度不如 Qt，项目已有 Qt6 完全够用 |
| 纯 libcurl + 手动 WSS | libcurl 不支持 WebSocket 协议，需从零实现帧协议和 TLS 握手 |

**理由**：
- 项目已依赖 `Qt6::Network`（`bili-dock.cpp` 用于 `QNetworkAccessManager` 加载头像），引入 `QWebSocket` 无需新依赖
- `QWebSocket` 原生支持 WSS（TLS 协商由 Qt 自动处理），提供 `binaryMessageReceived` 信号直接接收二进制帧
- Qt 信号/槽机制天然适合异步事件驱动的 WebSocket 客户端
- 实现成本最低：仅需连接信号、构造/解析包头，无需事件循环适配代码

### 决定 2：使用 libbrotli-dev 系统库进行 Brotli 解压

**选择**：通过 pkg-config 查找 `libbrotlienc` 和 `libbrotlidec`（`libbrotli-dev` 包），在 CMakeLists.txt 中添加依赖。不使用 protover=2 (Zlib) 作为主协议。

**替代方案及排除理由**：

| 方案 | 排除理由 |
|------|----------|
| protover=2 (Zlib) | B站已逐渐废弃 Zlib 压缩，虽然 Qt6 自带 `qUncompress` 可解压 Zlib，但不推荐作为长期方案 |
| 静态编译 brotli 源码 | 增加项目构建复杂度，需要维护 vendored 依赖的版本更新 |
| Conan/vcpkg 包管理 | 项目当前使用系统包管理器（pkg-config），引入包管理器会破坏构建一致性 |
| protover=1 (无压缩) | 已被 B站禁用，无法建立连接 |

**理由**：
- `libbrotli-dev` 在主流发行版均有维护（Debian/Ubuntu/Fedora/Arch）
- 解压 API 简单：`BrotliDecoderDecompress(compressed_size, compressed_data, &uncompressed_size, uncompressed_buffer)`
- B站目前实际使用 protover=3（Brotli），3 个独立开源项目（champkeh/blive-ws、yulinfeng000/blive、bilibili-live-danmaku npm 包）一致推荐
- 运行时依赖 `libbrotli1` 是轻量级共享库（~300KB），符合 OBS 插件资源约束

### 决定 3：使用固定大小环形缓冲区缓存弹幕

**选择**：`std::vector<DanmakuMessage>` 预分配 1000 个元素，通过 `cache_write_pos_` 循环写入实现环形缓冲区。不动态分配，不清理旧条目。

**替代方案及排除理由**：

| 方案 | 排除理由 |
|------|----------|
| `std::deque` + append/pop | 每次追加可能触发重新分配，pop 后内存不归还，不如固定预分配可控 |
| `std::list` 链表 | 每次插入一个 `new`，大量小对象分配导致内存碎片，OBS 插件应避免 |
| `QList` 无限追加 | 长时间直播（数小时）累积数万条弹幕，内存持续增长，OBS 可能被 kill |
| 文件/磁盘缓存 | OBS 插件不应产生磁盘 I/O，弹幕数据无持久化需求 |

**理由**：
- 1000 条上限确保内存占用恒定（约 1000 × 200 bytes ≈ 200KB）
- 环形缓冲区写入是 `O(1)`，无分配/释放开销
- OBS 插件运行在 OBS 进程内，内存受限（OBS 自身已使用大量 GPU/系统内存），弹幕功能必须保持低内存占用
- 客户端弹幕展示只需最近数百条，历史滚动无意义（B站官方 Web 端也只展示有限条数）

### 决定 4：指数退避重连策略（1s → 2s → 4s → ... → max 30s）

> **已被取代**：本决定已由 [ADR-005](2026-09-13-danmaku-no-auto-reconnect.md) 取代。当前实现不自动重连，连接关闭即退出弹幕互动，需用户手动点击「重连」。以下内容为历史记录。

**选择**：连接断开后启动重连定时器，延迟为 `min(1000ms × 2^attempt, 30000ms)`，每次重连前重新调用 `getDanmuInfo` 获取新 token。

**替代方案及排除理由**：

| 方案 | 排除理由 |
|------|----------|
| 固定间隔（如 5s） | 高频重连可能触发 B站风控，导致 IP 被暂时限制 |
| 无限重试 | 需要上限防止永久运行（如 token 过期/房间关闭） |
| 不重连 | 对用户不友好，短暂网络抖动导致弹幕永久丢失 |
| 仅重连 3 次 | 长期直播（数小时）中偶然断开后弹幕无法恢复 |

**理由**：
- 指数退避是 WebSocket 客户端的行业标准实践（参考 `bilibili-live-danmaku` npm 包的实现）
- 每次重连前重新获取 token：B站 token 有时效性（约 2 小时），直接复用旧 token 可能认证失败
- max 30s 上限：避免长时间等待，30s 后如果仍然失败，用户可手动重连（重新开播）
- 重连次数无上限但用户主动 `disconnect_from_room()` 时终止：区分"意外断开"与"用户主动断开"

### 决定 5：弹幕功能与核心开播功能完全隔离

**选择**：`DanmakuWebSocket` 为独立类，通过 Qt 信号/槽与 `BiliDock` 通信。弹幕相关的任何异常（HTTP 失败、WebSocket 断开、解压错误、JSON 解析失败）均不向上传播，不影响 `LiveService` 和 OBS 推流。

**替代方案及排除理由**：

| 方案 | 排除理由 |
|------|----------|
| 弹幕 WebSocket 集成到 `BiliDock` 内部 | 违反单一职责原则，`BiliDock` 已承担 UI 布局 + OBS 流配置 + 账号管理等职责 |
| 弹幕失败时弹窗警告 | OBS 插件应静默辅助，弹窗打断用户流程 |
| 在 `LiveService` 中管理弹幕连接 | `LiveService` 是业务逻辑（开播/停播），不应关心 WebSocket 协议细节 |

**理由**：
- 弹幕是"锦上添花"的增值功能（非核心），失败不应影响核心开播和推流
- 独立类便于单元测试（可 mock `QWebSocket` 和 `BilibiliApi`）
- 信号/槽解耦：若未来弹幕功能需要插件化（可在编译时禁用），只需移除 `danmaku-ws.cpp` 文件，不影响其他模块
- 遵循项目现有分层模式（`BilibiliApi` ↔ `*Service` ↔ `BiliDock`，每层通过指针注入）

### 决定 6：buvid3 在首次 `getDanmuInfo` 前获取并缓存

**选择**：`DanmakuWebSocket` 在 `connect_to_room()` 中检查 `buvid3_`，若为空则调用 `api_->get_buvid3()`，将返回值缓存为成员变量，后续重连复用之。

**替代方案及排除理由**：

| 方案 | 排除理由 |
|------|----------|
| 每次连接时重新获取 buvid3 | 增加不必要的 HTTP 调用，buvid3 是设备级标识，session 内稳定 |
| 存到 `SessionState` 供全局共享 | buvid3 仅弹幕功能使用，不应污染全局状态结构体 |
| 存到 `ConfigManager` 磁盘持久化 | 增加配置复杂度，buvid3 从 API 获取极其廉价 |

**理由**：
- `get_buvid3()` 返回快（<200ms），首次调用阻塞时间可接受
- 内存缓存避免重复 HTTP 调用，符合"最小化网络请求"原则
- 若未来 buvid3 过期（目前未观察到），重连时 `getDanmuInfo` 失败会触发 `get_buvid3()` 重新获取的改进方案也极易实施

## 后果

### 正面

1. **零 Qt 层面新依赖**：QWebSocket 属于 Qt6::Network，已安装
2. **仅 1 个系统级新依赖**：`libbrotli-dev`（编译时）+ `libbrotli1`（运行时），Debian/Ubuntu 直接 apt 安装
3. **低内存占用**：环形缓冲区固定 200KB，WebSocket 连接本身 < 1MB 内存
4. **完全隔离**：弹幕相关代码在独立文件中，编译时可选禁用，运行时失败不影响推流
5. **可观测**：所有连接状态变化和错误通过 `blog()` 记录到 OBS 日志
6. **容错**：Brotli 解压失败、JSON 解析失败均只影响单条消息，不中断连接
7. **遵循现有模式**：模块分层和依赖注入与项目现有 `*Service` 模式一致

### 负面

1. **libbrotli 运行时依赖**：用户需安装 `libbrotli1` 包（已在 Debian/Ubuntu 桌面版默认安装，概率高）
2. **UI 线程阻塞**：`getDanmuInfo` HTTP 调用和 `get_buvid3` 在 UI 线程执行（同步 libcurl 调用），阻塞约 500ms-2s。未来可升级为 `std::async` + `QMetaObject::invokeMethod` 异步方案
3. **QListWidget 大消息量场景**：极端场景（100+/秒弹幕）下 QListWidget 追加/删除可能引起 UI 微卡。未来可升级为 QListView + 自定义 Model 虚拟化渲染
4. **TCP 连接常驻**：弹幕 WebSocket 连接在直播期间持续保持（约每 30s 一次心跳收发，总计 < 200 bytes），消耗一个 TCP 连接和少量 CPU

### 后续优化

- **异步 HTTP 调用**：将 `getDanmuInfo` 和 `get_buvid3` 包装到 `std::async`，结果通过 `QMetaObject::invokeMethod` 投递回主线程（约 +40 行代码）
- **QListView 虚拟化**：若弹幕速率持续 > 50/秒，替换 `QListWidget` 为 `QListView` + 自定义 `QAbstractListModel`（约 +80 行代码）
- **消息过滤**：支持按消息类型过滤（仅显示弹幕/礼物/SC），减少渲染负载
- **编译开关**：`option(ENABLE_DANMAKU "Enable danmaku display" ON)` CMake 选项，允许发行版禁用此功能

## 风险

| 风险 | 缓解 |
|------|------|
| B站风控升级导致 WebSocket 认证失败 | 弹幕功能降级，开播/推流不受影响；日志记录详细错误信息便于诊断 |
| libbrotli API 版本不兼容 | 使用稳定 API `BrotliDecoderDecompress`（v1.0+ 无破坏性变更） |
| 房间号变更时重连到旧房间 | `connect_to_room()` 被调用时取消当前连接，仅允许一个活跃连接 |
| QWebSocket WSS 握手在特定网络环境失败 | 通过 `sslErrors` 信号记录详细 SSL 错误；OBS 插件在用户桌面环境运行，TLS 通常正常 |
