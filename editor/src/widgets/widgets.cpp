#include "widgets/widgets.hpp"

#include "theme/icons.hpp"
#include "theme/theme.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

namespace ox::editor {

void repolish(QWidget* w) {
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

QLabel* makeBadge(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("role", "badge");
    return l;
}

QToolButton* makeToolButton(const QString& icon, const QString& tooltip, QWidget* parent, bool checkable) {
    auto* b = new QToolButton(parent);
    b->setIcon(Icons::get(icon));
    b->setToolTip(tooltip);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QFrame* makeHairline(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setProperty("role", "hairline");
    f->setFixedHeight(1);
    return f;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setProperty("role", "section");
    return l;
}

// ---- NumberField --------------------------------------------------------------------------------------------

NumberField::NumberField(QWidget* parent) : QWidget(parent) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_edit = new QLineEdit(this);
    m_edit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_edit->installEventFilter(this);
    m_edit->setCursor(Qt::SizeHorCursor);
    m_edit->setMinimumWidth(28);
    m_edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    lay->addWidget(m_edit);
    connect(m_edit, &QLineEdit::editingFinished, this, [this] {
        if (!m_edit->isModified()) return;
        m_edit->setModified(false);
        commitText();
    });
    refreshText();
}

void NumberField::setInteger(bool integer) {
    m_integer = integer;
    if (integer) {
        m_decimals = 0;
        m_step = std::max(1.0, m_step);
    }
    refreshText();
}

void NumberField::setRange(double min, double max) {
    m_min = min;
    m_max = max;
}

void NumberField::setDecimals(int decimals) {
    m_decimals = decimals;
    refreshText();
}

void NumberField::setSuffix(const QString& suffix) {
    m_suffix = suffix;
    refreshText();
}

void NumberField::setAxis(const QString& label, const QColor& color) {
    m_axisColor = color;
    if (!m_axis) {
        m_axis = new QLabel(this);
        m_axis->setAlignment(Qt::AlignCenter);
        m_axis->setFixedWidth(16);
        m_axis->setCursor(Qt::SizeHorCursor);
        m_axis->installEventFilter(this);
        static_cast<QHBoxLayout*>(layout())->insertWidget(0, m_axis);
    }
    m_axis->setText(label);
    m_axis->setStyleSheet(QStringLiteral("QLabel{color:%1;font-weight:700;background:%2;border:1px solid %3;border-right:none;"
                                         "border-top-left-radius:6px;border-bottom-left-radius:6px;font-size:%4px;}")
                              .arg(cssColor(color), cssColor(withAlpha(color, 38)), cssColor(colors().border))
                              .arg(Theme::instance().fontSize() - 2));
    m_edit->setStyleSheet(QStringLiteral("QLineEdit{border-top-left-radius:0;border-bottom-left-radius:0;}"));
}

void NumberField::setReadOnly(bool ro) {
    m_readOnly = ro;
    m_edit->setReadOnly(ro);
    m_edit->setCursor(ro ? Qt::ArrowCursor : Qt::SizeHorCursor);
}

double NumberField::clampValue(double v) const {
    v = std::clamp(v, m_min, m_max);
    if (m_integer) v = std::round(v);
    return v;
}

void NumberField::setValue(double v) {
    if (isEditing()) return;
    m_value = v;
    m_mixed = false;
    refreshText();
}

void NumberField::setMixed(bool mixed) {
    if (isEditing()) return;
    m_mixed = mixed;
    refreshText();
}

void NumberField::refreshText() {
    m_edit->setProperty("mixed", m_mixed);
    if (m_mixed) {
        m_edit->setText(QStringLiteral("—"));
    } else if (m_integer) {
        m_edit->setText(QString::number(qint64(std::llround(m_value))) + m_suffix);
    } else {
        QString t = QString::number(m_value, 'f', m_decimals);
        if (t.contains(QLatin1Char('.'))) {
            while (t.endsWith(QLatin1Char('0'))) t.chop(1);
            if (t.endsWith(QLatin1Char('.'))) t += QLatin1Char('0');
        }
        if (t == QLatin1String("-0.0")) t = QStringLiteral("0.0");
        m_edit->setText(t + m_suffix);
    }
    m_edit->setCursorPosition(0);
    repolish(m_edit);
}

void NumberField::commitText() {
    QString t = m_edit->text().trimmed();
    if (!m_suffix.isEmpty() && t.endsWith(m_suffix)) t.chop(m_suffix.size());
    bool ok = false;
    double v = t.toDouble(&ok);
    if (!ok) {
        // tiny expression support: "2*3", "10/4", "1+2"
        for (QChar op : {QLatin1Char('*'), QLatin1Char('/'), QLatin1Char('+')}) {
            const int i = t.indexOf(op, 1);
            if (i > 0) {
                bool a = false, b = false;
                const double l = t.left(i).toDouble(&a), r = t.mid(i + 1).toDouble(&b);
                if (a && b) {
                    ok = true;
                    v = op == QLatin1Char('*') ? l * r : op == QLatin1Char('/') ? (r != 0 ? l / r : l) : l + r;
                }
                break;
            }
        }
    }
    if (!ok) {
        refreshText();
        return;
    }
    m_value = clampValue(v);
    m_mixed = false;
    refreshText();
    Q_EMIT edited(m_value, EditPhase::Single);
}

bool NumberField::eventFilter(QObject* obj, QEvent* ev) {
    if (m_readOnly) return QWidget::eventFilter(obj, ev);
    const bool onAxis = obj == m_axis;
    const bool focused = m_edit->hasFocus();
    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton || (focused && !onAxis)) break;
        m_pressed = true;
        m_dragging = false;
        m_pressPos = me->globalPosition().toPoint();
        m_dragStartValue = m_value;
        m_dragAccum = 0.0;
        return true;
    }
    case QEvent::MouseMove: {
        if (!m_pressed) break;
        auto* me = static_cast<QMouseEvent*>(ev);
        const QPoint p = me->globalPosition().toPoint();
        const int dx = p.x() - m_pressPos.x();
        if (!m_dragging && std::abs(dx) < 3) return true;
        double mul = 1.0;
        if (me->modifiers() & Qt::ShiftModifier) mul = 10.0;
        if (me->modifiers() & Qt::AltModifier) mul = 0.1;
        const EditPhase phase = m_dragging ? EditPhase::Update : EditPhase::Begin;
        m_dragging = true;
        m_dragAccum += dx * m_step * mul;
        m_pressPos = p;
        const double before = m_value;
        m_value = clampValue(m_dragStartValue + m_dragAccum);
        m_mixed = false;
        refreshText();
        if (m_value != before || phase == EditPhase::Begin) Q_EMIT edited(m_value, phase);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (!m_pressed) break;
        m_pressed = false;
        if (m_dragging) {
            m_dragging = false;
            Q_EMIT edited(m_value, EditPhase::End);
        } else if (!onAxis) {
            m_edit->setFocus(Qt::MouseFocusReason);
            m_edit->selectAll();
            m_edit->setCursor(Qt::IBeamCursor);
        }
        return true;
    }
    case QEvent::FocusOut:
        if (obj == m_edit) {
            m_edit->setCursor(Qt::SizeHorCursor);
            if (m_edit->isModified()) {
                m_edit->setModified(false);
                commitText();
            }
        }
        break;
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (obj == m_edit && ke->key() == Qt::Key_Escape) {
            m_edit->setModified(false);
            m_edit->clearFocus();
            refreshText();
            return true;
        }
        if (obj == m_edit && (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)) {
            m_value = clampValue(m_value + (ke->key() == Qt::Key_Up ? 1 : -1) * (m_integer ? 1.0 : m_step * 10.0));
            m_mixed = false;
            const bool f = m_edit->hasFocus();
            m_edit->clearFocus();
            refreshText();
            if (f) m_edit->setFocus();
            Q_EMIT edited(m_value, EditPhase::Single);
            return true;
        }
        break;
    }
    default: break;
    }
    return QWidget::eventFilter(obj, ev);
}

