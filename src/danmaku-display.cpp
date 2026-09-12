#include "danmaku-display.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

// ── 自定义 HTML 富文本 Item Delegate ──
class DanmakuHtmlDelegate : public QStyledItemDelegate {
public:
    explicit DanmakuHtmlDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        // 绘制背景（如 SC 醒目留言的高亮背景色）
        QVariant bg = index.data(Qt::BackgroundRole);
        if (bg.canConvert<QBrush>()) {
            painter->fillRect(opt.rect, bg.value<QBrush>());
        }

        // 富文本渲染
        QTextDocument doc;
        doc.setDocumentMargin(2);
        doc.setDefaultStyleSheet("body { font-family: sans-serif; font-size: 12px; }");
        doc.setHtml(opt.text);
        doc.setTextWidth(opt.rect.width());

        painter->translate(opt.rect.topLeft());
        QRect clip(0, 0, opt.rect.width(), opt.rect.height());
        doc.drawContents(painter, clip);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        QTextDocument doc;
        doc.setDocumentMargin(2);
        doc.setDefaultStyleSheet("body { font-family: sans-serif; font-size: 12px; }");
        doc.setHtml(opt.text);
        int width = opt.rect.width() > 0 ? opt.rect.width() : 200;
        doc.setTextWidth(width);
        return QSize(width, static_cast<int>(std::ceil(doc.size().height())) + 2);
    }
};

