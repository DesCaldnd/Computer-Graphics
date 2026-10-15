// Viewport frame pacing: the viewport timer must reach its frame limit while the editor's engine ticks in edit
// mode, even in projects with many asset files (regression: the asset hot-reload watcher walked the whole asset
// tree synchronously on every 16 ms engine tick, ~40 ms per tick for ~750 files on Windows, which held the
// viewport of the default showcase scene at ~13-40 fps with a sub-millisecond frame time).
#include "test_common.hpp"

#include "core/project.hpp"
#include "panels/stats_panel.hpp"
#include "viewport/frame_pacer.hpp"
#include "viewport/viewport_panel.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QScreen>

#include <cmath>
#include <cstdio>

namespace ox::editor::test {

namespace {

QString pacingDir(const QString& name) {
    const QString dir = QDir(tempRoot()).filePath(name);
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    return dir;
}

// Many small files in nested folders under Assets/ (not importable: only the watcher sees them).
void writeManyFiles(const QString& assetsDir, int count) {
    for (int i = 0; i < count; ++i) {
        const QString dir = QDir(assetsDir).filePath(QStringLiteral("Bulk/D%1").arg(i / 100));
        if (i % 100 == 0) QDir().mkpath(dir);
        QFile f(QDir(dir).filePath(QStringLiteral("file%1.dat").arg(i)));
        if (f.open(QIODevice::WriteOnly)) f.write("x");
    }
}

} // namespace

// Measured tick rates need an optimized build: in Debug (Qt debug DLLs) one paint of the offscreen software viewport
// takes ~25 ms, which holds the UI loop at 33-41 ticks/s, the same range as the blocked-event-loop regression these
// tests guard, so the measurement cannot tell them apart there. The pacer logic is covered by the model tests below.
#ifndef NDEBUG
// Not a constant: an unconditional QSKIP (a return) makes the rest of the test unreachable, C4702 under /WX.
inline volatile bool g_skipTickRates = true;
#define OX_SKIP_TICK_RATE_IN_DEBUG()                                                                                   \
    do {                                                                                                               \
        if (g_skipTickRates) QSKIP("viewport tick rates are measured in optimized builds (~25 ms software paint in Debug)"); \
    } while (false)
#else
#define OX_SKIP_TICK_RATE_IN_DEBUG() (void)0
#endif

class ViewportPacingTests : public QObject {
    Q_OBJECT
    std::unique_ptr<EditorContext> ctx;

private Q_SLOTS:
    void init() { ctx = makeContext(); }
    void cleanup() { ctx.reset(); }

    void viewportTickReachesFrameLimitWhileEngineTicks() {
        OX_SKIP_TICK_RATE_IN_DEBUG();
        auto p = Project::create(pacingDir(QStringLiteral("Pacing")), QStringLiteral("Pacing"), QStringLiteral("Blank"), nullptr);
        QVERIFY(p);
        writeManyFiles(p->contentDir(), 3000);
        ctx->preferences().modify([](PreferenceValues& v) {
            v.matchDisplayRefresh = false;
            v.frameLimit = 120;
            v.throttleWhenUnfocused = false; // the offscreen test window is never the active window
        });
        ctx->setProject(std::move(p));

        ViewportPanel panel(ctx.get());
        panel.resize(320, 180);
        panel.show();
        panel.setContinuousRedraw(true);
        ctx->play().setEditTicking(true); // as MainWindow does: the engine ticks in edit mode every 16 ms

        pump(500); // warm-up (first frames, watcher baseline)
        const quint64 ticks0 = panel.ticks();
        const unsigned long long engine0 = ctx->runtime().frames();
        QElapsedTimer t;
        t.start();
        pump(1500);
        const double seconds = double(t.elapsed()) / 1000.0;
        const double tickRate = double(panel.ticks() - ticks0) / seconds;
        const double engineRate = double(ctx->runtime().frames() - engine0) / seconds;
        ctx->play().setEditTicking(false);
        std::fprintf(stderr, "viewport ticks %.1f/s (limit 120), engine ticks %.1f/s (timer 16 ms)\n", tickRate, engineRate);

        // Timer granularity and a loaded CI machine cost some ticks; a blocked event loop costs most of them.
        QVERIFY2(tickRate >= 60.0, qPrintable(QStringLiteral("viewport ticks %1/s").arg(tickRate)));
#if OX_EDITOR_HAS_RUNTIME
        QVERIFY2(engineRate >= 30.0, qPrintable(QStringLiteral("engine ticks %1/s").arg(engineRate)));
#endif
    }

