#include "dialogs/project_browser.hpp"

#include "core/editor_context.hpp"
#include "dialogs/branding.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QGridLayout>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ox::editor {

namespace {

QPixmap projectThumb(const QString& projectFile, const QString& name, QSize size) {
    QPixmap pm(size * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 10, 10);
    p.setClipPath(clip);
    const QString thumb = QDir(QFileInfo(projectFile).absolutePath()).filePath(QStringLiteral("Saved/thumbnail.png"));
    QImage img(thumb);
    if (!img.isNull()) {
        p.drawImage(QRectF(0, 0, size.width(), size.height()), img.scaled(size * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    } else {
        const uint h = qHash(name);
        const QColor a = QColor::fromHsv(int(h % 360), 120, 150), b = QColor::fromHsv(int((h / 7) % 360), 140, 70);
        QLinearGradient g(0, 0, size.width(), size.height());
        g.setColorAt(0, a);
        g.setColorAt(1, b);
        p.fillRect(QRectF(0, 0, size.width(), size.height()), g);
        QFont f;
        f.setPixelSize(size.height() / 2);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(QRectF(0, 0, size.width(), size.height()), Qt::AlignCenter, name.left(1).toUpper());
    }
    return pm;
}

} // namespace

ProjectBrowser::ProjectBrowser(EditorContext* ctx, QWidget* parent) : QDialog(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("OxDialog"));
    setWindowTitle(tr("OxwaldEngine Project Browser"));
    resize(980, 640);
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // left brand rail
    auto* rail = new QFrame(this);
    rail->setProperty("role", "sidebar");
    rail->setFixedWidth(250);
    auto* rl = new QVBoxLayout(rail);
    rl->setContentsMargins(20, 24, 20, 20);
    rl->setSpacing(12);
    auto* logo = new QLabel(rail);
    logo->setPixmap(logoPixmap(56 * 2).scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto* title = new QLabel(QStringLiteral("OxwaldEngine"), rail);
    title->setProperty("role", "heading");
    auto* sub = new QLabel(tr("Pick up where you left off or start something new."), rail);
    sub->setProperty("role", "faint");
    sub->setWordWrap(true);
    m_tabs = new SegmentedControl({tr("Recent"), tr("New Project")}, rail);
    rl->addWidget(logo);
    rl->addWidget(title);
    rl->addWidget(sub);
    rl->addSpacing(8);
    rl->addWidget(m_tabs);
    rl->addStretch(1);
    auto* openOther = new QPushButton(Icons::get(QStringLiteral("open")), tr("Open Other…"), rail);
    rl->addWidget(openOther);
    auto* ver = new QLabel(tr("Editor %1").arg(QStringLiteral(OX_EDITOR_VERSION)), rail);
    ver->setProperty("role", "faint");
    rl->addWidget(ver);
    root->addWidget(rail);

    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack, 1);

    // recent
    auto* recentPage = new QWidget(m_stack);
    auto* rp = new QVBoxLayout(recentPage);
    rp->setContentsMargins(24, 24, 24, 20);
    rp->setSpacing(12);
    auto* rt = new QLabel(tr("Recent Projects"), recentPage);
    rt->setProperty("role", "heading");
    rp->addWidget(rt);
    m_recent = new QListWidget(recentPage);
    m_recent->setObjectName(QStringLiteral("RecentProjects"));
    m_recent->setViewMode(QListView::IconMode);
    m_recent->setIconSize(QSize(196, 118));
    m_recent->setGridSize(QSize(220, 168));
    m_recent->setResizeMode(QListView::Adjust);
    m_recent->setMovement(QListView::Static);
    m_recent->setWordWrap(true);
    m_recent->setSpacing(4);
    rp->addWidget(m_recent, 1);
    auto* rfoot = new QHBoxLayout();
    rfoot->addStretch(1);
    auto* openBtn = new QPushButton(tr("Open"), recentPage);
    openBtn->setProperty("role", "primary");
    rfoot->addWidget(openBtn);
    rp->addLayout(rfoot);
    m_stack->addWidget(recentPage);

    // new project
    auto* newPage = new QWidget(m_stack);
    auto* np = new QVBoxLayout(newPage);
    np->setContentsMargins(24, 24, 24, 20);
    np->setSpacing(12);
    auto* nt = new QLabel(tr("New Project"), newPage);
    nt->setProperty("role", "heading");
    np->addWidget(nt);
    np->addWidget(makeSectionLabel(tr("Template"), newPage));
    m_templates = new QListWidget(newPage);
    m_templates->setViewMode(QListView::IconMode);
    m_templates->setIconSize(QSize(64, 64));
    m_templates->setGridSize(QSize(220, 130));
    m_templates->setMovement(QListView::Static);
    m_templates->setFixedHeight(150);
    auto addTemplate = [&](const QString& id, const QString& name, const QString& icon, const QString& tip) {
        auto* it = new QListWidgetItem(Icons::get(icon, Icons::Tint::Accent), name, m_templates);
        it->setData(Qt::UserRole, id);
        it->setToolTip(tip);
    };
    addTemplate(QStringLiteral("Blank"), tr("Blank"), QStringLiteral("cube"), tr("Sun, sky, camera and a floor."));
    addTemplate(QStringLiteral("Showcase"), tr("Showcase"), QStringLiteral("sparkles"), tr("A small demo level with lights, props and prefabs."));
    m_templates->setCurrentRow(1);
    np->addWidget(m_templates);
    np->addWidget(makeSectionLabel(tr("Project"), newPage));
    auto* form = new QGridLayout();
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    m_name = new QLineEdit(QStringLiteral("MyGame"), newPage);
    m_location = new QLineEdit(QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Oxwald Projects")), newPage);
    auto* browse = makeToolButton(QStringLiteral("folder-open"), tr("Choose folder"), newPage);
    form->addWidget(new QLabel(tr("Name"), newPage), 0, 0);
    form->addWidget(m_name, 0, 1, 1, 2);
    form->addWidget(new QLabel(tr("Location"), newPage), 1, 0);
    form->addWidget(m_location, 1, 1);
    form->addWidget(browse, 1, 2);
    np->addLayout(form);
    np->addStretch(1);
    auto* nfoot = new QHBoxLayout();
    nfoot->addStretch(1);
    auto* createBtn = new QPushButton(Icons::get(QStringLiteral("add"), Icons::Tint::OnAccent), tr("Create Project"), newPage);
    createBtn->setProperty("role", "primary");
    nfoot->addWidget(createBtn);
    np->addLayout(nfoot);
    m_stack->addWidget(newPage);

    connect(m_tabs, &SegmentedControl::activated, m_stack, &QStackedWidget::setCurrentIndex);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Project Location"), m_location->text());
        if (!d.isEmpty()) m_location->setText(d);
    });
    connect(createBtn, &QPushButton::clicked, this, &ProjectBrowser::create);
    connect(openOther, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open Project"), m_location->text(), tr("OxwaldEngine project (*.oxproj *.oxproject)"));
        if (!f.isEmpty()) openPath(f);
    });
    connect(m_recent, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { openPath(it->data(Qt::UserRole).toString()); });
    connect(openBtn, &QPushButton::clicked, this, [this] {
        if (auto* it = m_recent->currentItem()) openPath(it->data(Qt::UserRole).toString());
    });
    populateRecent();
    const bool hasRecent = m_recent->count() > 0;
    m_tabs->setCurrent(hasRecent ? 0 : 1);
    m_stack->setCurrentIndex(hasRecent ? 0 : 1);
}