void NumberField::paintEvent(QPaintEvent* e) { QWidget::paintEvent(e); }

// ---- VectorField --------------------------------------------------------------------------------------------

VectorField::VectorField(int components, QWidget* parent, bool integer) : QWidget(parent) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    static const char* names[] = {"X", "Y", "Z", "W"};
    for (int i = 0; i < components; ++i) {
        auto* f = new NumberField(this);
        f->setInteger(integer);
        f->setAxis(QString::fromLatin1(names[i]), i < 3 ? Theme::axisColor(i) : colors().textDim);
        lay->addWidget(f, 1);
        m_fields.push_back(f);
        connect(f, &NumberField::edited, this, [this, i](double, EditPhase phase) { Q_EMIT edited(i, phase); });
    }
}

void VectorField::setLabels(const QStringList& labels) {
    for (int i = 0; i < m_fields.size() && i < labels.size(); ++i) {
        m_fields[i]->setAxis(labels[i], i < 3 ? Theme::axisColor(i) : colors().textDim);
    }
}

void VectorField::setValues(const QVector<double>& v) {
    for (int i = 0; i < m_fields.size() && i < v.size(); ++i) m_fields[i]->setValue(v[i]);
}

void VectorField::setMixed(const QVector<bool>& mixed) {
    for (int i = 0; i < m_fields.size() && i < mixed.size(); ++i) {
        if (mixed[i]) m_fields[i]->setMixed(true);
    }
}

