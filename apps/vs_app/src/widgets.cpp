#include "widgets.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <numbers>

#include "vs/feature/expression.hpp"

namespace vs::app {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string sstr(const QString& s) { return s.toUtf8().toStdString(); }

QString param_text(const feature::Param& param, ParamUnit unit) {
  if (!param.expression.empty()) return qstr(param.expression);
  switch (unit) {
    case ParamUnit::Angle: return QString::number(param.value * 180.0 / std::numbers::pi, 'g', 10) + " deg";
    case ParamUnit::Count: return QString::number(param.value, 'g', 10);
    case ParamUnit::Length: return QString::number(param.value, 'g', 12);
  }
  return {};
}

std::optional<feature::Param> parse_param(const QString& text, ParamUnit unit) {
  QString t = text.trimmed();
  if (t.isEmpty()) return std::nullopt;
  // A bare number in an angle field means degrees.
  static const QRegularExpression bare(QStringLiteral("^[-+]?[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?$"));
  if (unit == ParamUnit::Angle && bare.match(t).hasMatch()) t += QStringLiteral(" deg");
  auto e = feature::Expression::parse(sstr(t));
  if (!e) return std::nullopt;
  if (e->is_constant()) {
    auto v = e->evaluate([](std::string_view) { return std::optional<double>(); });
    if (!v) return std::nullopt;
    return feature::Param::of(*v);
  }
  return feature::Param::expr(sstr(t));
}

ParamEdit::ParamEdit(ParamUnit unit, QWidget* parent) : QLineEdit(parent), unit_(unit) {
  connect(this, &QLineEdit::textChanged, this, [this] {
    setStyleSheet(param() ? QString() : QStringLiteral("QLineEdit { background: #fde2e2; }"));
    emit paramChanged();
  });
  switch (unit) {
    case ParamUnit::Length: setPlaceholderText(tr("mm, or an expression")); break;
    case ParamUnit::Angle: setPlaceholderText(tr("degrees, or an expression")); break;
    case ParamUnit::Count: setPlaceholderText(tr("a whole number, or an expression")); break;
  }
}

void ParamEdit::set_param(const feature::Param& param) { setText(param_text(param, unit_)); }

std::optional<feature::Param> ParamEdit::param() const { return parse_param(text(), unit_); }

EditDialog::EditDialog(const QString& title, QWidget* parent) : QDialog(parent) {
  setWindowTitle(title);
  setModal(false);
  auto* layout = new QVBoxLayout(this);
  form_ = new QFormLayout;
  form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->addLayout(form_);
  message_ = new QLabel;
  message_->setWordWrap(true);
  message_->setVisible(false);
  layout->addWidget(message_);
  buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  layout->addWidget(buttons_);
  connect(buttons_, &QDialogButtonBox::accepted, this, &EditDialog::commit);
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  setMinimumWidth(460);
}

void EditDialog::set_message(const QString& text, bool error) {
  message_->setText(text);
  message_->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QStringLiteral("color: #555;"));
  message_->setVisible(!text.isEmpty());
}

QString EditDialog::message() const { return message_->isVisible() ? message_->text() : QString(); }

void EditDialog::watch(QWidget* w) {
  if (auto* e = qobject_cast<QLineEdit*>(w)) connect(e, &QLineEdit::textChanged, this, &EditDialog::changed);
  if (auto* c = qobject_cast<QComboBox*>(w)) {
    connect(c, &QComboBox::currentIndexChanged, this, &EditDialog::changed);
  }
  if (auto* b = qobject_cast<QAbstractButton*>(w)) connect(b, &QAbstractButton::toggled, this, &EditDialog::changed);
  if (auto* l = qobject_cast<QListWidget*>(w)) connect(l, &QListWidget::itemChanged, this, &EditDialog::changed);
  if (auto* s = qobject_cast<QSpinBox*>(w)) connect(s, &QSpinBox::valueChanged, this, &EditDialog::changed);
  if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
    connect(d, &QDoubleSpinBox::valueChanged, this, &EditDialog::changed);
  }
  if (auto* t = qobject_cast<QTableWidget*>(w)) connect(t, &QTableWidget::itemChanged, this, &EditDialog::changed);
}

}  // namespace vs::app
