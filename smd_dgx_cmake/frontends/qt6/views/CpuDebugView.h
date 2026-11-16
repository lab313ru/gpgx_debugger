#pragma once
#include <QWidget>
#include <QElapsedTimer>
#include <QString>
#include <functional>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"
#include "debugger/DisasmListing.h"

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
QT_END_NAMESPACE

// The standalone app's CPU debugger, after the Gens "M68K Disassembler"
// window: a listing around PC, the 68000 registers and run control, and a
// breakpoint panel for every kind of breakpoint the backend knows.
//
// Only the 68000 so far. The CPU part sits in a tab of its own; the
// breakpoint panel beside it already covers both processors and the VDP.
//
// The buttons call the backend and have no shortcuts: the host's Debug menu
// owns F5/F7/F8/F9/Ctrl+F7, and a second binding would make them ambiguous.
// F2 and a double-click on a listing line toggle an execution breakpoint.
class CpuDebugView : public QWidget {
    Q_OBJECT
public:
    explicit CpuDebugView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b);

    // Whether an emulator is running, from the host: GpgxBackend::isRunning()
    // stays true after a stop. Off, run control and register edits are
    // disabled; on again, the listing starts over around the new PC.
    void setRunning(bool on);

    // Registers, run state, breakpoints and the listing, read again. While the
    // emulator runs the listing stays where it is and is rebuilt at most once
    // per kRunningListMs, however often this is called.
    void refresh();

    // Refreshes, then brings the 68000 PC's line into view: the listing is
    // re-centred on PC only when that line is not among the decoded ones,
    // otherwise it is just scrolled to. The host calls it on every pause.
    void followPc();

    QSize sizeHint() const override { return {980, 380}; }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class BpKind { Pc, Ram68k, RamZ80, Vram, Cram, Vsram };

    QWidget* buildCpuTab();
    QWidget* buildBreakpointPanel();
    QLineEdit* addRegField(const QString& name, int digits,
                           std::function<uint32_t(const M68kRegs&)> read,
                           std::function<void(M68kRegs&, uint32_t)> write = nullptr);

    // listing
    void centreOn(uint32_t addr, int rowHint);
    void rebuildListing(bool force);
    void fillListing();
    void scrollListingBy(int rows);
    int  visibleRows() const;
    void toggleBreakpointAt(int row);

    // run control and registers
    void setControls(bool running, bool paused);
    void showRegisters(const M68kRegs& r);
    void commitRegField(QLineEdit* edit);
    void writeRegisters(const std::function<void(M68kRegs&)>& change);

    // breakpoints
    void refreshBreakpoints();
    void addBreakpointFromPanel();
    void deleteSelectedBreakpoints();
    void onBreakpointItemChanged(QTableWidgetItem* item);
    void updatePanelForKind();
    BpKind panelKind() const;
    void   panelMessage(const QString& text);

    static constexpr int kBefore = 20;            // listing lines above the centre
    static constexpr int kAfter  = 60;            // and below it
    static constexpr int kRunningListMs = 100;    // the host's refresh period

    IDebugBackend* backend_ = nullptr;
    bool           running_ = false;

    QTabWidget*   cpuTabs_    = nullptr;
    QTableWidget* listing_    = nullptr;
    QLineEdit*    gotoEdit_   = nullptr;
    QPushButton*  btnStepInto_ = nullptr;
    QPushButton*  btnStepOver_ = nullptr;
    QPushButton*  btnStepOut_  = nullptr;
    QPushButton*  btnRun_      = nullptr;
    QPushButton*  btnPause_    = nullptr;

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

    QRadioButton* kindRadio_[6]{};
    QLineEdit*    bpAddr_   = nullptr;
    QComboBox*    bpSize_   = nullptr;
    QCheckBox*    bpRead_   = nullptr;
    QCheckBox*    bpWrite_  = nullptr;
    QTableWidget* bpTable_  = nullptr;
    QLabel*       bpMsg_    = nullptr;

    // What the listing shows: centred on centre_, rows as last filled.
    std::vector<disasm::ListingLine> lines_;
    std::vector<uint32_t>            disabledAt_;   // lines with only disabled 68000 PC breakpoints
    uint32_t      centre_    = 0;
    bool          hasCentre_ = false;
    int           rowHint_   = -1;                  // scroll the centre line to this row offset once
    QElapsedTimer listClock_;

    // What the breakpoint table shows, in row order.
    std::vector<Breakpoint> bps_;
    int  selectBpId_ = -1;                          // select this id on the next rebuild
};