QVector<double> VectorField::values() const {
    QVector<double> v;
    for (auto* f : m_fields) v.push_back(f->value());
    return v;
}

bool VectorField::isEditing() const {
    for (auto* f : m_fields) {
        if (f->isEditing()) return true;
    }
    return false;
}

void VectorField::setStep(double step) {
    for (auto* f : m_fields) f->setStep(step);
}
void VectorField::setRange(double min, double max) {
    for (auto* f : m_fields) f->setRange(min, max);
}
void VectorField::setDecimals(int d) {
    for (auto* f : m_fields) f->setDecimals(d);
}
void VectorField::setReadOnly(bool ro) {
    for (auto* f : m_fields) f->setReadOnly(ro);
}

// ---- ColorButton --------------------------------------------------------------------------------------------

ColorButton::ColorButton(QWidget* parent) : QPushButton(parent) {
    setMinimumHeight(24);
    setCursor(Qt::PointingHandCursor);
    connect(this, &QPushButton::clicked, this, &ColorButton::pick);
}

void ColorButton::setColor(const QColor& c) {
    m_color = c;
    m_mixed = false;
    update();
}

void ColorButton::setMixed(bool m) {
    m_mixed = m;
    update();
}

void ColorButton::pick() {
    const QColor before = m_color;
    QColorDialog dlg(m_color, window());
    dlg.setOption(QColorDialog::ShowAlphaChannel, m_alpha);
    dlg.setOption(QColorDialog::DontUseNativeDialog, true);
    dlg.setWindowTitle(tr("Pick Colour"));
    bool begun = false;
    connect(&dlg, &QColorDialog::currentColorChanged, this, [&](const QColor& c) {
        m_color = c;
        update();
        Q_EMIT colorEdited(c, begun ? EditPhase::Update : EditPhase::Begin);
        begun = true;
    });
    if (dlg.exec() == QDialog::Accepted) {
        m_color = dlg.selectedColor();
        Q_EMIT colorEdited(m_color, begun ? EditPhase::End : EditPhase::Single);
    } else {
        m_color = before;
        if (begun) Q_EMIT colorEdited(before, EditPhase::End);
    }
    update();
}

void ColorButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& c = colors();
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 6, 6);
    p.setClipPath(path);
    // checkerboard for alpha
    const int s = 6;
    for (int y = 0; y < height(); y += s) {
        for (int x = 0; x < width(); x += s) {
            p.fillRect(x, y, s, s, ((x / s + y / s) % 2) ? QColor(200, 200, 200) : QColor(150, 150, 150));
        }
    }
    if (m_mixed) {
        p.fillRect(rect(), c.bg3);
        p.setPen(c.textFaint);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("—"));
    } else {
        QColor col = m_color;
        // HDR colours (components > 1) are shown normalised with an intensity badge.
        const double mx = std::max({m_color.redF(), m_color.greenF(), m_color.blueF(), 1.0f});
        if (mx > 1.0) col = QColor::fromRgbF(float(m_color.redF() / mx), float(m_color.greenF() / mx), float(m_color.blueF() / mx));
        QColor opaque = col;
        opaque.setAlpha(255);
        p.fillRect(QRect(0, 0, width() / 2, height()), opaque);
        p.fillRect(QRect(width() / 2, 0, width() - width() / 2, height()), col);
        p.setPen(col.lightnessF() > 0.6 ? QColor(20, 20, 24) : QColor(240, 240, 245));
        QFont f = font();
        f.setPixelSize(std::max(9, Theme::instance().fontSize() - 2));
        p.setFont(f);
        p.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, opaque.name().toUpper());
    }
    p.setClipping(false);
    p.setPen(QPen(underMouse() ? c.borderHover : c.border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// ---- SearchField --------------------------------------------------------------------------------------------

SearchField::SearchField(const QString& placeholder, QWidget* parent) : QLineEdit(parent) {
    setPlaceholderText(placeholder.isEmpty() ? tr("Search…") : placeholder);
    setProperty("role", "search");
    setClearButtonEnabled(true);
    addAction(Icons::get(QStringLiteral("search")), QLineEdit::LeadingPosition);
}

// ---- SegmentedControl ---------------------------------------------------------------------------------------

SegmentedControl::SegmentedControl(const QStringList& labels, QWidget* parent) : QWidget(parent), m_group(new QButtonGroup(this)) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_group->setExclusive(true);
    for (int i = 0; i < labels.size(); ++i) {
        auto* b = new QPushButton(labels[i], this);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("segment", labels.size() == 1 ? "single" : i == 0 ? "left" : i == labels.size() - 1 ? "right" : "middle");
        b->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(b, 1);
        m_group->addButton(b, i);
        m_buttons.push_back(b);
    }
    connect(m_group, &QButtonGroup::idClicked, this, &SegmentedControl::activated);
}

void SegmentedControl::setLabels(const QStringList& labels) {
    for (int i = 0; i < m_buttons.size() && i < labels.size(); ++i) m_buttons[i]->setText(labels[i]);
}

void SegmentedControl::setCurrent(int index) {
    if (index < 0 || index >= m_buttons.size()) {
        m_group->setExclusive(false);
        for (auto* b : m_buttons) b->setChecked(false);
        m_group->setExclusive(true);
        return;
    }
    m_buttons[index]->setChecked(true);
}

int SegmentedControl::current() const { return m_group->checkedId(); }

// ---- ToggleSwitch -------------------------------------------------------------------------------------------

ToggleSwitch::ToggleSwitch(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
    setFocusPolicy(Qt::StrongFocus);
}

void ToggleSwitch::setChecked(bool c) {
    if (c == m_checked) return;
    m_checked = c;
    update();
    Q_EMIT toggled(c);
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* e) {
    if (isEnabled() && rect().contains(e->position().toPoint())) setChecked(!m_checked);
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& c = colors();
    QRectF track = QRectF(rect()).adjusted(1, 2, -1, -2);
    QColor bg = m_checked ? c.accent : (m_hover ? c.bg4 : c.bg3);
    if (!isEnabled()) bg = m_checked ? withAlpha(c.accent, 90) : c.bg2;
    p.setPen(QPen(m_checked ? bg : c.borderStrong, 1));
    p.setBrush(bg);
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    const qreal d = track.height() - 4;
    const qreal x = m_checked ? track.right() - d - 2 : track.left() + 2;
    p.setPen(Qt::NoPen);
    p.setBrush(isEnabled() ? QColor(255, 255, 255) : c.textFaint);
    p.drawEllipse(QRectF(x, track.top() + 2, d, d));
}

// ---- CollapsibleSection -------------------------------------------------------------------------------------

CollapsibleSection::CollapsibleSection(const QString& title, const QString& icon, QWidget* parent) : QFrame(parent) {
    setProperty("role", "card");
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_header = new QFrame(this);
    m_header->setProperty("role", "cardHeader");
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->installEventFilter(this);
    m_headerLayout = new QHBoxLayout(m_header);
    m_headerLayout->setContentsMargins(6, 4, 6, 4);
    m_headerLayout->setSpacing(6);
    m_chevron = new QToolButton(m_header);
    m_chevron->setIcon(Icons::get(QStringLiteral("chevron-down")));
    m_chevron->setIconSize(QSize(12, 12));
    m_chevron->setAutoRaise(true);
    m_chevron->setFixedSize(18, 18);
    connect(m_chevron, &QToolButton::clicked, this, [this] { setExpanded(!m_expanded); });
    m_icon = new QLabel(m_header);
    m_icon->setPixmap(Icons::get(icon, Icons::Tint::Accent).pixmap(QSize(16, 16)));
    m_icon->setFixedSize(18, 18);
    m_title = new QLabel(title, m_header);
    QFont f = m_title->font();
    f.setWeight(QFont::DemiBold);
    m_title->setFont(f);
    m_subtitle = new QLabel(m_header);
    m_subtitle->setProperty("role", "faint");
    m_subtitle->hide();
    m_headerLayout->addWidget(m_chevron);
    m_headerLayout->addWidget(m_icon);
    m_headerLayout->addWidget(m_title);
    m_headerLayout->addWidget(m_subtitle);
    m_headerLayout->addStretch(1);
    m_layout->addWidget(m_header);
}

void CollapsibleSection::setTitle(const QString& t) { m_title->setText(t); }
void CollapsibleSection::setSubtitle(const QString& t) {
    m_subtitle->setText(t);
    m_subtitle->setVisible(!t.isEmpty());
}

void CollapsibleSection::setBody(QWidget* body) {
    if (m_body) m_body->deleteLater();
    m_body = body;
    m_layout->addWidget(body);
    body->setVisible(m_expanded);
}

void CollapsibleSection::addHeaderWidget(QWidget* w) { m_headerLayout->addWidget(w); }

void CollapsibleSection::setExpanded(bool expanded) {
    if (m_expanded == expanded) return;
    m_expanded = expanded;
    m_chevron->setIcon(Icons::get(expanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right")));
    if (m_body) m_body->setVisible(expanded);
    m_header->setStyleSheet(expanded ? QString() : QStringLiteral("QFrame{border-radius:8px;}"));
    Q_EMIT toggled(expanded);
}

bool CollapsibleSection::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == m_header) {
        if (ev->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(ev)->button() == Qt::LeftButton) {
            setExpanded(!m_expanded);
            return true;
        }
        if (ev->type() == QEvent::ContextMenu) {
            Q_EMIT headerContextMenu(static_cast<QContextMenuEvent*>(ev)->globalPos());
            return true;
        }
    }
    return QFrame::eventFilter(obj, ev);
}

// ---- DockTitleBar -------------------------------------------------------------------------------------------

DockTitleBar::DockTitleBar(const QString& title, const QString& icon, QWidget* dock) : QWidget(dock), m_dock(dock), m_iconName(icon) {
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(10, 0, 4, 0);
    m_layout->setSpacing(6);
    m_icon = new QLabel(this);
    m_icon->setPixmap(Icons::get(icon, Icons::Tint::Accent).pixmap(QSize(14, 14)));
    m_title = new QLabel(title, this);
    QFont f = m_title->font();
    f.setWeight(QFont::DemiBold);
    m_title->setFont(f);
    m_layout->addWidget(m_icon);
    m_layout->addWidget(m_title);
    m_layout->addStretch(1);
    setFixedHeight(30);
    if (auto* d = qobject_cast<QDockWidget*>(dock)) {
        auto* floatBtn = makeToolButton(QStringLiteral("external"), tr("Float"), this);
        floatBtn->setIconSize(QSize(13, 13));
        auto* closeBtn = makeToolButton(QStringLiteral("close"), tr("Close"), this);
        closeBtn->setIconSize(QSize(13, 13));
        connect(floatBtn, &QToolButton::clicked, d, [d] { d->setFloating(!d->isFloating()); });
        connect(closeBtn, &QToolButton::clicked, d, &QDockWidget::close);
        m_layout->addWidget(floatBtn);
        m_layout->addWidget(closeBtn);
    }
    connect(&Theme::instance(), &Theme::changed, this, [this] {
        m_icon->setPixmap(Icons::get(m_iconName, Icons::Tint::Accent).pixmap(QSize(14, 14)));
    });
}

void DockTitleBar::setTitle(const QString& title) { m_title->setText(title); }

void DockTitleBar::setCompact(bool compact) {
    setFixedHeight(compact ? 0 : 30);
    for (QWidget* w : findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) w->setVisible(!compact);
}

void DockTitleBar::addWidget(QWidget* w) { m_layout->insertWidget(m_layout->count() - 2, w); }

void DockTitleBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& c = colors();
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, 0.5);
    QPainterPath path;
    path.addRoundedRect(r.adjusted(0, 0, 0, 10), 8, 8);
    p.setClipRect(rect());
    p.setPen(QPen(c.border, 1));
    p.setBrush(c.bg1);
    p.drawPath(path);
    // accent stripe when the panel (or a child) has focus
    const bool active = m_dock && m_dock->isAncestorOf(QApplication::focusWidget());
    if (active) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        p.drawRoundedRect(QRectF(10, height() - 2.0, 28, 2), 1, 1);
    }
    p.setPen(QPen(c.border, 1));
    p.drawLine(QPointF(1, height() - 0.5), QPointF(width() - 1, height() - 0.5));
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent*) {
    if (auto* d = qobject_cast<QDockWidget*>(m_dock)) d->setFloating(!d->isFloating());
}

} // namespace ox::editor
