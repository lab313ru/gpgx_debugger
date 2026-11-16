#include "MainWindow.h"
#include "debugger/EmuHost.h"
#include "platform/AudioOutput.h"
#include "debugger/BridgeServer.h"
#include "views/EmulatorScreen.h"
#include "views/VdpRamView.h"
#include "views/VdpRegView.h"
#include "views/ScrollView.h"
#include "views/SaveStateView.h"
#include "views/VdpSpritesView.h"
#include "views/PlaneExplorerView.h"
#include "views/SoundDebugView.h"
#include "views/MemoryView.h"
#include "views/RamSearchView.h"
#include "views/RamWatchView.h"
#include "views/CpuDebugView.h"
#include "debugger/GpgxBackend.h"
#include "ViewSettings.h"

#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QToolButton>
#include <QStatusBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QFile>
#include <QDir>

#include <algorithm>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Genesis Plus GX Debugger"));
    resize(1440, 900);

    // One host for the window's whole life, restarted per ROM. It owns the only
    // GpgxBackend in the process; see the note in MainWindow.h.
    emuHost_ = new EmuHost();
    backend_ = emuHost_->backend();

    screen_  = new EmulatorScreen(this);
    screen_->setBackend(backend_);      // controller input goes through it
    setCentralWidget(screen_);
    screen_->setFocus();

    statusLabel_ = new QLabel(QStringLiteral("No ROM loaded"), this);
    statusBar()->addWidget(statusLabel_);
    attachLabel_ = new QLabel(this);
    attachLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addPermanentWidget(attachLabel_);

    buildMenus();
    buildDocks();

    // The backend has a single pause callback and EmuHost::start takes it, so
    // the window listens to the host's events, as the bridge and IDA do.
    eventSink_ = emuHost_->addEventSink([this](const DebugEvent& ev) {
        if (ev.type == DebugEvent::Type::Paused) {
            const uint32_t pc = ev.pc;
            const Cpu cpu = ev.cpu;
            QMetaObject::invokeMethod(this, [this, pc, cpu]{ onPaused(pc, cpu); }, Qt::QueuedConnection);
        } else if (ev.type == DebugEvent::Type::Resumed) {
            QMetaObject::invokeMethod(this, [this]{ onResumed(); }, Qt::QueuedConnection);
        }
    });

    // Live views: Gens updated its tool windows every frame; 10 Hz is enough
    // and keeps the GUI thread light. Reads of emulator memory while the core
    // runs are racy-but-benign, same as the original.
    refreshTimer_.setInterval(100);
    connect(&refreshTimer_, &QTimer::timeout, this, &MainWindow::refreshViews);

    // Restore the window last: dock state can only be applied once every dock
    // exists, and it is keyed on the object names set in buildDocks().
    auto& s = viewsettings::store();
    const QByteArray geom = s.value(QStringLiteral("Window/geometry")).toByteArray();
    const QByteArray docks = s.value(QStringLiteral("Window/state")).toByteArray();
    if (!geom.isEmpty())  restoreGeometry(geom);
    if (!docks.isEmpty()) restoreState(docks);

    // A layout saved before the Debugger dock existed keeps whichever tab it had
    // in front, so the new dock would start hidden behind the Hex Editor: bring
    // it forward. Not yet, though: the dock tab bars appear when the window is
    // first laid out, and raise() needs one.
    if (!docks.isEmpty() && !s.value(QStringLiteral("Window/stateHasDebugger")).toBool())
        if (auto* d = findChild<QDockWidget*>(QStringLiteral("Debugger")))
            QTimer::singleShot(0, d, &QWidget::raise);
}

MainWindow::~MainWindow()
{
    if (emuHost_) emuHost_->removeEventSink(eventSink_);
    stopEmulator();
    delete emuHost_; emuHost_ = nullptr;
    backend_ = nullptr;
    // The views wrote their settings from their destructors as the widget tree
    // came down; commit the file now.
    viewsettings::flush();
}

