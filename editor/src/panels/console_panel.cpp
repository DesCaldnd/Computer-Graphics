#include "panels/console_panel.hpp"

#include "core/editor_context.hpp"
#include "core/log_capture.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/cvar.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/engine.hpp>
#endif

#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QCompleter>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QScrollBar>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <set>

namespace ox::editor {

namespace {
QColor levelColor(int level) {
    const ThemePalette& c = colors();
    switch (log::Level(level)) {
    case log::Level::Trace: return c.textFaint;
    case log::Level::Debug: return c.textDim;
    case log::Level::Info: return c.info;
    case log::Level::Warn: return c.warning;
    case log::Level::Error:
    case log::Level::Fatal: return c.error;
    }
    return c.text;
}
QString levelIcon(int level) {
    switch (log::Level(level)) {
    case log::Level::Trace: return QStringLiteral("trace");
    case log::Level::Debug: return QStringLiteral("debug");
    case log::Level::Info: return QStringLiteral("info");
    case log::Level::Warn: return QStringLiteral("warning");
    default: return QStringLiteral("error");
    }
}

// "path/to/file.lua:42" inside a log message (Lua errors and tracebacks).
const QRegularExpression& sourceLinkPattern() {
    static const QRegularExpression re(QStringLiteral(R"(([A-Za-z0-9_\-./]+\.(?:lua|glsl|vert|frag|comp|oxbt|cpp|hpp)):(\d+))"));
    return re;
}

// x of the message column inside a row (shared by painting and hit testing).
int messageX(const QModelIndex& i, const QRect& r, const QFontMetrics& fm) {
    int x = r.left() + 8;
    x += fm.horizontalAdvance(i.data(LogListModel::TimeRole).toString()) + 10;
    x += 20;
    const QString cat = i.data(LogListModel::CategoryRole).toString();
    if (!cat.isEmpty()) x += fm.horizontalAdvance(cat) + 12 + 8;
    return x;
}

// Link rectangles of the message (line by line) with their match.
struct LinkHit {
    QRect rect;
    QString file;
    int line = 0;
};
std::vector<LinkHit> messageLinks(const QModelIndex& i, const QRect& r, const QFontMetrics& fm) {
    std::vector<LinkHit> out;
    const QStringList lines = i.data().toString().split(QLatin1Char('\n'));
    const int x0 = messageX(i, r, fm);
    for (int ln = 0; ln < lines.size(); ++ln) {
        auto it = sourceLinkPattern().globalMatch(lines[ln]);
        while (it.hasNext()) {
            const auto m = it.next();
            const int x = x0 + fm.horizontalAdvance(lines[ln].left(m.capturedStart(0)));
            const int w = fm.horizontalAdvance(m.captured(0));
            out.push_back({QRect(x, r.top() + 4 + ln * fm.height(), w, fm.height()), m.captured(1), m.captured(2).toInt()});
        }
    }
    return out;
}

class LogDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        const QFontMetrics fm(Theme::monoFont());
        const int lines = std::max<qsizetype>(1, i.data().toString().count(QLatin1Char('\n')) + 1);
        (void)o;
        return QSize(200, int(lines * fm.height() + 8));
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& i) const override {
        const ThemePalette& c = colors();
        const int level = i.data(LogListModel::LevelRole).toInt();
        p->save();
        QRect r = opt.rect;
        if (opt.state & QStyle::State_Selected) p->fillRect(r, c.accentSubtle);
        else if (level >= int(log::Level::Error)) p->fillRect(r, withAlpha(c.error, 22));
        else if (level == int(log::Level::Warn)) p->fillRect(r, withAlpha(c.warning, 16));
        else if (i.row() % 2) p->fillRect(r, c.bgAlt);
        const QFont mono = Theme::monoFont();
        p->setFont(mono);
        const QFontMetrics fm(mono);
        int x = r.left() + 8;
        const int y = r.top() + 4;
        p->setPen(c.textFaint);
        const QString time = i.data(LogListModel::TimeRole).toString();
        p->drawText(QRect(x, y, 70, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, time);
        x += fm.horizontalAdvance(time) + 10;
        const QPixmap ic = Icons::pixmap(levelIcon(level), 28, levelColor(level));
        p->drawPixmap(QRect(x, y + (fm.height() - 14) / 2, 14, 14), ic);
        x += 20;
        const QString cat = i.data(LogListModel::CategoryRole).toString();
        if (!cat.isEmpty()) {
            const int cw = fm.horizontalAdvance(cat) + 12;
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(c.bg3);
            p->drawRoundedRect(QRectF(x, y, cw, fm.height()), 4, 4);
            p->setPen(c.textDim);
            p->drawText(QRect(x, y, cw, fm.height()), Qt::AlignCenter, cat);
            x += cw + 8;
        }
        // Message line by line; source locations (file.lua:42) are drawn as links (accent + underline).
        const QColor textColor = level >= int(log::Level::Warn) ? levelColor(level) : c.text;
        QFont linkFont = mono;
        linkFont.setUnderline(true);
        p->setClipRect(QRect(x, r.top(), r.right() - x - 6, r.height()));
        const QStringList lines = i.data().toString().split(QLatin1Char('\n'));
        for (int ln = 0; ln < lines.size(); ++ln) {
            const QString& line = lines[ln];
            const int ly = y + ln * fm.height();
            int lx = x;
            int pos = 0;
            auto drawPart = [&](const QString& t, bool link) {
                if (t.isEmpty()) return;
                p->setFont(link ? linkFont : mono);
                p->setPen(link ? c.accentText : textColor);
                p->drawText(QPoint(lx, ly + fm.ascent()), t);
                lx += fm.horizontalAdvance(t);
            };
            auto it = sourceLinkPattern().globalMatch(line);
            while (it.hasNext()) {
                const auto m = it.next();
                drawPart(line.mid(pos, m.capturedStart(0) - pos), false);
                drawPart(m.captured(0), true);
                pos = int(m.capturedEnd(0));
            }
            drawPart(line.mid(pos), false);
        }
        p->setClipping(false);
        p->restore();
    }
};
} // namespace

