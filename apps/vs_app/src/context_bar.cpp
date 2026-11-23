#include "context_bar.hpp"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "actions.hpp"
#include "icon_painter.hpp"

namespace vs::app {

namespace {

QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString button_name(Context context, ToolId id) {
  return QStringLiteral("button_%1_%2").arg(qs(to_string(context)), qs(tool(id).name));
}

QString more_name(Context context, std::size_t group) {
  return QStringLiteral("more_%1_%2").arg(qs(to_string(context))).arg(group);
}

std::size_t index_of(Context c) { return static_cast<std::size_t>(c); }

// The tab row's buttons: the current tab underlined in the accent colour.
constexpr auto kTabStyle =
    "QToolButton { border: none; border-bottom: 2px solid transparent; padding: 4px 12px; }"
    "QToolButton:checked { border-bottom: 2px solid palette(highlight); font-weight: bold; }"
    "QToolButton:hover:!checked { border-bottom: 2px solid palette(mid); }"
    "QToolButton:disabled { color: palette(mid); }";

constexpr auto kToolStyle = "QToolButton { padding: 2px 4px; min-width: 44px; }";
// Icons only, in a window too narrow for the labels.
constexpr auto kCompactStyle = "QToolButton { padding: 2px; }";

}  // namespace

ContextBar::ContextBar(const ToolActions& actions, QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("context_bar"));
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(4, 0, 4, 2);
  outer->setSpacing(0);

  auto* row = new QHBoxLayout;
  row->setSpacing(2);
  for (Context c : kContexts) {
    auto* t = new QToolButton;
    t->setObjectName(QStringLiteral("tab_") + qs(to_string(c)));
    t->setText(qs(title(c)));
    t->setCheckable(true);
    t->setAutoRaise(true);
    t->setStyleSheet(QString::fromLatin1(kTabStyle));
    t->setFocusPolicy(Qt::NoFocus);
    connect(t, &QToolButton::clicked, this, [this, c] {
      // Only the window changes the context: put the check back first.
      for (Context k : kContexts) tabs_.at(index_of(k))->setChecked(k == context_);
      if (c != context_) emit tabRequested(c);
    });
    tabs_.at(index_of(c)) = t;
    row->addWidget(t);
  }
  detail_ = new QLabel;
  detail_->setObjectName(QStringLiteral("context_detail"));
  detail_->setStyleSheet(QStringLiteral("QLabel { padding: 0 10px; color: palette(dark); }"));
  // Long details are cut at the edge, never widening the window.
  detail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  row->addWidget(detail_, 1);
  corner_ = new QHBoxLayout;
  corner_->setSpacing(4);
  row->addLayout(corner_);
  outer->addLayout(row);

  auto* line = new QFrame;
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  outer->addWidget(line);

  pages_ = new QStackedWidget;
  // The pages never set the window's width: fit_page() fits them to it.
  pages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  pages_->setMinimumWidth(0);
  for (Context c : kContexts) pages_->addWidget(make_page(c, actions));
  outer->addWidget(pages_);
  // The bar is never narrower than the widest toolbar with icons only, so
  // the window's narrowest width always fits every context's buttons.
  int widest = 0;
  for (Context c : kContexts) {
    QWidget* page = pages_->widget(static_cast<int>(index_of(c)));
    set_page_style(page, Qt::ToolButtonIconOnly);
    widest = std::max(widest, page->layout()->sizeHint().width());
  }
  const QMargins m = outer->contentsMargins();
  setMinimumWidth(widest + m.left() + m.right());
  set_context(Context::Layout);
}

void ContextBar::set_page_style(QWidget* page, Qt::ToolButtonStyle style) {
  const bool compact = style == Qt::ToolButtonIconOnly;
  for (QToolButton* b : page->findChildren<QToolButton*>()) {
    if (b->toolButtonStyle() == style) continue;
    b->setToolButtonStyle(style);
    // More buttons keep the style's own look.
    if (b->defaultAction() != nullptr) b->setStyleSheet(QString::fromLatin1(compact ? kCompactStyle : kToolStyle));
  }
  page->layout()->invalidate();
}