void MainWindow::stopEmulator()
{
    // Before the host: the bridge's handlers hold an EmuHost*, and its stop()
    // deregisters the event sink that points back here.
    delete bridge_; bridge_ = nullptr;
    if (emuHost_) emuHost_->stop();
    delete audio_; audio_ = nullptr;      // outlives the host: the sink writes to it
    refreshTimer_.stop();
    if (debuggerView_) debuggerView_->setRunning(false);
    attachLabel_->setText(QStringLiteral("IDA attach: open a ROM first"));
    attachLabel_->setToolTip(QString());
    updateApplyHint();      // nothing waits for a reset with no game running
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    // Before the views are destroyed: each one persists its own state in its
    // destructor, and flush() below has to see all of it.
    auto& s = viewsettings::store();
    s.setValue(QStringLiteral("Window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("Window/state"), saveState());
    s.setValue(QStringLiteral("Window/stateHasDebugger"), true);

    stopEmulator();
    e->accept();
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Open ROM..."), this, &MainWindow::openRom, QKeySequence::Open);
    file->addSeparator();
    actReset_     = file->addAction(QStringLiteral("&Reset"),      this, &MainWindow::resetSoft);
    actHardReset_ = file->addAction(QStringLiteral("&Hard Reset"), this, &MainWindow::resetHard);
    actReload_    = file->addAction(QStringLiteral("Re&load ROM"), this, &MainWindow::reloadRom);
    actReset_->setToolTip(QStringLiteral("The console's reset button"));
    actHardReset_->setToolTip(QStringLiteral(
        "Power off and on, keeping the cartridge save; applies System Boot ROM and Cartridge Lock-On"));
    actReload_->setToolTip(QStringLiteral(
        "Load the ROM file again, keeping the cartridge save; applies every option"));
    file->setToolTipsVisible(true);
    // Asked as the menu opens rather than at every place a game starts or stops.
    connect(file, &QMenu::aboutToShow, this, [this] {
        const bool running = emuHost_ && emuHost_->isRunning();
        actReset_->setEnabled(running);
        actHardReset_->setEnabled(running);
        actReload_->setEnabled(running);
    });
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), this, &QWidget::close, QKeySequence::Quit);

    auto* debug = menuBar()->addMenu(QStringLiteral("&Debug"));
    actPause_    = debug->addAction(QStringLiteral("&Pause"),     this, &MainWindow::debugPause,    Qt::Key_F5);
    actResume_   = debug->addAction(QStringLiteral("&Resume"),    this, &MainWindow::debugResume,   Qt::Key_F9);
    actStepInto_ = debug->addAction(QStringLiteral("Step &Into"), this, &MainWindow::debugStepInto, Qt::Key_F7);
    actStepOver_ = debug->addAction(QStringLiteral("Step &Over"), this, &MainWindow::debugStepOver, Qt::Key_F8);
    actStepOut_  = debug->addAction(QStringLiteral("Step O&ut"),  this, &MainWindow::debugStepOut,
                                    QKeySequence(Qt::CTRL | Qt::Key_F7));

    actPause_->setEnabled(false);
    actResume_->setEnabled(false);
    actStepInto_->setEnabled(false);
    actStepOver_->setEnabled(false);
    actStepOut_->setEnabled(false);

    buildOptionsMenu();
}