DanmakuDisplay::DanmakuDisplay(QWidget *parent)
    : QWidget(parent)
{
    qRegisterMetaType<DanmakuMessage>("DanmakuMessage");
    qRegisterMetaType<GiftMessage>("GiftMessage");
    qRegisterMetaType<SuperChatMessage>("SuperChatMessage");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(2);

    // ── 状态栏 ──
    auto *status_row = new QHBoxLayout();
    status_label_ = new QLabel("[已关闭]");
    status_label_->setStyleSheet(
        "color: #888; font-size: 11px; padding: 0 4px;");
    status_row->addWidget(status_label_);

    btn_toggle_ = new QPushButton();
    btn_toggle_->setFixedWidth(46);
    btn_toggle_->setStyleSheet("QPushButton { padding: 1px 4px; font-size: 11px; }");
    connect(btn_toggle_, &QPushButton::clicked, this, [this]() {
        if (state_ == DanmakuWebSocket::State::Connected) {
            emit disconnect_requested();
        } else if (state_ == DanmakuWebSocket::State::Closed) {
            emit reconnect_requested();
        }
    });
    status_row->addWidget(btn_toggle_);
    apply_state_to_controls();

    status_row->addStretch();

    popularity_label_ = new QLabel("人气: 0");
    popularity_label_->setStyleSheet(
        "color: #FFB74D; font-size: 11px; padding: 0 4px;");
    status_row->addWidget(popularity_label_);

    layout->addLayout(status_row);

    // ── 弹幕列表 ──
    list_widget_ = new QListWidget();
    list_widget_->setItemDelegate(new DanmakuHtmlDelegate(list_widget_));
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

void DanmakuDisplay::append_danmaku(const DanmakuMessage &msg)
{
    // 昵称颜色：舰长（大航海）红色 > 粉丝团按勋章等级渐变（橘黄→亮金）> 普通用户默认色
    QColor name_color("#e0e0e0");
    if (msg.guard_level > 0) {
        name_color = QColor("#FF5252");   // 舰长：红
    } else if (!msg.fan_badge.empty()) {
        // 勋章等级 1→40，HSL 线性插值：橘黄 #FF9800 → 亮金 #FFD54F
        int level = std::clamp(msg.fan_badge_level, 1, 40);
        const double t = (level - 1) / 39.0;
        const QColor low("#FF9800"), high("#FFD54F");
        name_color = QColor::fromHslF(
            low.hueF() + (high.hueF() - low.hueF()) * t,
            low.saturationF() + (high.saturationF() - low.saturationF()) * t,
            low.lightnessF() + (high.lightnessF() - low.lightnessF()) * t);
    }

    // 富文本：仅昵称着色，消息文本保持默认色
    QString text = QString("<span style=\"color:%1\">%2</span>: <span style=\"color:#e0e0e0\">%3</span>")
        .arg(name_color.name(),
             QString::fromStdString(msg.username).toHtmlEscaped(),
             QString::fromStdString(msg.message).toHtmlEscaped());
    auto *item = new QListWidgetItem(text);
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_gift(const GiftMessage &msg)
{
    QString text = QString("<span style=\"color:#4FC3F7\">[礼物] %1 %2 %3%4%5</span>")
        .arg(QString::fromStdString(msg.username).toHtmlEscaped(),
             QString::fromStdString(msg.action).toHtmlEscaped(),
             QString::fromStdString(msg.gift_name).toHtmlEscaped())
        .arg(msg.num > 1 ? QString(" x%1").arg(msg.num) : "")
        .arg(msg.combo_num > 1 ? QString(" (连送%1)").arg(msg.combo_num) : "");
    auto *item = new QListWidgetItem(text);
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_super_chat(const SuperChatMessage &msg)
{
    double yuan = msg.price / 100.0;
    QString text = QString("<span style=\"color:#FFB74D\">[SC ¥%1] %2: %3</span>")
        .arg(yuan, 0, 'f', 2)
        .arg(QString::fromStdString(msg.username).toHtmlEscaped(),
             QString::fromStdString(msg.message).toHtmlEscaped());
    auto *item = new QListWidgetItem(text);
    item->setBackground(QColor("#3d2e1a"));  // 深色背景
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_guard(const GuardMessage &msg)
{
    QString guard_title = "舰长";
    if (msg.guard_level == 1) guard_title = "总督";
    else if (msg.guard_level == 2) guard_title = "提督";

    QString text = QString("<span style=\"color:#FF5252; font-weight:bold;\">[大航海] 恭喜 %1 开通 %2 x%3%4！</span>")
        .arg(QString::fromStdString(msg.username).toHtmlEscaped(),
             guard_title,
             QString::number(msg.guard_num),
             QString::fromStdString(msg.guard_unit));
    auto *item = new QListWidgetItem(text);
    item->setBackground(QColor("#4A2328"));
    list_widget_->insertItem(0, item);
    trim_items();
}

void DanmakuDisplay::append_like(const LikeMessage &msg)
{
    QString text = QString("<span style=\"color:#F48FB1;\">[点赞] %1 为主播点赞了 (x%2)</span>")
        .arg(QString::fromStdString(msg.username).toHtmlEscaped(),
             QString::number(msg.like_count));
    auto *item = new QListWidgetItem(text);
    list_widget_->insertItem(0, item);
    trim_items();
}

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

void DanmakuDisplay::set_connection_state(DanmakuWebSocket::State state)
{
    state_ = state;
    apply_state_to_controls();
}

// 文案与按钮跟随连接状态：已连接→可关闭，连接中→置灰，已关闭→可连接
void DanmakuDisplay::apply_state_to_controls()
{
    switch (state_) {
    case DanmakuWebSocket::State::Connected:
        status_label_->setText("[已连接]");
        status_label_->setStyleSheet(
            "color: #81C784; font-size: 11px; padding: 0 4px; font-weight: bold;");
        btn_toggle_->setText("关闭");
        btn_toggle_->setToolTip("关闭弹幕互动（关闭后不再自动重连）");
        btn_toggle_->setEnabled(true);
        break;
    case DanmakuWebSocket::State::Connecting:
        status_label_->setText("[连接中...]");
        status_label_->setStyleSheet(
            "color: #FFB74D; font-size: 11px; padding: 0 4px;");
        btn_toggle_->setText("连接中");
        btn_toggle_->setToolTip("正在连接直播间弹幕服务，请稍候");
        btn_toggle_->setEnabled(false);   // 置灰，避免重复触发 start_app
        break;
    case DanmakuWebSocket::State::Closed:
    default:
        status_label_->setText("[已关闭]");
        status_label_->setStyleSheet(
            "color: #888; font-size: 11px; padding: 0 4px;");
        btn_toggle_->setText("连接");
        btn_toggle_->setToolTip("连接直播间弹幕服务，进入弹幕互动");
        btn_toggle_->setEnabled(true);
        break;
    }
}

void DanmakuDisplay::set_status_text(const QString &text, const QString &style)
{
    status_label_->setText(text);
    if (!style.isEmpty()) {
        status_label_->setStyleSheet(style);
    }
}

void DanmakuDisplay::clear_all()
{
    list_widget_->clear();
}
