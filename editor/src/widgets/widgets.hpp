#pragma once

#include "core/common.hpp"

#include <QButtonGroup>
#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

class QHBoxLayout;
class QVBoxLayout;

// Small reusable controls with the editor look (see theme/editor.qss for their style properties).
namespace ox::editor {

// Numeric field: type a value, or press and drag horizontally to scrub (Shift = x10, Alt = x0.1).
// Optional coloured axis tag on the left (vector components).
class NumberField : public QWidget {
    Q_OBJECT
public:
    explicit NumberField(QWidget* parent = nullptr);

    void setInteger(bool integer);
    void setRange(double min, double max);
    void setStep(double step) { m_step = step; }
    void setDecimals(int decimals);
    void setSuffix(const QString& suffix);
    void setAxis(const QString& label, const QColor& color);
    void setReadOnly(bool ro);

    // While the user is typing or dragging, external setValue() calls are ignored.
    void setValue(double v);
    void setMixed(bool mixed);
    [[nodiscard]] double value() const { return m_value; }
    [[nodiscard]] bool isEditing() const { return m_dragging || m_edit->hasFocus(); }
    [[nodiscard]] QLineEdit* lineEdit() const { return m_edit; }

Q_SIGNALS:
    void edited(double value, ox::editor::EditPhase phase);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void refreshText();
    double clampValue(double v) const;
    void commitText();

    QLineEdit* m_edit;
    QLabel* m_axis = nullptr;
    QColor m_axisColor;
    double m_value = 0.0;
    double m_min = -1e12, m_max = 1e12;
    double m_step = 0.01;
    int m_decimals = 3;
    bool m_integer = false;
    bool m_mixed = false;
    bool m_readOnly = false;
    QString m_suffix;
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_pressPos;
    double m_dragStartValue = 0.0;
    double m_dragAccum = 0.0;
};

// Coloured X/Y/Z(/W) number fields.
class VectorField : public QWidget {
    Q_OBJECT
public:
    VectorField(int components, QWidget* parent = nullptr, bool integer = false);
    void setValues(const QVector<double>& v);
    void setMixed(const QVector<bool>& mixed);
    [[nodiscard]] QVector<double> values() const;
    [[nodiscard]] bool isEditing() const;
    void setStep(double step);
    void setRange(double min, double max);
    void setDecimals(int d);
    void setReadOnly(bool ro);
    void setLabels(const QStringList& labels);
    [[nodiscard]] NumberField* field(int i) const { return m_fields[i]; }

Q_SIGNALS:
    // Component index edited; values() holds the full new vector.
    void edited(int component, ox::editor::EditPhase phase);

private:
    QVector<NumberField*> m_fields;
};

// Colour swatch; opens a colour dialog. HDR colours get an intensity multiplier.
class ColorButton : public QPushButton {
    Q_OBJECT
public:
    explicit ColorButton(QWidget* parent = nullptr);
    void setColor(const QColor& c);
    [[nodiscard]] QColor color() const { return m_color; }
    void setAlphaEnabled(bool a) { m_alpha = a; }
    void setMixed(bool m);

Q_SIGNALS:
    void colorEdited(const QColor& c, ox::editor::EditPhase phase);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    void pick();
    QColor m_color = Qt::white;
    bool m_alpha = false;
    bool m_mixed = false;
};

// Search box with icon and clear button.
class SearchField : public QLineEdit {
    Q_OBJECT
public:
    explicit SearchField(const QString& placeholder = {}, QWidget* parent = nullptr);
};

// Exclusive segmented buttons ("Low | Medium | High | Ultra").
class SegmentedControl : public QWidget {
    Q_OBJECT
public:
    explicit SegmentedControl(const QStringList& labels, QWidget* parent = nullptr);
    void setCurrent(int index); // -1 = none checked (e.g. "Custom")
    [[nodiscard]] int current() const;
    [[nodiscard]] QPushButton* button(int i) const { return m_buttons[i]; }
    [[nodiscard]] int count() const { return int(m_buttons.size()); }
    void setLabels(const QStringList& labels);

Q_SIGNALS:
    void activated(int index);

private:
    QVector<QPushButton*> m_buttons;
    QButtonGroup* m_group;
};

// iOS-style switch.
class ToggleSwitch : public QWidget {
    Q_OBJECT
    Q_PROPERTY(bool checked READ isChecked WRITE setChecked NOTIFY toggled)
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);
    [[nodiscard]] bool isChecked() const { return m_checked; }
    void setChecked(bool c);
    [[nodiscard]] QSize sizeHint() const override { return {34, 20}; }

Q_SIGNALS:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; update(); }

private:
    bool m_checked = false;
    bool m_hover = false;
};

// Card with a clickable header (chevron, icon, title, trailing widgets) and a collapsible body.
class CollapsibleSection : public QFrame {
    Q_OBJECT
public:
    CollapsibleSection(const QString& title, const QString& icon, QWidget* parent = nullptr);
    void setBody(QWidget* body);
    [[nodiscard]] QWidget* body() const { return m_body; }
    void addHeaderWidget(QWidget* w);
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const { return m_expanded; }
    void setTitle(const QString& t);
    void setSubtitle(const QString& t);
    [[nodiscard]] QFrame* header() const { return m_header; }

Q_SIGNALS:
    void toggled(bool expanded);
    void headerContextMenu(const QPoint& globalPos);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QFrame* m_header;
    QToolButton* m_chevron;
    QLabel* m_icon;
    QLabel* m_title;
    QLabel* m_subtitle;
    QHBoxLayout* m_headerLayout;
    QVBoxLayout* m_layout;
    QWidget* m_body = nullptr;
    bool m_expanded = true;
};

// Title bar for dock panels: icon, title, trailing actions and a float/close pair.
class DockTitleBar : public QWidget {
    Q_OBJECT
public:
    DockTitleBar(const QString& title, const QString& icon, QWidget* dock);
    void setTitle(const QString& title);
    void addWidget(QWidget* w);
    // Tabbed docks show their tab instead of a title bar.
    void setCompact(bool compact);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QLabel* m_icon;
    QLabel* m_title;
    QHBoxLayout* m_layout;
    QWidget* m_dock;
    QString m_iconName;
};

// Rounded "pill" label (badges in headers).
QLabel* makeBadge(const QString& text, QWidget* parent = nullptr);
// Small flat tool button with an icon and tooltip.
QToolButton* makeToolButton(const QString& icon, const QString& tooltip, QWidget* parent = nullptr, bool checkable = false);
// Horizontal hairline separator.
QFrame* makeHairline(QWidget* parent = nullptr);
// Section caption ("SHADOWS").
QLabel* makeSectionLabel(const QString& text, QWidget* parent = nullptr);
// Re-polishes a widget after a dynamic style property change.
void repolish(QWidget* w);

} // namespace ox::editor