namespace {

const QString kCoreGroup = QStringLiteral("Core");

QString menuText(QString s)
{
    return s.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

QString choiceLabel(const coreopt::OptionDef& def, int stored)
{
    for (const auto& c : def.choices)
        if (c.stored == stored) return QString::fromStdString(c.label);
    return QString::number(stored);
}

} // namespace

void MainWindow::buildOptionsMenu()
{
    using coreopt::Apply;
    using coreopt::Category;
    using coreopt::CoreOptions;

    // Stored as libretro's value strings, one key per option. A value this
    // build does not list is skipped, so that option keeps its default.
    for (const auto& def : coreopt::options()) {
        const QString v = viewsettings::getString(kCoreGroup, QString::fromLatin1(def.key));
        if (!v.isEmpty()) coreopt::setValue(coreOpts_, def, v.toStdString());
    }
    // The host keeps them across ROMs: every start() loads with them.
    const std::string problem = emuHost_->setCoreOptions(coreOpts_);
    if (!problem.empty())
        statusBar()->showMessage(QString::fromStdString(problem), 20000);

    // libretro's categories in libretro's order.
    auto* options = menuBar()->addMenu(QStringLiteral("&Options"));
    QMenu* sys   = options->addMenu(QStringLiteral("&System"));
    QMenu* video = options->addMenu(QStringLiteral("&Video"));
    QMenu* audio = options->addMenu(QStringLiteral("&Audio"));
    QMenu* input = options->addMenu(QStringLiteral("&Input"));
    QMenu* hacks = options->addMenu(QStringLiteral("Emulation &Hacks"));
    auto menuFor = [&](Category c) {
        switch (c) {
        case Category::System: return sys;
        case Category::Video:  return video;
        case Category::Audio:  return audio;
        case Category::Hacks:  break;
        }
        return hacks;
    };

    for (const auto& def : coreopt::options()) {
        // What it takes is in the title, as libretro's "(Requires Restart)"
        // is, so it shows before the submenu opens.
        QString title = menuText(QString::fromLatin1(def.label));
        QString tip;
        if (def.apply == Apply::Reset) {
            title += QStringLiteral(" (Hard Reset)");
            tip = QStringLiteral("Takes effect at File > Hard Reset");
        }
        if (def.apply == Apply::Reload) {
            title += QStringLiteral(" (Reload ROM)");
            tip = QStringLiteral("Takes effect at File > Reload ROM");
        }
        if (def.field == &CoreOptions::bootRom)
            tip += QStringLiteral(". Reads bios_MD.bin from beside the executable or the working directory");
        if (def.field == &CoreOptions::lockOn)
            tip += QStringLiteral(". Reads ggenie.bin, areplay.bin or sk.bin and sk2chip.bin from the working directory");
        if (def.field == &CoreOptions::fmCore)
            tip = QStringLiteral("Switching between MAME and Nuked replaces the FM chip and restarts the PSG: "
                                 "the music stops until the game sets up its next sound. "
                                 "A save state brings back the one it was saved with");
        if (def.field == &CoreOptions::enhancedVscrollLimit)
            tip = QStringLiteral("For Enhanced per-tile vertical scroll: where two neighbouring columns "
                                 "scroll further apart than this, the enhancement stays off");
        // The label is libretro's, the meaning the one its own description
        // gives: libretro's code turns lock-ups off at "Enabled".
        if (def.field == &CoreOptions::lockups)
            tip = QStringLiteral("Enabled: the console locks up, as real hardware does, on an access "
                                 "nothing answers. libretro's core does the opposite at this value");
        QMenu* m = menuFor(def.category)->addMenu(title);
        m->menuAction()->setToolTip(tip);

        auto* group = new QActionGroup(m);       // exclusive
        const int current = coreOpts_.*def.field;
        for (const auto& c : def.choices) {
            QAction* a = m->addAction(menuText(QString::fromStdString(c.label)));
            a->setCheckable(true);
            a->setChecked(c.stored == current);
            a->setData(c.stored);
            group->addAction(a);
            const int stored = c.stored;
            connect(a, &QAction::triggered, this, [this, &def, stored] { setCoreOption(def, stored); });
        }
        optionGroups_.push_back({ &def, group });

        // A choice whose ROM file is missing says so, checked as the menu
        // opens so a file copied in meanwhile is seen.
        if (def.field == &CoreOptions::bootRom || def.field == &CoreOptions::lockOn) {
            connect(m, &QMenu::aboutToShow, this, [m, &def] {
                for (QAction* a : m->actions()) {
                    CoreOptions probe;
                    probe.*def.field = a->data().toInt();
                    const std::string brief = def.field == &CoreOptions::lockOn
                        ? coreopt::checkLockOn(probe).brief
                        : probe.bootRom ? coreopt::findBootRom().brief : std::string();
                    QString text = menuText(choiceLabel(def, probe.*def.field));
                    if (!brief.empty()) text += QStringLiteral(" — ") + QString::fromStdString(brief);
                    a->setText(text);
                }
            });
        }
    }

    // The rest of libretro's list, shown so their absence is not a mystery.
    auto unsupported = [](QMenu* m, const QString& text, const QString& why) {
        QAction* a = m->addAction(text);
        a->setEnabled(false);
        a->setToolTip(why);
    };
    const QString notMd   = QStringLiteral("Not offered: this debugger is for Mega Drive cartridges");
    const QString notCd   = QStringLiteral("Not offered: this debugger is for Mega Drive cartridges, not the Mega-CD");
    const QString padOnly = QStringLiteral("Not offered: only gamepads are connected");
    const QString noSkip  = QStringLiteral("Not offered: every frame is emulated and shown");
    sys->addSeparator();
    unsupported(sys, QStringLiteral("CD System BRAM"), notCd);
    unsupported(sys, QStringLiteral("CD Backup Cart BRAM"), notCd);
    unsupported(sys, QStringLiteral("CD add-on (MD mode)"), notCd);
    video->addSeparator();
    unsupported(video, QStringLiteral("Hide Master System Side Borders"), notMd);
    unsupported(video, QStringLiteral("Game Gear Extended Screen"), notMd);
    unsupported(video, QStringLiteral("Frameskip"), noSkip);
    unsupported(video, QStringLiteral("Frameskip Threshold (%)"), noSkip);
    audio->addSeparator();
    unsupported(audio, QStringLiteral("Master System FM (YM2413)"), notMd);
    unsupported(audio, QStringLiteral("Master System FM (YM2413) Core"), notMd);
    unsupported(audio, QStringLiteral("CD-DA Volume"), notCd);
    unsupported(audio, QStringLiteral("PCM Volume"), notCd);
    unsupported(audio, QStringLiteral("Advanced Channel Volume Settings"),
                QStringLiteral("Not in this build: this version of the core has no per-channel volumes"));
    unsupported(input, QStringLiteral("Light Gun Input"), padOnly);
    unsupported(input, QStringLiteral("Show Light Gun Crosshair"), padOnly);
    unsupported(input, QStringLiteral("Invert Mouse Y-Axis"), padOnly);
    hacks->addSeparator();
    unsupported(hacks, QStringLiteral("Remove Per-Line Sprite Limit"),
                QStringLiteral("Not in this build: the core is compiled without HAVE_NO_SPRITE_LIMIT"));
    unsupported(hacks, QStringLiteral("CPU Speed"),
                QStringLiteral("Not in this build: the core is compiled without M68K_OVERCLOCK_SHIFT "
                               "and Z80_OVERCLOCK_SHIFT"));
    unsupported(hacks, QStringLiteral("CD access time"), notCd);
    for (QMenu* m : { sys, video, audio, input, hacks })
        m->setToolTipsVisible(true);

    // RESET and RELOAD options wait for their event; the status bar says so
    // until it happens, beside the button that makes it happen.
    applyLabel_  = new QLabel(this);
    applyButton_ = new QToolButton(this);
    applyButton_->setAutoRaise(true);
    statusBar()->insertPermanentWidget(0, applyLabel_);
    statusBar()->insertPermanentWidget(1, applyButton_);
    applyLabel_->hide();
    applyButton_->hide();
    connect(applyButton_, &QToolButton::clicked, this, [this] {
        if (coreopt::needs(emuHost_->coreOptionsRunning(), emuHost_->coreOptions()) == Apply::Reload)
            reloadRom();
        else
            resetHard();
    });
    // Polled at the views' rate, which runs while a game does: whatever
    // applies the options — this button, the File menu, a new ROM, a save
    // state — and a run that ends on its own show up here.
    connect(&refreshTimer_, &QTimer::timeout, this, &MainWindow::watchHost);
}

void MainWindow::setCoreOption(const coreopt::OptionDef& def, int stored)
{
    const int was = coreOpts_.*def.field;
    if (was == stored) return;
    coreOpts_.*def.field = stored;
    saveCoreOptions();
    const QString problem = QString::fromStdString(emuHost_->setCoreOptions(coreOpts_));

    // The problem names a missing boot ROM or lock-on file: report it with
    // the option that asks for that file, not with every later change.
    const bool asksForFile = def.field == &coreopt::CoreOptions::bootRom
                          || def.field == &coreopt::CoreOptions::lockOn;
    const bool running = emuHost_->isRunning();
    QString msg = QString::fromLatin1(def.label) + QStringLiteral(": ") + choiceLabel(def, stored);
    if (asksForFile && !problem.isEmpty())
        msg += QStringLiteral(" — ") + problem;
    else if (!running)
        msg += QStringLiteral(" — applies when a ROM is loaded");
    else if (def.apply == coreopt::Apply::Live && backend_->isPaused())
        msg += QStringLiteral(" — applies at the end of the frame, once the emulator is resumed");
    auto nuked = [](int fm) { return fm >= coreopt::FmNukedYm2612; };
    if (running && def.field == &coreopt::CoreOptions::fmCore && nuked(was) != nuked(stored))
        msg += QStringLiteral(" — the sound chips restart, so the music returns with the game's next sound");
    statusBar()->showMessage(msg, asksForFile && !problem.isEmpty() ? 20000 : 6000);
    updateApplyHint();
}

void MainWindow::saveCoreOptions()
{
    // Every option, not only the changed one, so the file shows them all.
    for (const auto& def : coreopt::options())
        viewsettings::putString(kCoreGroup, QString::fromLatin1(def.key),
                                QString::fromStdString(coreopt::valueOf(coreOpts_, def)));
    viewsettings::flush();
}

void MainWindow::watchHost()
{
    // The emulation thread ends on its own when Reload ROM cannot load the
    // file after all: the game is gone and the window must not say Running.
    if (!emuHost_->isRunning()) {
        const QString why = QFile::decodeName(QByteArray::fromStdString(emuHost_->stopReason()));
        stopEmulator();
        setWindowTitle(QStringLiteral("Genesis Plus GX Debugger"));
        statusLabel_->setText(QStringLiteral("Stopped: ") + why);
        for (QAction* a : { actPause_, actResume_, actStepInto_, actStepOver_, actStepOut_ })
            a->setEnabled(false);
        return;
    }
    adoptHostOptions();
    updateApplyHint();
}

// The host changes an option itself when a save state brings back the FM core
// it was saved with; the menu follows, so it shows what is playing.
void MainWindow::adoptHostOptions()
{
    const coreopt::CoreOptions host = emuHost_->coreOptions();
    if (host == coreOpts_) return;
    for (const auto& [def, group] : optionGroups_) {
        const int v = host.*def->field;
        if (v == coreOpts_.*def->field) continue;
        coreOpts_.*def->field = v;
        for (QAction* a : group->actions())
            if (a->data().toInt() == v) a->setChecked(true);
        statusBar()->showMessage(QString::fromLatin1(def->label) + QStringLiteral(": ")
                                 + choiceLabel(*def, v) + QStringLiteral(", as the save state was made with"),
                                 8000);
    }
    saveCoreOptions();
}

void MainWindow::updateApplyHint()
{
    using coreopt::Apply;
    QStringList waiting;
    Apply worst = Apply::Live;
    if (emuHost_ && emuHost_->isRunning()) {
        const coreopt::CoreOptions want = emuHost_->coreOptions();
        const coreopt::CoreOptions have = emuHost_->coreOptionsRunning();
        for (const auto& def : coreopt::options())
            if (def.apply != Apply::Live && want.*def.field != have.*def.field) {
                waiting << QString::fromLatin1(def.label);
                worst = std::max(worst, def.apply);
            }
    }
    if (waiting.isEmpty()) {
        applyLabel_->hide();
        applyButton_->hide();
        return;
    }
    const QString event = worst == Apply::Reload ? QStringLiteral("Reload ROM")
                                                 : QStringLiteral("Hard Reset");
    applyLabel_->setText(waiting.join(QStringLiteral(", ")) + QStringLiteral(": applies after ") + event);
    applyButton_->setText(event);
    applyLabel_->show();
    applyButton_->show();
}

void MainWindow::reportQueued(const QString& what, bool wasPaused)
{
    // The host finishes the frame first, breakpoints or not; a paused
    // emulator runs that far and then stops again.
    statusBar()->showMessage(wasPaused
        ? what + QStringLiteral(" at the end of the frame; paused again at the first instruction after it")
        : what, 6000);
}

void MainWindow::resetSoft()
{
    const bool paused = backend_->isPaused();
    if (!emuHost_->reset(false)) { statusBar()->showMessage(QStringLiteral("No ROM loaded"), 4000); return; }
    reportQueued(QStringLiteral("Reset"), paused);
}

void MainWindow::resetHard()
{
    const bool paused = backend_->isPaused();
    if (!emuHost_->reset(true)) { statusBar()->showMessage(QStringLiteral("No ROM loaded"), 4000); return; }
    reportQueued(QStringLiteral("Hard reset"), paused);
}

void MainWindow::reloadRom()
{
    if (!emuHost_->isRunning()) { statusBar()->showMessage(QStringLiteral("No ROM loaded"), 4000); return; }
    const bool paused = backend_->isPaused();
    if (!emuHost_->reload()) {
        statusBar()->showMessage(QStringLiteral("Reload ROM: the ROM file is missing or empty right now"), 8000);
        return;
    }
    reportQueued(QStringLiteral("Reload ROM"), paused);
}

void MainWindow::buildDocks()
{
    vdpRamView_    = new VdpRamView(this);
    vdpRegView_    = new VdpRegView(this);
    scrollView_    = new ScrollView(this);
    statesView_    = new SaveStateView(this);
    spritesView_   = new VdpSpritesView(this);
    planeView_     = new PlaneExplorerView(this);
    soundView_     = new SoundDebugView(this);
    hexView_       = new MemoryView(this);
    ramSearchView_ = new RamSearchView(this);
    ramWatchView_  = new RamWatchView(this);
    debuggerView_  = new CpuDebugView(this);

    vdpRamView_->setBackend(backend_);
    vdpRegView_->setBackend(backend_);
    scrollView_->setBackend(backend_);
    statesView_->setBackend(backend_);
    spritesView_->setBackend(backend_);
    planeView_->setBackend(backend_);
    soundView_->setBackend(backend_);
    hexView_->setBackend(backend_);
    ramSearchView_->setBackend(backend_);
    ramWatchView_->setBackend(backend_);
    debuggerView_->setBackend(backend_);
    // The menu's steps act on the CPU of the Debugger dock's tab in front and
    // say so when it is the Z80; the keys stay the menu's alone.
    connect(debuggerView_, &CpuDebugView::currentCpuChanged, this, [this](Cpu cpu) {
        const QString who = cpu == Cpu::Z80 ? QStringLiteral(" (Z80)") : QString();
        actStepInto_->setText(QStringLiteral("Step &Into") + who);
        actStepOver_->setText(QStringLiteral("Step &Over") + who);
        actStepOut_->setText(QStringLiteral("Step O&ut") + who);
    });

    connect(ramSearchView_, &RamSearchView::addWatchRequested,
            ramWatchView_,  &RamWatchView::addWatch);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));

    auto addDock = [&](const QString& title, QWidget* w, Qt::DockWidgetArea area, bool visible) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(title);
        dock->setWidget(w);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        addDockWidget(area, dock);
        viewMenu->addAction(dock->toggleViewAction());
        dock->setVisible(visible);
        return dock;
    };

    auto* dTiles   = addDock(QStringLiteral("VDP Ram"),        vdpRamView_,    Qt::RightDockWidgetArea,  true);
    auto* dRegs    = addDock(QStringLiteral("VDP Registers"),  vdpRegView_,    Qt::RightDockWidgetArea,  false);
    auto* dSprites = addDock(QStringLiteral("VDP Sprites"),    spritesView_,   Qt::RightDockWidgetArea,  false);
    auto* dScroll  = addDock(QStringLiteral("Scroll"),         scrollView_,    Qt::RightDockWidgetArea,  false);
    tabifyDockWidget(dTiles, dRegs);
    tabifyDockWidget(dTiles, dSprites);
    tabifyDockWidget(dTiles, dScroll);
    dTiles->raise();

    auto* dHex    = addDock(QStringLiteral("Hex Editor"),      hexView_,       Qt::BottomDockWidgetArea, true);
    auto* dSearch = addDock(QStringLiteral("RAM Search"),      ramSearchView_, Qt::BottomDockWidgetArea, false);
    auto* dWatch  = addDock(QStringLiteral("RAM Watch"),       ramWatchView_,  Qt::BottomDockWidgetArea, false);
    // The standalone has no IDA, so it carries its own disassembly, registers
    // and breakpoints; it is the window a debugging session starts in.
    auto* dDebug  = addDock(QStringLiteral("Debugger"),        debuggerView_,  Qt::BottomDockWidgetArea, true);
    tabifyDockWidget(dHex, dSearch);
    tabifyDockWidget(dHex, dWatch);
    tabifyDockWidget(dHex, dDebug);
    dDebug->raise();

    addDock(QStringLiteral("Save States"),     statesView_, Qt::LeftDockWidgetArea, false);
    addDock(QStringLiteral("Plane Explorer"),  planeView_, Qt::LeftDockWidgetArea, false);
    addDock(QStringLiteral("YM2612 && PSG"),   soundView_, Qt::LeftDockWidgetArea, false);
}

