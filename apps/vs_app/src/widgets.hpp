#pragma once

// Small widgets the authoring dialogs share (M14):
//
//   ParamEdit    a line edit for a Param: a number ("12.5", "10 mm",
//                "30 deg") or an expression over the variables
//                ("width / 2"). Constant text becomes a value; anything
//                naming a variable stays an expression, evaluated by the
//                rebuild. Angle fields read a bare number as degrees.
//   RefPicker    a combo box over labelled reference choices
//                (reference_choices.hpp) that keeps a reference it doesn't
//                list (a broken one) as "(missing) ...", so editing a
//                feature never silently changes what it refers to.
//   CheckList    a list of labelled choices with check boxes.
//   EditDialog   the base of every authoring dialog: OK / Cancel, a message
//                line, and two signals: changed() for live previews and
//                commitRequested() for OK, so the main window can refuse a
//                change and keep the dialog open with the reason.

#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>

#include <optional>
#include <string>
#include <vector>

#include "reference_choices.hpp"
#include "vs/feature/tree.hpp"

class QFormLayout;
class QLabel;
class QDialogButtonBox;

namespace vs::app {

QString qstr(std::string_view s);
std::string sstr(const QString& s);

enum class ParamUnit { Length, Angle, Count };

// The text a Param is shown as, and back (nullopt when it doesn't parse).
QString param_text(const feature::Param& param, ParamUnit unit);
std::optional<feature::Param> parse_param(const QString& text, ParamUnit unit);

class ParamEdit : public QLineEdit {
  Q_OBJECT

 public:
  explicit ParamEdit(ParamUnit unit = ParamUnit::Length, QWidget* parent = nullptr);
  void set_param(const feature::Param& param);
  [[nodiscard]] std::optional<feature::Param> param() const;
  [[nodiscard]] ParamUnit unit() const { return unit_; }

 signals:
  void paramChanged();

 private:
  ParamUnit unit_;
};

template <typename Ref>
class RefPicker {
 public:
  RefPicker() = default;
  RefPicker(QComboBox* box, std::vector<Choice<Ref>> choices, bool allow_none = false,
            const QString& none_label = QStringLiteral("(none)"))
      : box_(box), choices_(std::move(choices)), allow_none_(allow_none) {
    box_->clear();
    if (allow_none_) box_->addItem(none_label);
    for (const auto& c : choices_) box_->addItem(qstr(c.label));
  }

  [[nodiscard]] QComboBox* box() const { return box_; }
  [[nodiscard]] bool empty() const { return choices_.empty(); }

  // A reference it doesn't list is kept as "(missing reference)"; with
  // `keep_missing` false the first choice is taken instead (new features).
  void set(const Ref& ref, bool keep_missing = true) {
    for (std::size_t i = 0; i < choices_.size(); ++i) {
      if (choices_[i].ref == ref) {
        box_->setCurrentIndex(static_cast<int>(i) + (allow_none_ ? 1 : 0));
        return;
      }
    }
    if (!keep_missing) {
      if (box_->count() > 0) box_->setCurrentIndex(allow_none_ ? 1 : 0);
      return;
    }
    // Not offered (broken, or out of scope): keep it, labelled.
    choices_.push_back({"(missing reference)", ref});
    box_->addItem(QStringLiteral("(missing reference)"));
    box_->setCurrentIndex(box_->count() - 1);
  }
  void set_none() {
    if (allow_none_) box_->setCurrentIndex(0);
  }
  [[nodiscard]] std::optional<Ref> get() const {
    const int i = box_->currentIndex() - (allow_none_ ? 1 : 0);
    if (i < 0 || static_cast<std::size_t>(i) >= choices_.size()) return std::nullopt;
    return choices_[static_cast<std::size_t>(i)].ref;
  }

 private:
  QComboBox* box_ = nullptr;
  std::vector<Choice<Ref>> choices_;
  bool allow_none_ = false;
};

template <typename Ref>
class CheckList {
 public:
  CheckList() = default;
  CheckList(QListWidget* list, std::vector<Choice<Ref>> choices) : list_(list), choices_(std::move(choices)) {
    list_->clear();
    for (const auto& c : choices_) {
      auto* item = new QListWidgetItem(qstr(c.label), list_);
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      item->setCheckState(Qt::Unchecked);
    }
  }
  [[nodiscard]] QListWidget* list() const { return list_; }

  void set(const std::vector<Ref>& refs) {
    for (int i = 0; i < list_->count(); ++i) list_->item(i)->setCheckState(Qt::Unchecked);
    for (const auto& r : refs) {
      bool found = false;
      for (std::size_t i = 0; i < choices_.size(); ++i) {
        if (choices_[i].ref == r) {
          list_->item(static_cast<int>(i))->setCheckState(Qt::Checked);
          found = true;
        }
      }
      if (!found) {
        choices_.push_back({"(missing reference)", r});
        auto* item = new QListWidgetItem(QStringLiteral("(missing reference)"), list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
      }
    }
  }
  // The checked choices, in list order.
  [[nodiscard]] std::vector<Ref> get() const {
    std::vector<Ref> out;
    for (std::size_t i = 0; i < choices_.size(); ++i) {
      if (list_->item(static_cast<int>(i))->checkState() == Qt::Checked) out.push_back(choices_[i].ref);
    }
    return out;
  }

 private:
  QListWidget* list_ = nullptr;
  std::vector<Choice<Ref>> choices_;
};

class EditDialog : public QDialog {
  Q_OBJECT

 public:
  explicit EditDialog(const QString& title, QWidget* parent = nullptr);

  // Shows why the change can't be made (or clears it with an empty text).
  void set_message(const QString& text, bool error = true);
  [[nodiscard]] QString message() const;
  // OK, as the button does: asks the owner to commit.
  void commit() { emit commitRequested(); }
  // Whether the fields describe something complete (every required
  // reference chosen, every number parsed).
  [[nodiscard]] virtual bool complete() const { return true; }

 signals:
  void changed();
  void commitRequested();

 protected:
  QFormLayout* form() const { return form_; }
  // Connects a widget's edits to changed().
  void watch(QWidget* widget);

 private:
  QFormLayout* form_ = nullptr;
  QLabel* message_ = nullptr;
  QDialogButtonBox* buttons_ = nullptr;
};

}  // namespace vs::app