// ---- models -------------------------------------------------------------------------------------------------

LogListModel::LogListModel(QObject* parent) : QAbstractListModel(parent) {
    auto& cap = LogCapture::instance();
    connect(&cap, &LogCapture::appended, this, [this](int first, int count) {
        beginInsertRows({}, first, first + count - 1);
        endInsertRows();
    });
    connect(&cap, &LogCapture::cleared, this, [this] {
        beginResetModel();
        endResetModel();
    });
}

int LogListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(LogCapture::instance().entries().size()); }

QVariant LogListModel::data(const QModelIndex& index, int role) const {
    const auto& entries = LogCapture::instance().entries();
    if (!index.isValid() || index.row() >= entries.size()) return {};
    const LogEntry& e = entries[index.row()];
    switch (role) {
    case Qt::DisplayRole: return e.message;
    case LevelRole: return int(e.level);
    case CategoryRole: return e.category;
    case TimeRole: return e.time.toString(QStringLiteral("HH:mm:ss"));
    case Qt::ToolTipRole: return QStringLiteral("[%1] %2: %3").arg(QString::fromLatin1(log::levelName(e.level).data()), e.category, e.message);
    default: return {};
    }
}

void LogFilterModel::setLevelEnabled(int level, bool on) {
    if (on) m_mask |= 1u << level;
    else m_mask &= ~(1u << level);
    invalidateRowsFilter();
}
void LogFilterModel::setCategory(const QString& c) {
    m_category = c;
    invalidateRowsFilter();
}
void LogFilterModel::setText(const QString& t) {
    m_text = t;
    invalidateRowsFilter();
}

bool LogFilterModel::filterAcceptsRow(int row, const QModelIndex& parent) const {
    const QModelIndex i = sourceModel()->index(row, 0, parent);
    const int level = i.data(LogListModel::LevelRole).toInt();
    if (level == int(log::Level::Fatal) ? !levelEnabled(int(log::Level::Error)) : !levelEnabled(level)) return false;
    if (!m_category.isEmpty() && i.data(LogListModel::CategoryRole).toString() != m_category) return false;
    if (!m_text.isEmpty() && !i.data().toString().contains(m_text, Qt::CaseInsensitive)) return false;
    return true;
}

// ---- panel --------------------------------------------------------------------------------------------------

