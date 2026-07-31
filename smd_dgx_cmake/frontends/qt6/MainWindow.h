#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QString>
#include <QTimer>
#include <vector>
#include <cstdint>

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
    void onPaused(uint32_t pc);
    void onResumed();
    void debugPause();
    void debugResume();
    void debugStepInto();
    void debugStepOver();
    void refreshViews();

private:
    void buildMenus();
    void stopEmulator();
    void buildDocks();

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

    QLabel* statusLabel_ = nullptr;

    QAction* actPause_    = nullptr;
    QAction* actResume_   = nullptr;
    QAction* actStepInto_ = nullptr;
    QAction* actStepOver_ = nullptr;
};