void MainWindow::openRom()
{
    const QString last = viewsettings::getString(QStringLiteral("Session"), QStringLiteral("lastRom"));
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open ROM"),
        last.isEmpty() ? QString() : QFileInfo(last).absolutePath(),
        QStringLiteral("ROM files (*.bin *.gen *.md *.smd);;All (*)"));
    if (path.isEmpty()) return;
    openRomFile(path);
}

void MainWindow::openRomFile(const QString& path)
{
    stopEmulator();

    // Pre-flight here rather than in the host: a missing or empty file is a
    // UI-level complaint, and load_rom() cannot tell the two apart.
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile() || fi.size() <= 0) {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("Not a readable ROM file:\n%1").arg(path));
        return;
    }

    audio_ = new AudioOutput();
    if (!audio_->isOpen())
        statusBar()->showMessage(QStringLiteral("No audio device — running silently"), 8000);

    emuHost_->setFrameSink([this](const uint8_t* d, int w, int h, int pitch, double aspect) {
        screen_->pushFrame(d, w, h, pitch, aspect);   // takes its own lock
    });
    emuHost_->setAudioSink([this](const int16_t* stereo, int frames) {
        audio_->write(stereo, frames);
    });

    if (!emuHost_->start(QFile::encodeName(path).toStdString())) {
        QMessageBox::critical(this, QStringLiteral("Error"),
                              QStringLiteral("gpgx could not load:\n%1").arg(path));
        stopEmulator();
        return;
    }

    // States live beside the ROM here; the IDA host puts them beside the database.
    statesView_->setStatesDir(fi.dir().filePath(fi.completeBaseName() + QStringLiteral("_states")));

    // Remembered so File > Open starts where the last ROM came from, and so a
    // future "reopen last" has something to work with.
    viewsettings::putString(QStringLiteral("Session"), QStringLiteral("lastRom"), path);
    viewsettings::flush();

    // Same control socket the IDA plugin serves, so the MCP tools work against
    // the standalone app too.
    bridge_ = new BridgeServer(emuHost_);
    if (bridge_->start()) {
        const unsigned port = bridge_->port();
        // IDA's Attach dialog scans only this range; a port set by SMD_DGX_PORT
        // outside it has to be named in Parameters instead.
        const bool listed = port >= BridgeServer::kBasePort
                         && port < BridgeServer::kBasePort + BridgeServer::kPortRange;
        attachLabel_->setText(listed
            ? QStringLiteral("IDA: Debugger → Attach to process…  (port %1)").arg(port)
            : QStringLiteral("IDA: Process options → Parameters: attach:%1").arg(port));
        attachLabel_->setToolTip(QStringLiteral(
            "To debug this game in IDA: open its 68000 database, choose SMD DGX as the debugger, "
            "then Debugger → Attach to process… and pick \"%1\" (ID %2).\n"
            "Or set Process options → Parameters to attach:%2 and press F9.\n"
            "The MCP tools use the same port.").arg(fi.completeBaseName()).arg(port));
    } else {
        qWarning("SMD DGX: control socket unavailable (port busy?)");
        attachLabel_->setText(QStringLiteral("IDA attach unavailable: no free control port"));
        attachLabel_->setToolTip(QStringLiteral(
            "Ports 27042-27057 are all taken, or SMD_DGX_PORT names a busy one."));
    }
    setWindowTitle(fi.completeBaseName() + QStringLiteral(" — Genesis Plus GX Debugger"));

    refreshTimer_.start();
    debuggerView_->setRunning(true);
    statusLabel_->setText(QStringLiteral("Running: ") + path);
    actPause_->setEnabled(true);
    actResume_->setEnabled(false);
    screen_->setFocus();
}