ConsolePanel::ConsolePanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("ConsolePanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* header = new QFrame(this);
    header->setProperty("role", "panelHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 8, 6);
    hl->setSpacing(4);
    const struct {
        log::Level level;
        QString name;
    } levels[] = {{log::Level::Debug, tr("Debug")}, {log::Level::Info, tr("Info")}, {log::Level::Warn, tr("Warnings")}, {log::Level::Error, tr("Errors")}};
    m_model = new LogListModel(this);
    m_filter = new LogFilterModel(this);
    m_filter->setSourceModel(m_model);
    for (int k = 0; k < 4; ++k) {
        auto* b = new QToolButton(header);
        b->setCheckable(true);
        b->setChecked(m_filter->levelEnabled(int(levels[k].level)));
        b->setIcon(Icons::fixed(levelIcon(int(levels[k].level)), levelColor(int(levels[k].level))));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setProperty("role", "text");
        b->setToolTip(tr("Show %1").arg(levels[k].name));
        const int lv = int(levels[k].level);
        connect(b, &QToolButton::toggled, this, [this, lv](bool on) { m_filter->setLevelEnabled(lv, on); });
        hl->addWidget(b);
        m_levelButtons[k] = b;
    }
    hl->addSpacing(6);
    m_category = new QComboBox(header);
    m_category->addItem(tr("All categories"), QString());
    m_category->setMinimumWidth(130);
    hl->addWidget(m_category);
    hl->addStretch(1);
    m_search = new SearchField(tr("Search log…"), header);
    m_search->setMaximumWidth(240);
    hl->addWidget(m_search);
    m_autoScroll = makeToolButton(QStringLiteral("arrow-right"), tr("Auto-scroll to newest"), header, true);
    m_autoScroll->setIcon(Icons::get(QStringLiteral("chevron-down")));
    m_autoScroll->setChecked(true);
    hl->addWidget(m_autoScroll);
    auto* clear = makeToolButton(QStringLiteral("trash"), tr("Clear log"), header);
    connect(clear, &QToolButton::clicked, this, [] { LogCapture::instance().clear(); });
    hl->addWidget(clear);
    lay->addWidget(header);

    m_view = new QListView(this);
    m_view->setObjectName(QStringLiteral("ConsoleLog"));
    m_view->setModel(m_filter);
    m_view->setItemDelegate(new LogDelegate(m_view));
    m_view->setUniformItemSizes(false);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setMouseTracking(true);
    m_view->viewport()->installEventFilter(this);
    lay->addWidget(m_view, 1);

    auto* inputRow = new QFrame(this);
    inputRow->setProperty("role", "footer");
    auto* il = new QHBoxLayout(inputRow);
    il->setContentsMargins(8, 6, 8, 6);
    il->setSpacing(6);
    auto* prompt = new QLabel(inputRow);
    prompt->setPixmap(Icons::get(QStringLiteral("console"), Icons::Tint::Accent).pixmap(QSize(16, 16)));
    m_input = new QLineEdit(inputRow);
    m_input->setObjectName(QStringLiteral("ConsoleInput"));
    m_input->setPlaceholderText(tr("Enter a console command or cvar (e.g. r.Shadows.Resolution 1024)  ·  Tab completes, ↑/↓ history"));
    m_input->setFont(Theme::monoFont());
    m_input->installEventFilter(this);
    il->addWidget(prompt);
    il->addWidget(m_input, 1);
    lay->addWidget(inputRow);

    m_completions = new QStringListModel(this);
    m_completer = new QCompleter(m_completions, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setMaxVisibleItems(12);
    m_input->setCompleter(m_completer);
    refreshCompletions();

    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        const QString line = m_input->text().trimmed();
        if (line.isEmpty()) return;
        execute(line);
        m_input->clear();
    });
    connect(m_search, &QLineEdit::textChanged, m_filter, &LogFilterModel::setText);
    connect(m_category, &QComboBox::currentIndexChanged, this, [this] { m_filter->setCategory(m_category->currentData().toString()); });
    connect(m_filter, &QAbstractItemModel::rowsInserted, this, [this] {
        if (m_autoScroll->isChecked()) m_view->scrollToBottom();
    });
    connect(&LogCapture::instance(), &LogCapture::appended, this, [this] {
        updateCounts();
        refreshCategories();
    });
    connect(&LogCapture::instance(), &LogCapture::cleared, this, &ConsolePanel::updateCounts);
    updateCounts();
    refreshCategories();
}

void ConsolePanel::refreshCompletions() {
    QStringList items;
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* engine = m_ctx->engine()) {
        for (const auto& c : engine->console().complete("")) items << QString::fromStdString(c).trimmed();
    }
#endif
    for (ICVar* c : CVarRegistry::instance().all()) items << QString::fromStdString(c->name());
    for (const auto& s : CVarRegistry::instance().complete("")) {
        const QString q = QString::fromStdString(s);
        if (!items.contains(q)) items << q;
    }
    items.sort(Qt::CaseInsensitive);
    m_completions->setStringList(items);
}

void ConsolePanel::refreshCategories() {
    std::set<QString> cats;
    for (const auto& e : LogCapture::instance().entries()) cats.insert(e.category);
    if (int(cats.size()) + 1 == m_category->count()) return;
    const QString cur = m_category->currentData().toString();
    QSignalBlocker b(m_category);
    m_category->clear();
    m_category->addItem(tr("All categories"), QString());
    for (const auto& c : cats) m_category->addItem(c, c);
    m_category->setCurrentIndex(std::max(0, m_category->findData(cur)));
}

void ConsolePanel::updateCounts() {
    auto& cap = LogCapture::instance();
    const int counts[4] = {cap.count(log::Level::Debug) + cap.count(log::Level::Trace), cap.count(log::Level::Info),
                           cap.count(log::Level::Warn), cap.count(log::Level::Error) + cap.count(log::Level::Fatal)};
    for (int k = 0; k < 4; ++k) {
        if (m_levelButtons[k]) m_levelButtons[k]->setText(QString::number(counts[k]));
    }
}

