#pragma once

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>

#include "danmaku-ws.h"

class DanmakuDisplay : public QWidget {
    Q_OBJECT

public:
    explicit DanmakuDisplay(QWidget *parent = nullptr);
    void set_max_visible_items(int count);

    // 不设最小高度：QSplitter 空间不足时由弹幕区整体收缩，保证上方设置区完整显示
    QSize minimumSizeHint() const override { return {0, 0}; }

public slots:
    void append_danmaku(const DanmakuMessage &msg);
    void append_gift(const GiftMessage &msg);
    void append_super_chat(const SuperChatMessage &msg);
    void append_guard(const GuardMessage &msg);
    void append_like(const LikeMessage &msg);
    void set_popularity(int popularity);
    void set_connected(bool connected);
    void set_status_text(const QString &text, const QString &style = "");
    void clear_all();

signals:
    void reconnect_requested();

private:
    void trim_items();

    QListWidget *list_widget_;
    QLabel *popularity_label_;
    QLabel *status_label_;
    QPushButton *btn_reconnect_;
    int max_visible_ = 200;
};
