#pragma once
#include <QWidget>
#include <QString>
#include <functional>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QTableWidget;
class QTableWidgetItem;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QPushButton;
class QRadioButton;
class QComboBox;
class QLabel;
class QTabWidget;
class QLayout;
QT_END_NAMESPACE

namespace cpuview { class ListingPane; }

// The standalone app's CPU debugger, after the Gens "M68K Disassembler"
// window: for each processor a tab with a listing around its PC, its
// registers and run control; beside the tabs a breakpoint panel for every
// kind of breakpoint the backend knows.
//
// The buttons call the backend and have no shortcuts: the host's Debug menu
// owns F5/F7/F8/F9/Ctrl+F7, and a second binding would make them ambiguous.
// The menu's steps act on the CPU of the tab in front (currentCpu).
// F2 and a double-click on a listing line toggle an execution breakpoint.
class CpuDebugView : public QWidget {
    Q_OBJECT
public:
    explicit CpuDebugView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b);

    // Whether an emulator is running, from the host: GpgxBackend::isRunning()
    // stays true after a stop. Off, run control and register edits are
    // disabled; on again, the listings start over around the new PCs.
    void setRunning(bool on);

    // Registers, run state, breakpoints and the listings, read again. While
    // the emulator runs a listing stays where it is and is rebuilt at most
    // once per ListingPane::kRunningListMs, and only while it is in front,
    // however often this is called.
    void refresh();

    // Refreshes, then brings each CPU's PC line into view in its tab: a
    // listing is re-centred on PC only when that line is not among the
    // decoded ones, otherwise it is just scrolled to. The host calls it on
    // every pause.
    void followPc();

    // The CPU whose tab is in front, and bringing a CPU's tab to the front.
    Cpu  currentCpu() const;
    void showCpu(Cpu cpu);

    QSize sizeHint() const override { return {980, 380}; }

signals:
    void currentCpuChanged(Cpu cpu);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class BpKind { Pc, Ram68k, RamZ80, Vram, Cram, Vsram };

    // Step Into, Step Over, Step Out, Run, Pause: one set per tab.
    enum { BtnStepInto, BtnStepOver, BtnStepOut, BtnRun, BtnPause, BtnCount };

    QWidget* buildCpuTab();
    QWidget* buildZ80Tab();
    QWidget* buildBreakpointPanel();
    QLayout* buildRunButtons(Cpu cpu, QPushButton* (&btns)[BtnCount]);
    QLineEdit* addRegField(const QString& name, int digits,
                           std::function<uint32_t(const M68kRegs&)> read,
                           std::function<void(M68kRegs&, uint32_t)> write = nullptr);
    QLineEdit* addZ80Field(const QString& name, int digits,
                           std::function<uint32_t(const Z80Regs&)> read,
                           std::function<void(Z80Regs&, uint32_t)> write = nullptr);
    QCheckBox* addZ80Bit(const QString& text, const QString& name, const QString& tip,
                         std::function<bool(const Z80Regs&)> read,
                         std::function<void(Z80Regs&, bool)> write);

    void rebuildListings(bool force);

    // run control and registers
    void setControls(bool running, bool paused);
    void showRegisters(const M68kRegs& r);
    void commitRegField(QLineEdit* edit);
    void writeRegisters(const std::function<void(M68kRegs&)>& change);
    void showZ80Registers(const Z80Regs& r);
    void commitZ80Field(QLineEdit* edit);
    void writeZ80Registers(const std::function<void(Z80Regs&)>& change);

    // breakpoints
    void refreshBreakpoints();
    void addBreakpointFromPanel();
    void deleteSelectedBreakpoints();
    void onBreakpointItemChanged(QTableWidgetItem* item);
    void updatePanelForKind();
    BpKind panelKind() const;
    Cpu    panelCpu() const;
    void   panelMessage(const QString& text);

    IDebugBackend* backend_ = nullptr;
    bool           running_ = false;

    QTabWidget*           cpuTabs_  = nullptr;
    cpuview::ListingPane* m68kList_ = nullptr;
    cpuview::ListingPane* z80List_  = nullptr;
    QPushButton*          m68kBtns_[BtnCount]{};
    QPushButton*          z80Btns_[BtnCount]{};

    struct RegField {
        QLineEdit* edit;
        int        digits;
        std::function<uint32_t(const M68kRegs&)> read;
        std::function<void(M68kRegs&, uint32_t)> write;   // null = read-only
    };
    std::vector<RegField> regFields_;
    struct SrBit { QCheckBox* box; int bit; };
    std::vector<SrBit> srBits_;
    QSpinBox* srMask_ = nullptr;

    struct Z80Field {
        QLineEdit* edit;
        int        digits;
        std::function<uint32_t(const Z80Regs&)> read;
        std::function<void(Z80Regs&, uint32_t)> write;    // null = read-only
    };
    std::vector<Z80Field> z80Fields_;
    struct Z80Bit {
        QCheckBox* box;
        std::function<bool(const Z80Regs&)> read;
        std::function<void(Z80Regs&, bool)> write;
    };
    std::vector<Z80Bit> z80Bits_;                   // the flags in F, IFF1, IFF2
    QSpinBox*  z80Im_   = nullptr;
    QLineEdit* z80Bank_ = nullptr;
    QLineEdit* z80Bus_  = nullptr;
    QLabel*    z80Held_ = nullptr;                  // why a Z80 step may not finish yet

    QRadioButton* kindRadio_[6]{};
    QComboBox*    bpCpu_    = nullptr;              // the CPU of a PC breakpoint
    QLineEdit*    bpAddr_   = nullptr;
    QComboBox*    bpSize_   = nullptr;
    QCheckBox*    bpRead_   = nullptr;
    QCheckBox*    bpWrite_  = nullptr;
    QTableWidget* bpTable_  = nullptr;
    QLabel*       bpMsg_    = nullptr;

    // What the breakpoint table shows, in row order.
    std::vector<Breakpoint> bps_;
    int  selectBpId_ = -1;                          // select this id on the next rebuild
};