void ProjectBrowser::showCreatePage() {
    m_tabs->setCurrent(1);
    m_stack->setCurrentIndex(1);
}

void ProjectBrowser::populateRecent() {
    m_recent->clear();
    for (const auto& r : m_ctx->preferences().values().recentProjects) {
        const bool exists = QFileInfo::exists(r.path);
        auto* it = new QListWidgetItem(QIcon(projectThumb(r.path, r.name, QSize(196, 118))),
                                       r.name + QLatin1Char('\n') + (exists ? r.lastOpened.toString(QStringLiteral("d MMM yyyy, HH:mm")) : tr("missing")),
                                       m_recent);
        it->setData(Qt::UserRole, r.path);
        it->setToolTip(r.path);
        if (!exists) it->setFlags(Qt::NoItemFlags);
    }
}

void ProjectBrowser::openPath(const QString& file) {
    QString err;
    auto p = Project::open(file, &err);
    if (!p) {
        QMessageBox::warning(this, tr("Open Project"), err);
        return;
    }
    m_ctx->preferences().addRecentProject(p->projectFile(), p->name());
    m_ctx->preferences().save();
    m_project = std::move(p);
    accept();
}

void ProjectBrowser::create() {
    const QString name = m_name->text().trimmed();
    if (name.isEmpty() || name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")))) {
        QMessageBox::warning(this, tr("New Project"), tr("Please enter a valid project name."));
        return;
    }
    QString err;
    const QString templ = m_templates->currentItem() ? m_templates->currentItem()->data(Qt::UserRole).toString() : QStringLiteral("Blank");
    auto p = Project::create(m_location->text(), name, templ, &err);
    if (!p) {
        QMessageBox::warning(this, tr("New Project"), err);
        return;
    }
    m_ctx->preferences().addRecentProject(p->projectFile(), p->name());
    m_ctx->preferences().save();
    m_project = std::move(p);
    accept();
}

} // namespace ox::editor
