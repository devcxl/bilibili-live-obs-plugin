# B站直播工具（bilibili-live-obs-plugin）

OBS Studio 插件上下文：把 B 站直播的开播/停播、直播间弹幕互动与语音播报接入 OBS 停靠面板。

## Language

**登录会话（login session）**：
B站账号在插件中的登录态（uid、csrf、房间号缓存），由扫码登录建立、登出时注销。
_Avoid_: 会话、session
（代码标识：`SessionState`、`UserService`）

**弹幕互动（danmaku interaction）**：
用户开启弹幕接收后，插件与官方长连保持连接、持续展示直播间实时消息的整体状态；用户可随时关闭，关闭后不再自动重连。
_Avoid_: 弹幕会话、互动会话
（代码标识：`DanmakuWebSocket::session_active_`）

**官方项目（Open Live project）**：
开发者在 B站直播开放平台注册的互动应用，凭 App ID、AccessKey、Secret 与主播身份码接入。
_Avoid_: 应用、App

**官方场次（game session）**：
官方项目的一次 `start_app` 到 `end_app` 的生命周期，以 `game_id` 标识；一个直播间同时只能存在一个。
_Avoid_: 会话、场

**集群链接（cluster link）**：
官方 `start_app` 返回的长连地址列表（`wss_link`），多个地址互为不同集群的镜像，官方要求生产环境实现连接失败后的切换兜底。
_Avoid_: 主机、服务器地址

**连接状态机（connection state machine）**：
弹幕长连的 Closed / Connecting / Connected 三态，用于区分网络抖动（自动退避重连）与用户主动关闭（不再重连）。
_Avoid_: 连接状态、状态位

**疑似冷却（suspected cooldown）**：
官方拒绝 `start_app` 的一类失败语义（代码按错误码 7001 处理），官方文档未证实该错误码含义；进入该分支时退避按官方「未正常 end 需间隔 1 分钟」对齐。
_Avoid_: 限流、封禁