QWidget* ContextBar::make_page(Context context, const ToolActions& actions) {
  auto* page = new QWidget;
  page->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  page->setObjectName(QStringLiteral("page_") + qs(to_string(context)));
  auto* h = new QHBoxLayout(page);
  h->setContentsMargins(0, 2, 0, 0);
  h->setSpacing(6);
  const auto& groups = tool_groups(context);
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const ToolGroup& group = groups[g];
    if (g > 0) {
      auto* sep = new QFrame;
      sep->setFrameShape(QFrame::VLine);
      sep->setFrameShadow(QFrame::Sunken);
      h->addWidget(sep);
    }
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(0);
    const std::size_t shown = std::min(group.shown, group.tools.size());
    for (std::size_t i = 0; i < shown; ++i) {
      auto* b = new QToolButton;
      b->setObjectName(button_name(context, group.tools[i]));
      b->setDefaultAction(actions.action(group.tools[i]));
      b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
      b->setIconSize(QSize(24, 24));
      b->setAutoRaise(true);
      b->setFocusPolicy(Qt::NoFocus);
      b->setStyleSheet(QString::fromLatin1(kToolStyle));
      // Never squeezed (Qt would elide the label): a toolbar too wide for
      // the window needs fewer buttons, not shorter words.
      b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      buttons->addWidget(b);
    }
    if (shown < group.tools.size()) {
      auto* more = new QToolButton;
      more->setObjectName(more_name(context, g));
      more->setText(tr("More"));
      more->setToolTip(tr("More %1 tools").arg(qs(group.name).toLower()));
      // A chevron where the icon goes, so the label lines up with its
      // neighbours' (QToolButton's own arrow isn't drawn under every style).
      more->setIcon(tool_icon(Icon::ChevronDown, palette().color(QPalette::Active, QPalette::ButtonText),
                              palette().color(QPalette::Disabled, QPalette::ButtonText)));
      more->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
      more->setIconSize(QSize(24, 24));
      more->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      more->setPopupMode(QToolButton::InstantPopup);
      more->setAutoRaise(true);
      more->setFocusPolicy(Qt::NoFocus);
      auto* menu = new QMenu(more);
      menu->setToolTipsVisible(true);
      for (std::size_t i = shown; i < group.tools.size(); ++i) menu->addAction(actions.action(group.tools[i]));
      more->setMenu(menu);
      buttons->addWidget(more);
    }
    v->addLayout(buttons);
    auto* caption = new QLabel(qs(group.name));
    caption->setAlignment(Qt::AlignHCenter);
    QFont f = caption->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    caption->setFont(f);
    caption->setStyleSheet(QStringLiteral("QLabel { color: palette(dark); }"));
    v->addWidget(caption);
    h->addWidget(box);
  }
  h->addStretch(1);
  return page;
}

void ContextBar::set_context(Context context) {
  context_ = context;
  for (Context c : kContexts) tabs_.at(index_of(c))->setChecked(c == context);
  pages_->setCurrentIndex(static_cast<int>(index_of(context)));
  fit_page();
}

void ContextBar::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  fit_page();
}

void ContextBar::fit_page() {
  QWidget* page = pages_->currentWidget();
  if (page == nullptr || page->layout() == nullptr) return;
  set_page_style(page, Qt::ToolButtonTextUnderIcon);
  const QMargins m = layout()->contentsMargins();
  const int room = width() - m.left() - m.right();
  if (page->layout()->sizeHint().width() > room) set_page_style(page, Qt::ToolButtonIconOnly);
}

void ContextBar::set_tab(Context tab, const Availability& availability) {
  QToolButton* t = tabs_.at(index_of(tab));
  t->setEnabled(availability.enabled);
  t->setToolTip(availability.enabled ? QString() : qs(availability.reason));
}

void ContextBar::set_detail(const QString& text) { detail_->setText(text); }

QString ContextBar::detail() const { return detail_->text(); }

void ContextBar::add_corner_widget(QWidget* widget) { corner_->addWidget(widget); }

QToolButton* ContextBar::tab(Context context) const { return tabs_.at(index_of(context)); }

QWidget* ContextBar::page(Context context) const { return pages_->widget(static_cast<int>(index_of(context))); }

QToolButton* ContextBar::button(Context context, ToolId id) const {
  return pages_->findChild<QToolButton*>(button_name(context, id));
}

QToolButton* ContextBar::more_button(Context context, std::size_t group) const {
  return pages_->findChild<QToolButton*>(more_name(context, group));
}

}  // namespace vs::app
