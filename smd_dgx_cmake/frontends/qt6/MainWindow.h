#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QString>
#include <QTimer>
#include <vector>
#include <utility>
#include <cstdint>

#include "debugger/DebugState.h"
#include "debugger/CoreOptions.h"

class EmulatorScreen;
class EmuHost;
class AudioOutput;
class BridgeServer;
class IDebugBackend;
class VdpRamView;
class VdpRegView;
class ScrollView;
class SaveStateView;
class VdpSpritesView;
class PlaneExplorerView;
class SoundDebugView;
class MemoryView;
class RamSearchView;
class RamWatchView;
class CpuDebugView;
class QToolButton;
class QActionGroup;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent*) override;

public slots:
    void openRom();
    void openRomFile(const QString& path);

private slots:
    void onPaused(uint32_t pc, Cpu cpu);
    void onResumed();
    void debugPause();
    void debugResume();
    void debugStepInto();
    void debugStepOver();
    void debugStepOut();
    void refreshViews();
    void resetSoft();
    void resetHard();
    void reloadRom();
    void updateApplyHint();
    void watchHost();

private:
    void buildMenus();
    void stopEmulator();
    void buildDocks();
    void buildOptionsMenu();
    void setCoreOption(const coreopt::OptionDef& def, int stored);
    void saveCoreOptions();
    void adoptHostOptions();
    void reportQueued(const QString& what, bool wasPaused);

    // The host owns the backend, and there must be exactly one: the CPU hook is
    // a single global that the most recently constructed GpgxBackend claims, so
    // a second instance leaves whichever one the UI holds receiving no hooks at
    // all — reads still work (core state is global), but pause, stepping and
    // breakpoints are silently inert.
    EmuHost*          emuHost_   = nullptr;
    IDebugBackend*    backend_   = nullptr;   // == emuHost_->backend()
    EmulatorScreen*   screen_    = nullptr;
    AudioOutput*      audio_   = nullptr;
    BridgeServer*     bridge_  = nullptr;
    QTimer            refreshTimer_;
    int               eventSink_ = -1;        // our registration on emuHost_

    VdpRamView*        vdpRamView_    = nullptr;
    VdpRegView*        vdpRegView_    = nullptr;
    ScrollView*        scrollView_    = nullptr;
    SaveStateView*     statesView_    = nullptr;
    VdpSpritesView*    spritesView_   = nullptr;
    PlaneExplorerView* planeView_     = nullptr;
    SoundDebugView*    soundView_     = nullptr;
    MemoryView*        hexView_       = nullptr;
    RamSearchView*     ramSearchView_ = nullptr;
    RamWatchView*      ramWatchView_  = nullptr;
    CpuDebugView*      debuggerView_  = nullptr;   // standalone only; IDA has its own

    QLabel* statusLabel_ = nullptr;
    QLabel* attachLabel_ = nullptr;           // how IDA reaches this session

    QAction* actPause_    = nullptr;
    QAction* actResume_   = nullptr;
    QAction* actStepInto_ = nullptr;
    QAction* actStepOver_ = nullptr;
    QAction* actStepOut_  = nullptr;

    // Emulator core options: the standalone app's only, kept in views.ini
    // [Core]. The IDA plugin never sets any and runs on the core defaults.
    coreopt::CoreOptions coreOpts_;
    QAction*     actReset_     = nullptr;
    QAction*     actHardReset_ = nullptr;
    QAction*     actReload_    = nullptr;
    QLabel*      applyLabel_   = nullptr;   // options waiting for a reset or reload
    QToolButton* applyButton_  = nullptr;   // ... and the one click that applies them
    // Each option's menu, to check the choice the host switched to itself.
    std::vector<std::pair<const coreopt::OptionDef*, QActionGroup*>> optionGroups_;
};
