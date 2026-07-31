// Qt-side widget builder for the embedded views. Includes Qt + our view headers
// ONLY — NO IDA headers (see ida_views_shared.h). The IDA TWidget arrives as a
// void* that is really a QWidget* in IDA's stock-Qt build.
//
// IDA 9.2+ ships the same Qt 6.8.2 our views target, so the widgets are used
// as-is and run on the Qt DLLs IDA has already loaded.

#include <QWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QPointer>
#include <QObject>

#include <mutex>

#include "debugger/EmuHost.h"
#include "ida_views_shared.h"

#include "views/EmulatorScreen.h"
#include "views/SaveStateView.h"
#include "views/ScrollView.h"
#include "views/VdpRamView.h"
#include "views/VdpRegView.h"
#include "views/VdpSpritesView.h"
#include "views/PlaneExplorerView.h"
#include "views/SoundDebugView.h"
#include "views/MemoryView.h"
#include "views/RamSearchView.h"
#include "views/RamWatchView.h"

extern EmuHost* smd_dgx_host();   // smd_dgx_ida.cpp

const char* const smd_dgx_view_titles[SMD_DGX_VIEW_COUNT] = {
    "SMD Screen",
    "SMD VDP Ram",
    "SMD VDP Registers",
    "SMD Scroll",
    "SMD Save States",
    "SMD VDP Sprites",
    "SMD Plane Explorer",
    "SMD YM2612 & PSG",
    "SMD Hex Editor",
    "SMD RAM Search",
    "SMD RAM Watch",
};

namespace {

struct Dock {
    QWidget* (*make)(QWidget* parent);
    void (*refresh)(QWidget*);
    QPointer<QWidget> widget;
};

template <typename V>
QWidget* make_view(QWidget* parent)
{
    auto* v = new V(parent);
    if (EmuHost* h = smd_dgx_host())
        v->setBackend(h->backend());
    return v;
}

template <typename V>
void refresh_view(QWidget* w)
{
    if (auto* v = qobject_cast<V*>(w)) {
        if (EmuHost* h = smd_dgx_host())
            v->setBackend(h->backend());   // rebind if the host restarted
        v->refresh();
    }
}

// The screen is driven by the frame sink, not the refresh timer.
QWidget* make_screen(QWidget* parent)
{
    auto* s = new EmulatorScreen(parent);
    if (EmuHost* h = smd_dgx_host())
        s->setBackend(h->backend());   // needed for controller input
    return s;
}

// Frames arrive through the sink, but the backend still has to be rebound in
// case the dock outlived a host restart (that is where input goes).
void refresh_screen(QWidget* w)
{
    if (auto* s = qobject_cast<EmulatorScreen*>(w)) {
        if (EmuHost* h = smd_dgx_host())
            s->setBackend(h->backend());
    }
}

// Set before any view exists, so remember it and apply on creation.
QString g_statesDir;

QWidget* make_save_states(QWidget* parent)
{
    auto* v = new SaveStateView(parent);
    if (EmuHost* h = smd_dgx_host()) v->setBackend(h->backend());
    if (!g_statesDir.isEmpty()) v->setStatesDir(g_statesDir);
    return v;
}

Dock g_docks[SMD_DGX_VIEW_COUNT] = {
    { &make_screen,                  &refresh_screen,                  {} },
    { &make_view<VdpRamView>,        &refresh_view<VdpRamView>,        {} },
    { &make_view<VdpRegView>,        &refresh_view<VdpRegView>,        {} },
    { &make_view<ScrollView>,        &refresh_view<ScrollView>,        {} },
    { &make_save_states,             &refresh_view<SaveStateView>,     {} },
    { &make_view<VdpSpritesView>,    &refresh_view<VdpSpritesView>,    {} },
    { &make_view<PlaneExplorerView>, &refresh_view<PlaneExplorerView>, {} },
    { &make_view<SoundDebugView>,    &refresh_view<SoundDebugView>,    {} },
    { &make_view<MemoryView>,        &refresh_view<MemoryView>,        {} },
    { &make_view<RamSearchView>,     &refresh_view<RamSearchView>,     {} },
    { &make_view<RamWatchView>,      &refresh_view<RamWatchView>,      {} },
};

QTimer* g_timer = nullptr;

// Screen widget reachable from the emulation thread. Set/cleared on the GUI
// thread, read under the lock by the frame sink.
std::mutex      g_screenMx;
EmulatorScreen* g_screen = nullptr;

void ensure_timer()
{
    if (g_timer) return;
    g_timer = new QTimer;
    g_timer->setInterval(100);   // 10 Hz, matches the standalone app
    QObject::connect(g_timer, &QTimer::timeout, [] {
        for (auto& d : g_docks)
            if (d.widget && d.widget->isVisible())
                d.refresh(d.widget);
    });
    g_timer->start();
}

} // namespace

void smd_dgx_view_attach(int idx, void* twidget_as_qwidget)
{
    if (idx < 0 || idx >= SMD_DGX_VIEW_COUNT) return;
    auto* host = reinterpret_cast<QWidget*>(twidget_as_qwidget);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    g_docks[idx].widget = g_docks[idx].make(host);
    layout->addWidget(g_docks[idx].widget);

    if (auto* screen = qobject_cast<EmulatorScreen*>(g_docks[idx].widget.data())) {
        { std::lock_guard<std::mutex> lk(g_screenMx); g_screen = screen; }
        // IDA destroys the widget when the dock is closed; drop the pointer the
        // frame sink reads before it dangles.
        QObject::connect(screen, &QObject::destroyed, [](QObject*) {
            std::lock_guard<std::mutex> lk(g_screenMx);
            g_screen = nullptr;
        });
    }
    ensure_timer();
}

void smd_dgx_set_states_dir(const char* path)
{
    g_statesDir = QString::fromLocal8Bit(path ? path : "");
    for (auto& d : g_docks)
        if (auto* v = qobject_cast<SaveStateView*>(d.widget.data()))
            v->setStatesDir(g_statesDir);
}

void smd_dgx_view_detach_all()
{
    if (g_timer) { g_timer->stop(); g_timer->deleteLater(); g_timer = nullptr; }
    { std::lock_guard<std::mutex> lk(g_screenMx); g_screen = nullptr; }
    for (auto& d : g_docks) d.widget = nullptr;
}

void smd_dgx_push_frame(const unsigned char* data, int w, int h, int pitch,
                        int vpX, int vpY, int vpW, int vpH)
{
    std::lock_guard<std::mutex> lk(g_screenMx);
    if (g_screen)
        g_screen->pushFrame(data, w, h, pitch, vpX, vpY, vpW, vpH);
}