QString ConsolePanel::execute(const QString& line) {
    m_history.removeAll(line);
    m_history.push_back(line);
    while (m_history.size() > 100) m_history.removeFirst();
    m_historyPos = -1;
    LogCapture::instance().append(log::Level::Info, QStringLiteral("console"), QStringLiteral("> ") + line);
    QString out;
    if (line == QLatin1String("help") || line == QLatin1String("?")) {
        out = tr("Type a cvar name to print it, \"name value\" to set it, or a command. %1 cvars registered.")
                  .arg(CVarRegistry::instance().all().size());
        LogCapture::instance().append(log::Level::Info, QStringLiteral("console"), out);
        return out;
    }
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* engine = m_ctx->engine()) {
        // The engine console: its commands (pause, step, timescale, level, save, load, stats, find, ...) + cvars.
        auto r = engine->console().execute(line.toStdString());
        if (r) {
            out = QString::fromStdString(*r);
            if (!out.isEmpty()) LogCapture::instance().append(log::Level::Info, QStringLiteral("console"), out);
        } else {
            out = QString::fromStdString(r.error().message);
            LogCapture::instance().append(log::Level::Error, QStringLiteral("console"), out);
        }
        refreshCompletions();
        return out;
    }
#endif
    auto r = CVarRegistry::instance().execute(line.toStdString());
    if (r) {
        out = QString::fromStdString(*r);
        if (!out.isEmpty()) LogCapture::instance().append(log::Level::Info, QStringLiteral("console"), out);
    } else {
        out = QString::fromStdString(r.error().message);
        LogCapture::instance().append(log::Level::Error, QStringLiteral("console"), out);
    }
    refreshCompletions();
    return out;
}

QString ConsolePanel::resolveSource(const QString& file) const {
    if (QFileInfo(file).isAbsolute()) return QFileInfo::exists(file) ? file : QString();
    QStringList bases;
    if (Project* p = m_ctx->project()) bases << p->contentDir() << p->rootDir() << QDir(p->rootDir()).filePath(QStringLiteral("scripts"));
    bases << QDir::currentPath();
    for (const QString& b : bases) {
        const QString candidate = QDir(b).filePath(file);
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    // Last resort: a file with that name anywhere in the asset tree.
    if (Project* p = m_ctx->project()) {
        QDirIterator it(p->contentDir(), {QFileInfo(file).fileName()}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) return it.next();
    }
    return {};
}

bool ConsolePanel::openLinkAt(QPoint pos) {
    const QModelIndex i = m_view->indexAt(pos);
    if (!i.isValid()) return false;
    const QFontMetrics fm(Theme::monoFont());
    for (const LinkHit& link : messageLinks(i, m_view->visualRect(i), fm)) {
        if (!link.rect.contains(pos)) continue;
        const QString path = resolveSource(link.file);
        if (path.isEmpty()) {
            Q_EMIT m_ctx->statusMessage(tr("Cannot find %1 in the project").arg(link.file), 3000);
            return true;
        }
        Q_EMIT m_ctx->openSourceRequested(path, link.line);
        return true;
    }
    return false;
}

bool ConsolePanel::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == m_view->viewport()) {
        if (ev->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton && openLinkAt(me->position().toPoint())) return true;
        } else if (ev->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex i = m_view->indexAt(me->position().toPoint());
            bool over = false;
            if (i.isValid()) {
                const QFontMetrics fm(Theme::monoFont());
                for (const LinkHit& link : messageLinks(i, m_view->visualRect(i), fm)) over |= link.rect.contains(me->position().toPoint());
            }
            m_view->viewport()->setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
        return QWidget::eventFilter(obj, ev);
    }
    if (obj == m_input && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (m_completer->popup()->isVisible()) return QWidget::eventFilter(obj, ev);
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (m_history.isEmpty()) return true;
            if (ke->key() == Qt::Key_Up) m_historyPos = m_historyPos < 0 ? int(m_history.size()) - 1 : std::max(0, m_historyPos - 1);
            else m_historyPos = m_historyPos < 0 ? -1 : m_historyPos + 1;
            if (m_historyPos >= int(m_history.size())) m_historyPos = -1;
            m_input->setText(m_historyPos < 0 ? QString() : m_history[m_historyPos]);
            return true;
        }
        if (ke->key() == Qt::Key_Tab) {
            const auto matches = CVarRegistry::instance().complete(m_input->text().toStdString());
            if (matches.size() == 1) m_input->setText(QString::fromStdString(matches.front()) + QLatin1Char(' '));
            else if (!matches.empty()) {
                m_completer->setCompletionPrefix(m_input->text());
                m_completer->complete();
            }
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

} // namespace ox::editor