    // A millisecond timer driven by the pacer reaches the target rate exactly (a 1000 / fps interval gives 125 fps
    // for 120 and 166 for 144), also when every timer event fires up to 1 ms late.
    void pacerAverageRateMatchesTargetWithMillisecondTimer() {
        for (const double fps : {60.0, 100.0, 120.0, 144.0, 165.0, 240.0}) {
            FramePacer pacer;
            qint64 now = 0;
            int ticks = 0;
            const qint64 end = 10'000'000'000; // 10 s
            unsigned lateness = 12345;
            while (now < end) {
                ++ticks;
                const int delayMs = pacer.next(now, fps);
                lateness = lateness * 1103515245u + 12345u;
                now += qint64(delayMs) * 1'000'000 + qint64(lateness % 1'000'000); // 0..1 ms late
            }
            const double rate = ticks / 10.0;
            QVERIFY2(std::abs(rate - fps) <= 0.2, qPrintable(QStringLiteral("target %1 fps, got %2").arg(fps).arg(rate)));
        }
    }

    // A UI-thread hiccup shorter than two periods costs no tick: the schedule holds and the next tick comes at once.
    void pacerCatchesUpAfterShortHiccup() {
        FramePacer pacer;
        qint64 now = 0;
        int ticks = 0;
        const qint64 end = 10'000'000'000; // 10 s at 100 Hz, every 10th frame 15 ms late
        while (now < end) {
            ++ticks;
            now += qint64(pacer.next(now, 100.0)) * 1'000'000;
            if (ticks % 10 == 0) now += 15'000'000;
        }
        QVERIFY2(ticks >= 995 && ticks <= 1001, qPrintable(QString::number(ticks)));
    }

    // After a long stall the schedule restarts from now: no burst of zero-delay ticks to catch up.
    void pacerDoesNotBurstAfterStall() {
        FramePacer pacer;
        qint64 now = 0;
        for (int i = 0; i < 10; ++i) now += qint64(pacer.next(now, 100.0)) * 1'000'000;
        now += 500'000'000; // 0.5 s stall
        QCOMPARE(pacer.next(now, 100.0), 10);
        QCOMPARE(pacer.next(now, 0.0), 0); // unlimited
    }

    // The viewport paces to its screen's refresh rate (default preference): 144/165/240 Hz panels are not capped at
    // the 120 fps frame limit, fractional rates (59.94) are kept exactly; the limit applies only with the option off.
    void pacingTargetFollowsDisplayRefreshRate() {
        PreferenceValues v;
        QVERIFY(v.matchDisplayRefresh); // default on
        v.frameLimit = 120;
        for (const double hz : {59.94, 100.0, 144.0, 165.0, 240.0, 360.0}) QCOMPARE(ViewportPanel::pacingFps(v, true, hz), hz);
        QCOMPARE(ViewportPanel::pacingFps(v, true, 0.0), 60.0); // screen reports no rate
        QCOMPARE(ViewportPanel::pacingFps(v, false, 144.0), double(v.unfocusedFps)); // throttled while unfocused
        v.throttleWhenUnfocused = false;
        QCOMPARE(ViewportPanel::pacingFps(v, false, 144.0), 144.0);
        v.matchDisplayRefresh = false;
        QCOMPARE(ViewportPanel::pacingFps(v, true, 144.0), 120.0);
        v.frameLimit = 0;
        QCOMPARE(ViewportPanel::pacingFps(v, true, 144.0), 0.0); // unlimited
    }

