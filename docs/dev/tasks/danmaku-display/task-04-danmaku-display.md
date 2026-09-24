---
name: "task-04-danmaku-display"
depends_on: ["task-01-cmake-and-structs"]
labels: ["frontend"]
worktree_root: ".worktree/task-04-danmaku-display/"
test_commands: []
verify_commands:
  - "cmake --build build --target bilibili-live-obs 2>&1 | tail -5"
tdd:
  mode: manual
  min_cycles: 1
acceptance:
  - criteria: "danmaku-display.h 定义 DanmakuDisplay 类，继承 QWidget"
    verification_type: manual
  - criteria: "danmaku-display.cpp 实现 QListWidget 弹幕列表 + popularity_label 人气值 + status_label 状态"
    verification_type: manual
  - criteria: "append_danmaku / append_gift / append_super_chat 追加消息到列表，超出 max_visible 自动裁剪"
    verification_type: manual
  - criteria: "不同消息类型使用不同前缀标识（如 [弹幕] / [礼物] / [SC]）"
    verification_type: manual
  - criteria: "编译通过"
    verification_type: lint
---

# Task 4: DanmakuDisplay UI 控件实现

## 目标概述

实现 `DanmakuDisplay` QWidget 弹幕展示面板，包含弹幕列表（QListWidget）、人气值标签、连接状态标签。通过 slot 接收三种消息类型（弹幕/礼物/SC）并追加到列表。

## 涉及文件

| 文件 | 修改内容 | 变更量 |
|------|----------|--------|
| `src/danmaku-display.h`（新建） | DanmakuDisplay 类定义 | ~30 行 |
| `src/danmaku-display.cpp`（新建） | UI 布局 + slot 实现 + 裁剪逻辑 | ~100 行 |

## 实现步骤

### 步骤 1：danmaku-display.h — 类定义

```cpp
#pragma once

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include "danmaku-ws.h"

class DanmakuDisplay : public QWidget {
    Q_OBJECT
public:
    explicit DanmakuDisplay(QWidget *parent = nullptr);

    void set_max_visible_items(int count);
    void set_popularity(int popularity);
    void set_connected(bool connected);

public slots:
    void append_danmaku(const DanmakuMessage &msg);
    void append_gift(const GiftMessage &msg);
    void append_super_chat(const SuperChatMessage &msg);
    void clear_all();

private:
    void trim_items();

    QListWidget *list_widget_;
    QLabel *popularity_label_;
    QLabel *status_label_;
    int max_visible_ = 500;
};
```

### 步骤 2：danmaku-display.cpp — 构造函数 + UI 布局

```cpp
#include "danmaku-display.h"
#include <QVBoxLayout>

DanmakuDisplay::DanmakuDisplay(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(2);

    // ── 状态栏 ──
    auto *status_row = new QHBoxLayout();
    status_label_ = new QLabel("已断开");
    status_label_->setStyleSheet(
        "color: #888; font-size: 11px; padding: 0 4px;");
    status_row->addWidget(status_label_);

    status_row->addStretch();

    popularity_label_ = new QLabel("人气: 0");
    popularity_label_->setStyleSheet(
        "color: #FFB74D; font-size: 11px; padding: 0 4px;");
    status_row->addWidget(popularity_label_);

    layout->addLayout(status_row);

    // ── 弹幕列表 ──
    list_widget_ = new QListWidget();
    list_widget_->setStyleSheet(
        "QListWidget {"
        "  background: #2b2b2b;"
        "  border: 1px solid #444;"
        "  border-radius: 4px;"
        "  color: #e0e0e0;"
        "  font-size: 12px;"
        "}"
        "QListWidget::item {"
        "  padding: 2px 4px;"
        "  border-bottom: 1px solid #333;"
        "}"
    );
    list_widget_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_widget_->setSelectionMode(QAbstractItemView::NoSelection);
    list_widget_->setFocusPolicy(Qt::NoFocus);

    layout->addWidget(list_widget_);
}
```

### 步骤 3：danmaku-display.cpp — 消息追加槽

```cpp
void DanmakuDisplay::append_danmaku(const DanmakuMessage &msg)
{
    QString text;
    if (msg.fan_badge.empty()) {
        text = QString::fromStdString(msg.username + ": " + msg.message);
    } else {
        text = QString("[%1|%2] %3: %4")
            .arg(QString::fromStdString(msg.fan_badge))
            .arg(msg.fan_badge_level)
            .arg(QString::fromStdString(msg.username),
                 QString::fromStdString(msg.message));
    }
    auto *item = new QListWidgetItem(text);
    item->setForeground(QColor("#e0e0e0"));
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_gift(const GiftMessage &msg)
{
    QString text = QString("[礼物] %1 %2 %3%4")
        .arg(QString::fromStdString(msg.username),
             QString::fromStdString(msg.action),
             QString::fromStdString(msg.gift_name))
        .arg(msg.num > 1 ? QString(" x%1").arg(msg.num) : "");
    if (msg.combo_num > 1) {
        text += QString(" (连送%1)").arg(msg.combo_num);
    }
    auto *item = new QListWidgetItem(text);
    item->setForeground(QColor("#4FC3F7"));  // 蓝色
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_super_chat(const SuperChatMessage &msg)
{
    QString text = QString("[SC ¥%1] %2: %3")
        .arg(msg.price)
        .arg(QString::fromStdString(msg.username),
             QString::fromStdString(msg.message));
    auto *item = new QListWidgetItem(text);
    item->setForeground(QColor("#FFB74D"));  // 金色
    item->setBackground(QColor("#3d2e1a"));  // 深色背景
    list_widget_->insertItem(0, item);
    trim_items();
}
```

### 步骤 4：danmaku-display.cpp — 裁剪 + 工具方法

```cpp
void DanmakuDisplay::trim_items()
{
    while (list_widget_->count() > max_visible_) {
        delete list_widget_->takeItem(list_widget_->count() - 1);
    }
}

void DanmakuDisplay::set_max_visible_items(int count)
{
    max_visible_ = std::max(50, std::min(count, 1000));
    trim_items();
}

void DanmakuDisplay::set_popularity(int popularity)
{
    popularity_label_->setText(
        QString("人气: %1").arg(popularity));
}

void DanmakuDisplay::set_connected(bool connected)
{
    if (connected) {
        status_label_->setText("已连接");
        status_label_->setStyleSheet(
            "color: #4CAF50; font-size: 11px; padding: 0 4px;");
    } else {
        status_label_->setText("已断开");
        status_label_->setStyleSheet(
            "color: #888; font-size: 11px; padding: 0 4px;");
    }
}

void DanmakuDisplay::clear_all()
{
    list_widget_->clear();
}
```

### 步骤 5：验证编译

```bash
cmake --build build --target bilibili-live-obs 2>&1 | tail -5
```

预期：编译通过。如果 linker 报 undefined symbol to `DanmakuWebSocket` 方法（danmaku-ws.cpp 缺实现），属预期 — Task 3 完成后解决。

## 设计说明

1. **消息排序**：新消息通过 `insertItem(0, ...)` 插入列表顶部（最新在上），符合聊天室习惯
2. **裁剪策略**：超出 `max_visible_` 后从列表底部（最旧）删除，保持性能
3. **颜色区分**：
   - 弹幕：灰色 `#e0e0e0`
   - 礼物：蓝色 `#4FC3F7`
   - SC：金色 `#FFB74D` + 深色背景 `#3d2e1a`
4. **样式**：深色主题，与 OBS 原生 UI 风格一致

## 验收

参考 frontmatter `acceptance` 字段中的 5 条验收标准。
