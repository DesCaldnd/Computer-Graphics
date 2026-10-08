#include "inspector/asset_inspector.hpp"

#include "content/thumbnails.hpp"
#include "core/editor_context.hpp"
#include "inspector/reflected_object_editor.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/convert.hpp>

#if OX_EDITOR_HAS_ASSETS
#include <oxwald/assets/asset_meta.hpp>
#include <oxwald/assets/material.hpp>
#endif

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace ox::editor {

namespace {

QString jsonText(const nlohmann::json& v) {
    if (v.is_string()) return QString::fromStdString(v.get<std::string>());
    if (v.is_number_float()) return QString::number(v.get<double>(), 'g', 6);
    return QString::fromStdString(v.dump());
}

} // namespace

AssetInspector::AssetInspector(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("AssetInspector"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget(scroll);
    m_layout = new QVBoxLayout(host);
    m_layout->setContentsMargins(8, 8, 8, 8);
    m_layout->setSpacing(8);
    m_layout->addStretch(1);
    scroll->setWidget(host);
    outer->addWidget(scroll);
}

void AssetInspector::setAsset(const QString& path) {
    m_path = path;
    m_asset = path.isEmpty() ? std::nullopt : m_ctx->services().assets().info(path);
    rebuild();
}

QWidget* AssetInspector::assetList(const QList<AssetInfo>& assets, const QString& empty) {
    auto* w = new QWidget();
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(10, 6, 10, 8);
    l->setSpacing(2);
    if (assets.isEmpty()) {
        auto* e = new QLabel(empty, w);
        e->setProperty("role", "faint");
        l->addWidget(e);
    }
    for (const AssetInfo& a : assets) {
        auto* b = new QToolButton(w);
        b->setIcon(Icons::fixed(Icons::forAssetType(a.type), QColor(Qt::gray)));
        b->setText(QStringLiteral("%1   %2").arg(a.name, a.type));
        b->setToolTip(a.relativePath);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setAutoRaise(true);
        b->setProperty("role", "text");
        const QString p = a.path;
        connect(b, &QToolButton::clicked, this, [this, p] { m_ctx->inspectAsset(p); });
        l->addWidget(b);
    }
    return w;
}

void AssetInspector::rebuild() {
    if (m_body) {
        m_body->hide();
        m_layout->removeWidget(m_body);
        m_body->deleteLater();
    }
    m_settings = nullptr;
    m_rawSettings = nullptr;
    m_material = nullptr;
    m_applyButton = nullptr;
    m_body = new QWidget(this);
    auto* bl = new QVBoxLayout(m_body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(8);
    m_layout->insertWidget(0, m_body);
    if (!m_asset) {
        auto* l = new QLabel(tr("Asset not found"), m_body);
        l->setProperty("role", "faint");
        bl->addWidget(l);
        return;
    }
    const AssetInfo& a = *m_asset;
    IAssetBackend& backend = m_ctx->services().assets();

    // ---- header ----
    auto* header = new QFrame(m_body);
    header->setProperty("role", "card");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(10, 10, 10, 10);
    hl->setSpacing(12);
    auto* thumb = new QLabel(header);
    thumb->setFixedSize(88, 88);
    thumb->setAlignment(Qt::AlignCenter);
    const QPixmap pm = ThumbnailCache::instance().get(*m_ctx, a, 176);
    if (!pm.isNull()) thumb->setPixmap(pm.scaled(QSize(88, 88) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else thumb->setPixmap(Icons::pixmap(Icons::forAssetType(a.type), 96, colors().textDim).scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    thumb->setStyleSheet(QStringLiteral("QLabel{background:%1;border-radius:8px;}").arg(cssColor(colors().bg0)));
    hl->addWidget(thumb);
    auto* info = new QVBoxLayout();
    info->setSpacing(3);
    auto* name = new QLabel(a.name, header);
    QFont nf = name->font();
    nf.setPointSizeF(nf.pointSizeF() * 1.25);
    nf.setWeight(QFont::DemiBold);
    name->setFont(nf);
    info->addWidget(name);
    auto* typeRow = new QHBoxLayout();
    typeRow->addWidget(makeBadge(a.type, header));
    if (!a.importer.isEmpty()) typeRow->addWidget(makeBadge(tr("importer: %1").arg(a.importer), header));
    if (backend.isDatabase()) typeRow->addWidget(makeBadge(a.imported ? tr("imported") : tr("not imported"), header));
    typeRow->addStretch(1);
    info->addLayout(typeRow);
    auto* rel = new QLabel(a.relativePath, header);
    rel->setProperty("role", "dim");
    rel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info->addWidget(rel);
    auto* idRow = new QHBoxLayout();
    auto* id = new QLabel(qs(a.uuid.toString()), header);
    id->setProperty("role", "faint");
    id->setFont(Theme::monoFont());
    id->setTextInteractionFlags(Qt::TextSelectableByMouse);
    idRow->addWidget(id);
    auto* copy = makeToolButton(QStringLiteral("copy"), tr("Copy UUID"), header);
    const QString uuidText = qs(a.uuid.toString());
    connect(copy, &QToolButton::clicked, this, [uuidText] { QApplication::clipboard()->setText(uuidText); });
    idRow->addWidget(copy);
    idRow->addStretch(1);
    info->addLayout(idRow);
    hl->addLayout(info, 1);
    bl->addWidget(header);

    // ---- actions ----
    auto* actions = new QHBoxLayout();
    if (backend.isDatabase() && !a.importer.isEmpty()) {
        auto* re = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Reimport"), m_body);
        re->setObjectName(QStringLiteral("asset.reimport"));
        connect(re, &QPushButton::clicked, this, [this] { reimport(); });
        actions->addWidget(re);
    }
    auto* reveal = new QPushButton(Icons::get(QStringLiteral("external")), tr("Show in Finder"), m_body);
    const QString dir = QFileInfo(a.path).absolutePath();
    connect(reveal, &QPushButton::clicked, this, [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    actions->addWidget(reveal);
    actions->addStretch(1);
    bl->addLayout(actions);

    // ---- import settings ----
    const auto settings = backend.importSettings(a.path);
    if (settings && (!settings->typeName.isEmpty() || !settings->settings.empty())) {
        const auto& s = settings;
        auto* sec = new CollapsibleSection(tr("Import Settings"), QStringLiteral("import"), m_body);
        sec->setSubtitle(s->typeName.isEmpty() ? s->importer : s->typeName);
        auto* body = new QWidget(sec);
        auto* l = new QVBoxLayout(body);
        l->setContentsMargins(0, 0, 0, 8);
        const reflect::TypeInfo* type = s->typeName.isEmpty() ? nullptr : reflect::TypeRegistry::instance().find(s->typeName.toStdString());
#if OX_EDITOR_HAS_ASSETS
        if (type) {
            m_settings = new ReflectedObjectEditor(m_ctx, *type, body);
            if (auto v = assets::plainJsonToValue(nlohmann::ordered_json::parse(s->settings.dump()))) m_settings->setValue(*v);
            l->addWidget(m_settings);
        }
#endif
        if (!m_settings && !s->settings.empty()) {
            m_rawSettings = new QPlainTextEdit(QString::fromStdString(s->settings.dump(2)), body);
            m_rawSettings->setFont(Theme::monoFont());
            m_rawSettings->setMinimumHeight(120);
            l->addWidget(m_rawSettings);
        }
        if (m_settings || m_rawSettings) {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(12, 0, 12, 0);
            m_applyButton = new QPushButton(Icons::get(QStringLiteral("check")), tr("Apply && Reimport"), body);
            m_applyButton->setObjectName(QStringLiteral("asset.applySettings"));
            m_applyButton->setEnabled(false);
            connect(m_applyButton, &QPushButton::clicked, this, [this] { applyImportSettings(); });
            auto* revert = new QPushButton(Icons::get(QStringLiteral("history")), tr("Revert"), body);
            connect(revert, &QPushButton::clicked, this, [this] { setAsset(m_path); });
            row->addStretch(1);
            row->addWidget(revert);
            row->addWidget(m_applyButton);
            l->addLayout(row);
            if (m_settings) connect(m_settings, &ReflectedObjectEditor::edited, m_applyButton, [this] { m_applyButton->setEnabled(true); });
            if (m_rawSettings) connect(m_rawSettings, &QPlainTextEdit::textChanged, m_applyButton, [this] { m_applyButton->setEnabled(true); });
        } else {
            auto* none = new QLabel(tr("This importer has no settings."), body);
            none->setProperty("role", "faint");
            none->setContentsMargins(12, 4, 12, 4);
            l->addWidget(none);
        }
        sec->setBody(body);
        bl->addWidget(sec);
    }
    if (settings) {
        const auto& s = settings;
        if (s->info.is_object() && !s->info.empty()) {
            auto* isec = new CollapsibleSection(tr("Import Info"), QStringLiteral("info"), m_body);
            auto* ib = new QWidget(isec);
            auto* g = new QGridLayout(ib);
            g->setContentsMargins(12, 6, 12, 8);
            g->setHorizontalSpacing(12);
            int r = 0;
            for (const auto& [k, v] : s->info.items()) {
                auto* kl = new QLabel(prettifyName(k), ib);
                kl->setProperty("role", "dim");
                auto* vl = new QLabel(jsonText(v), ib);
                vl->setWordWrap(true);
                vl->setTextInteractionFlags(Qt::TextSelectableByMouse);
                g->addWidget(kl, r, 0, Qt::AlignTop);
                g->addWidget(vl, r++, 1);
            }
            g->setColumnStretch(1, 1);
            isec->setBody(ib);
            bl->addWidget(isec);
        }
    }

    // ---- material values (.oxmat) ----
#if OX_EDITOR_HAS_ASSETS
    if (a.type == QLatin1String("Material") && !a.isSubAsset && a.path.endsWith(QLatin1String(".oxmat"), Qt::CaseInsensitive)) {
        if (auto mat = assets::loadMaterialFile(fsPath(a.path))) {
            auto* sec = new CollapsibleSection(tr("Material"), QStringLiteral("material"), m_body);
            auto* body = new QWidget(sec);
            auto* l = new QVBoxLayout(body);
            l->setContentsMargins(0, 0, 0, 8);
            m_material = new ReflectedObjectEditor(m_ctx, reflect::typeOf<assets::MaterialAsset>(), body);
            m_material->setValue(serial::toValue(*mat));
            l->addWidget(m_material);
            auto* save = new QPushButton(Icons::get(QStringLiteral("save")), tr("Save Material"), body);
            save->setObjectName(QStringLiteral("asset.saveMaterial"));
            save->setEnabled(false);
            connect(m_material, &ReflectedObjectEditor::edited, save, [save] { save->setEnabled(true); });
            connect(save, &QPushButton::clicked, this, [this, save] {
                if (saveMaterial()) save->setEnabled(false);
            });
            auto* row = new QHBoxLayout();
            row->setContentsMargins(12, 0, 12, 0);
            row->addStretch(1);
            row->addWidget(save);
            l->addLayout(row);
            sec->setBody(body);
            bl->addWidget(sec);
        }
    }
#endif

    // ---- relations ----
    if (backend.isDatabase()) {
        const QList<AssetInfo> subs = backend.subAssets(a.path);
        if (!subs.isEmpty()) {
            auto* sec = new CollapsibleSection(tr("Sub-assets (%1)").arg(subs.size()), QStringLiteral("layers"), m_body);
            sec->setBody(assetList(subs, {}));
            bl->addWidget(sec);
        }
        auto* deps = new CollapsibleSection(tr("Dependencies"), QStringLiteral("link"), m_body);
        deps->setBody(assetList(backend.dependencies(a.path), tr("None")));
        bl->addWidget(deps);
        auto* users = new CollapsibleSection(tr("Used By"), QStringLiteral("target"), m_body);
        users->setBody(assetList(backend.dependents(a.path), tr("Nothing references this asset")));
        bl->addWidget(users);
    }
}

bool AssetInspector::applyImportSettings() {
    if (!m_asset) return false;
    nlohmann::json settings;
#if OX_EDITOR_HAS_ASSETS
    if (m_settings) settings = nlohmann::json::parse(assets::valueToPlainJson(m_settings->value()).dump());
#endif
    if (m_rawSettings) {
        settings = nlohmann::json::parse(m_rawSettings->toPlainText().toStdString(), nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) {
            Q_EMIT m_ctx->statusMessage(tr("Import settings: invalid JSON"), 4000);
            return false;
        }
    }
    QString err;
    if (!m_ctx->services().assets().setImportSettings(m_asset->path, settings, &err)) {
        Q_EMIT m_ctx->statusMessage(tr("Reimport failed: %1").arg(err), 6000);
        return false;
    }
    ThumbnailCache::instance().invalidate(m_asset->uuid);
    Q_EMIT m_ctx->statusMessage(tr("Reimported %1 with new settings").arg(m_asset->name), 3000);
    setAsset(m_path);
    return true;
}

bool AssetInspector::reimport() {
    if (!m_asset) return false;
    QString err;
    if (!m_ctx->services().assets().reimport(m_asset->path, &err)) {
        Q_EMIT m_ctx->statusMessage(tr("Reimport failed: %1").arg(err), 6000);
        return false;
    }
    ThumbnailCache::instance().invalidate(m_asset->uuid);
    Q_EMIT m_ctx->statusMessage(tr("Reimported %1").arg(m_asset->name), 3000);
    setAsset(m_path);
    return true;
}

bool AssetInspector::saveMaterial() {
#if OX_EDITOR_HAS_ASSETS
    if (!m_asset || !m_material) return false;
    assets::MaterialAsset mat;
    serial::fromValue(m_material->value(), mat);
    if (auto st = assets::saveMaterial(mat, fsPath(m_asset->path)); !st) {
        Q_EMIT m_ctx->statusMessage(tr("Cannot save material: %1").arg(QString::fromStdString(st.error().message)), 5000);
        return false;
    }
    QString err;
    m_ctx->services().assets().reimport(m_asset->path, &err);
    ThumbnailCache::instance().invalidate(m_asset->uuid);
    Q_EMIT m_ctx->statusMessage(tr("Saved %1").arg(QFileInfo(m_asset->path).fileName()), 3000);
    return true;
#else
    return false;
#endif
}

} // namespace ox::editor