void MainWindow::refreshViews()
{
    // Only visible views pay the refresh cost.
    auto refreshIfVisible = [](auto* v) { if (v && v->isVisible()) v->refresh(); };
    refreshIfVisible(vdpRamView_);
    refreshIfVisible(vdpRegView_);
    refreshIfVisible(scrollView_);
    refreshIfVisible(statesView_);
    refreshIfVisible(spritesView_);
    refreshIfVisible(planeView_);
    refreshIfVisible(soundView_);
    refreshIfVisible(hexView_);
    refreshIfVisible(ramSearchView_);
    refreshIfVisible(ramWatchView_);
    refreshIfVisible(debuggerView_);
}

void MainWindow::onPaused(uint32_t pc, Cpu cpu)
{
    // On pause refresh everything, visible or not, so states are consistent
    vdpRamView_->refresh();
    vdpRegView_->refresh();
    scrollView_->refresh();
    statesView_->refresh();
    spritesView_->refresh();
    planeView_->refresh();
    soundView_->refresh();
    hexView_->refresh();
    ramSearchView_->refresh();
    ramWatchView_->refresh();
    // A Z80 stop brings its tab forward, and the Debugger dock with it: the
    // menu steps go to the tab in front, so it must be one the user can see.
    // A 68000 stop leaves whichever tab is in front.
    if (cpu == Cpu::Z80) {
        debuggerView_->showCpu(Cpu::Z80);
        if (auto* dock = qobject_cast<QDockWidget*>(debuggerView_->parentWidget())) {
            dock->show();
            dock->raise();
        }
    }
    debuggerView_->followPc();

    if (audio_) audio_->pause(true);     // nothing refills it while stopped
    actPause_->setEnabled(false);
    actResume_->setEnabled(true);
    actStepInto_->setEnabled(true);
    actStepOver_->setEnabled(true);
    actStepOut_->setEnabled(true);
    const bool z80 = cpu == Cpu::Z80;
    const QString hex = QStringLiteral("%1").arg(z80 ? pc & 0xFFFF : pc & 0xFFFFFF,
                                                 z80 ? 4 : 6, 16, QLatin1Char('0')).toUpper();
    statusLabel_->setText((z80 ? QStringLiteral("Paused at Z80 $") : QStringLiteral("Paused at $")) + hex);
}

void MainWindow::onResumed()
{
    if (audio_) audio_->pause(false);
    actPause_->setEnabled(true);
    actResume_->setEnabled(false);
    actStepInto_->setEnabled(false);
    actStepOver_->setEnabled(false);
    actStepOut_->setEnabled(false);
    debuggerView_->refresh();            // its Pause button
    statusLabel_->setText(QStringLiteral("Running"));
}

void MainWindow::debugPause()   { if (emuHost_ && emuHost_->isRunning()) backend_->pause(); }
void MainWindow::debugResume()  { backend_->resume(); }
void MainWindow::debugStepInto(){ backend_->stepInto(debuggerView_->currentCpu()); }
void MainWindow::debugStepOver(){ backend_->stepOver(debuggerView_->currentCpu()); }
void MainWindow::debugStepOut() { backend_->stepOut(debuggerView_->currentCpu()); }
