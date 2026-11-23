#include "browser.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QToolButton>
#include <QVBoxLayout>

#include "icon_painter.hpp"

namespace vs::app {

namespace {

constexpr int kIconSize = 20;

QIcon icon_for(const QWidget* w, Icon icon) {
  return tool_icon(icon, w->palette().color(QPalette::Active, QPalette::ButtonText),
                   w->palette().color(QPalette::Disabled, QPalette::ButtonText), kIconSize);
}

QToolButton* small_button(const QWidget* owner, Icon icon, const QString& tooltip) {
  auto* b = new QToolButton;
  b->setIcon(icon_for(owner, icon));
  b->setIconSize(QSize(kIconSize, kIconSize));
  b->setToolTip(tooltip);
  b->setAutoRaise(true);
  b->setFocusPolicy(Qt::NoFocus);
  return b;
}

}  // namespace

// --- FoldPanel ---------------------------------------------------------------------------------

int FoldPanel::strip_width() { return kIconSize + 16; }

FoldPanel::FoldPanel(const QString& title, Qt::Edge side, QWidget* parent) : QWidget(parent) {
  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  const bool left = side == Qt::LeftEdge;

  expanded_ = new QWidget;
  expanded_layout_ = new QVBoxLayout(expanded_);
  expanded_layout_->setContentsMargins(0, 0, 0, 0);
  expanded_layout_->setSpacing(0);
  auto* header = new QHBoxLayout;
  header->setContentsMargins(6, 2, 2, 2);
  auto* name = new QLabel(title);
  QFont f = name->font();
  f.setBold(true);
  name->setFont(f);
  fold_ = small_button(this, left ? Icon::ChevronLeft : Icon::ChevronRight, tr("Fold %1").arg(title));
  fold_->setObjectName(QStringLiteral("fold"));
  header->addWidget(name, 1);
  header->addWidget(fold_);
  expanded_layout_->addLayout(header);

  strip_ = new QWidget;
  strip_->setFixedWidth(strip_width());
  strip_layout_ = new QVBoxLayout(strip_);
  strip_layout_->setContentsMargins(2, 2, 2, 2);
  strip_layout_->setSpacing(2);
  unfold_ = small_button(this, left ? Icon::ChevronRight : Icon::ChevronLeft, tr("Show %1").arg(title));
  unfold_->setObjectName(QStringLiteral("unfold"));
  strip_layout_->addWidget(unfold_);
  strip_layout_->addStretch(1);
  strip_->hide();

  outer->addWidget(expanded_, 1);
  outer->addWidget(strip_);
  connect(fold_, &QToolButton::clicked, this, [this] { set_folded(true); });
  connect(unfold_, &QToolButton::clicked, this, [this] { set_folded(false); });
}

void FoldPanel::set_body(QWidget* body) {
  if (body_ != nullptr) expanded_layout_->removeWidget(body_);
  body_ = body;
  expanded_layout_->addWidget(body_, 1);
}

int FoldPanel::add_strip_button(Icon icon, const QString& tooltip) {
  const int index = strip_buttons_++;
  QToolButton* b = small_button(this, icon, tooltip);
  b->setObjectName(QStringLiteral("strip_%1").arg(index));
  // After the unfold button, before the stretch.
  strip_layout_->insertWidget(index + 1, b);
  connect(b, &QToolButton::clicked, this, [this, index] {
    set_folded(false);
    emit stripClicked(index);
  });
  return index;
}

void FoldPanel::set_folded(bool folded) {
  if (folded == folded_) return;
  folded_ = folded;
  expanded_->setVisible(!folded);
  strip_->setVisible(folded);
  // Folded, the panel is only as wide as the strip.
  setMaximumWidth(folded ? strip_width() : QWIDGETSIZE_MAX);
  emit foldedChanged(folded);
}

// --- Browser -----------------------------------------------------------------------------------

Browser::Browser(QWidget* parent) : FoldPanel(tr("Browser"), Qt::LeftEdge, parent) {
  setObjectName(QStringLiteral("browser"));
  column_ = new QWidget;
  column_layout_ = new QVBoxLayout(column_);
  column_layout_->setContentsMargins(0, 0, 0, 0);
  column_layout_->setSpacing(0);
  set_body(column_);
  connect(this, &FoldPanel::stripClicked, this, [this](int section) { reveal(section); });
}

int Browser::add_section(const QString& title, Icon icon, QWidget* content, bool open) {
  const int index = static_cast<int>(sections_.size());
  auto* header = new QToolButton;
  header->setObjectName(QStringLiteral("section_%1").arg(index));
  header->setText(title);
  header->setCheckable(true);
  header->setChecked(open);
  header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  header->setAutoRaise(true);
  header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  header->setFocusPolicy(Qt::NoFocus);
  header->setStyleSheet(QStringLiteral("QToolButton { text-align: left; font-weight: bold; padding: 3px 4px; "
                                       "border-top: 1px solid palette(mid); }"));
  if (index == 0) header->setStyleSheet(header->styleSheet().replace(QStringLiteral("palette(mid)"),
                                                                     QStringLiteral("transparent")));
  column_layout_->addWidget(header);
  column_layout_->addWidget(content, open ? 1 : 0);
  content->setVisible(open);
  sections_.push_back({header, content});
  connect(header, &QToolButton::toggled, this, [this, index](bool on) { set_section_open(index, on); });
  add_strip_button(icon, title);
  relayout();
  return index;
}

void Browser::set_section_open(int section, bool open) {
  if (section < 0 || section >= section_count()) return;
  Section& s = sections_[static_cast<std::size_t>(section)];
  if (s.header->isChecked() != open) s.header->setChecked(open);  // comes back here through toggled
  s.content->setVisible(open);
  relayout();
}

bool Browser::section_open(int section) const {
  return section >= 0 && section < section_count() && sections_[static_cast<std::size_t>(section)].header->isChecked();
}

QToolButton& Browser::section_header(int section) const { return *sections_.at(static_cast<std::size_t>(section)).header; }

void Browser::reveal(int section) {
  set_folded(false);
  set_section_open(section, true);
}

void Browser::relayout() {
  // Open sections share the height; with every section folded the headers
  // sit at the top.
  bool any_open = false;
  for (const Section& s : sections_) {
    const bool open = s.header->isChecked();
    s.header->setIcon(icon_for(this, open ? Icon::ChevronDown : Icon::ChevronRight));
    column_layout_->setStretchFactor(s.content, open ? 1 : 0);
    any_open = any_open || open;
  }
  // A stretch at the end only while everything is folded.
  const int last = column_layout_->count() - 1;
  QLayoutItem* tail = last >= 0 ? column_layout_->itemAt(last) : nullptr;
  const bool has_tail = tail != nullptr && tail->spacerItem() != nullptr;
  if (!any_open && !has_tail) column_layout_->addStretch(1);
  if (any_open && has_tail) delete column_layout_->takeAt(last);
}

}  // namespace vs::app