    // The panel reads the rate of the screen it is on and its timer ticks at that rate.
    void viewportTicksAtItsScreenRefreshRate() {
        ctx->preferences().modify([](PreferenceValues& v) {
            v.matchDisplayRefresh = true;
            v.frameLimit = 30; // ignored while matching the display
            v.throttleWhenUnfocused = false;
        });
        ViewportPanel panel(ctx.get());
        panel.resize(320, 180);
        panel.show();
        QVERIFY(panel.screen());
        const double hz = panel.screen()->refreshRate();
        QCOMPARE(panel.displayRefreshRate(), hz);
        const double target = hz >= 1.0 ? hz : 60.0;
        QCOMPARE(panel.currentTargetFps(), target);
        OX_SKIP_TICK_RATE_IN_DEBUG();
        pump(300);
        const quint64 ticks0 = panel.ticks();
        QElapsedTimer t;
        t.start();
        pump(1500);
        const double rate = double(panel.ticks() - ticks0) / (double(t.elapsed()) / 1000.0);
        std::fprintf(stderr, "screen %.2f Hz: viewport ticks %.1f/s\n", hz, rate);
        QVERIFY2(rate >= target * 0.6 && rate <= target * 1.1,
                 qPrintable(QStringLiteral("screen %1 Hz, viewport ticks %2/s").arg(hz).arg(rate)));
    }

    // The stats panel refreshes its pass / system tables every 500 ms. Recreating every row (~75 GPU passes in the
    // showcase) cost ~40 ms on the UI thread each time and delayed the viewport tick (100 Hz viewport at ~93 fps):
    // rows with an unchanged count are updated in place.
    void statsTableUpdatesRowsInPlace() {
        QWidget host;
        auto* grid = new QGridLayout(&host);
        const QVector<QPair<QString, double>> rows{{QStringLiteral("A"), 1.0}, {QStringLiteral("B"), 2.0}, {QStringLiteral("C"), 0.5}};
        StatsPanel::fillTable(grid, rows, QStringLiteral("empty"));
        QCOMPARE(grid->count(), 9);
        QWidget* const name0 = grid->itemAtPosition(0, 0)->widget();
        QWidget* const bar1 = grid->itemAtPosition(1, 1)->widget();
        const QVector<QPair<QString, double>> next{{QStringLiteral("A"), 4.0}, {QStringLiteral("D"), 2.0}, {QStringLiteral("C"), 1.0}};
        StatsPanel::fillTable(grid, next, QStringLiteral("empty"));
        QCOMPARE(grid->count(), 9);
        QCOMPARE(grid->itemAtPosition(0, 0)->widget(), name0); // same widgets, new contents
        QCOMPARE(grid->itemAtPosition(1, 1)->widget(), bar1);
        QCOMPARE(qobject_cast<QLabel*>(grid->itemAtPosition(1, 0)->widget())->text(), QStringLiteral("D"));
        QCOMPARE(qobject_cast<QLabel*>(grid->itemAtPosition(0, 2)->widget())->text(), QStringLiteral("4.000 ms"));
        QCOMPARE(qobject_cast<QProgressBar*>(grid->itemAtPosition(0, 1)->widget())->value(), 1000);
        QCOMPARE(qobject_cast<QProgressBar*>(bar1)->value(), 500);
        // A different row count rebuilds; no rows shows the placeholder.
        StatsPanel::fillTable(grid, {{QStringLiteral("A"), 1.0}}, QStringLiteral("empty"));
        QCOMPARE(grid->count(), 3);
        StatsPanel::fillTable(grid, {}, QStringLiteral("empty"));
        QCOMPARE(grid->count(), 1);
        QCOMPARE(qobject_cast<QLabel*>(grid->itemAtPosition(0, 0)->widget())->text(), QStringLiteral("empty"));
        StatsPanel::fillTable(grid, rows, QStringLiteral("empty"));
        QCOMPARE(grid->count(), 9);
    }

    void frameStatsTextShowsIntervalCpuAndGpu() {
        ViewportPanel panel(ctx.get());
        const QString text = panel.frameStatsText();
        QVERIFY2(text.contains(QStringLiteral("FPS")) && text.contains(QStringLiteral("CPU")) && text.contains(QStringLiteral("GPU")),
                 qPrintable(text));
    }
};

OX_EDITOR_TEST(ViewportPacingTests);

} // namespace ox::editor::test

#include "test_viewport_pacing.moc"
