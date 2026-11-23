#include "problems.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

#include "icon_painter.hpp"
#include "icons.hpp"

namespace vs::app {

namespace {

QIcon icon_for(const QWidget* w, Icon icon) {
  return tool_icon(icon, w->palette().color(QPalette::Active, QPalette::ButtonText),
                   w->palette().color(QPalette::Disabled, QPalette::ButtonText), 16);
}

QString counts_text(std::size_t errors, std::size_t warnings) {
  return QObject::tr("%n error(s)", nullptr, static_cast<int>(errors)) + QStringLiteral(", ") +
         QObject::tr("%n warning(s)", nullptr, static_cast<int>(warnings));
}

}  // namespace

// --- ProblemsButton ----------------------------------------------------------------------------

ProblemsButton::ProblemsButton(QWidget* parent) : QToolButton(parent) {
  setObjectName(QStringLiteral("problems_button"));
  setCheckable(true);
  setAutoRaise(true);
  setFocusPolicy(Qt::NoFocus);
  setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  setIconSize(QSize(16, 16));
  set_counts(0, 0);
}

void ProblemsButton::set_counts(std::size_t errors, std::size_t warnings) {
  errors_ = errors;
  warnings_ = warnings;
  if (errors > 0) {
    setIcon(icon_for(this, Icon::Error));
  } else if (warnings > 0) {
    setIcon(icon_for(this, Icon::Warning));
  } else {
    setIcon(icon_for(this, Icon::Done));
  }
  setText(errors == 0 && warnings == 0 ? tr("No problems") : counts_text(errors, warnings));
  setToolTip(tr("Problems: %1. Click to show or hide them.").arg(counts_text(errors, warnings)));
}

// --- ProblemsDrawer ----------------------------------------------------------------------------

ProblemsDrawer::ProblemsDrawer(QWidget* list, QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("problems_drawer"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  auto* header = new QHBoxLayout;
  header->setContentsMargins(8, 2, 2, 2);
  auto* title = new QLabel(tr("Problems"));
  QFont f = title->font();
  f.setBold(true);
  title->setFont(f);
  summary_ = new QLabel;
  summary_->setObjectName(QStringLiteral("problems_summary"));
  summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  summary_->setStyleSheet(QStringLiteral("QLabel { color: palette(dark); padding-left: 8px; }"));
  close_ = new QToolButton;
  close_->setObjectName(QStringLiteral("problems_close"));
  close_->setIcon(icon_for(this, Icon::ChevronDown));
  close_->setIconSize(QSize(16, 16));
  close_->setAutoRaise(true);
  close_->setFocusPolicy(Qt::NoFocus);
  close_->setToolTip(tr("Hide the problems"));
  header->addWidget(title);
  header->addWidget(summary_, 1);
  header->addWidget(close_);
  v->addLayout(header);
  v->addWidget(list, 1);
  connect(close_, &QToolButton::clicked, this, &ProblemsDrawer::closeRequested);
}

void ProblemsDrawer::set_summary(const QString& source, std::size_t errors, std::size_t warnings) {
  summary_->setText(tr("%1: %2").arg(source, counts_text(errors, warnings)));
}

QString ProblemsDrawer::summary() const { return summary_->text(); }

}  // namespace vs::app
