#pragma once
#include <QWidget>
#include <QElapsedTimer>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"
#include "debugger/DisasmListing.h"

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLineEdit;
QT_END_NAMESPACE

namespace cpuview {

// One CPU's disassembly listing with its Go to row, as the Debugger dock shows
// it: listAround for the 68000, listAroundZ80 for the Z80. The lines are
// decoded around a centre address; the PC line is marked, the view scrolls
// within the decoded lines and decodes more at either end.
//
// F2 and a double-click toggle an execution breakpoint of this CPU on the
// line. The pane changes breakpoints only through the backend and then emits
// breakpointsChanged, so the owner can redraw its list and the other listing.
class ListingPane : public QWidget {
    Q_OBJECT
public:
    explicit ListingPane(Cpu cpu, QWidget* parent = nullptr);

    Cpu cpu() const { return cpu_; }
    void setBackend(IDebugBackend* b);

    // Off, nothing is decoded; on again after off, the listing starts over
    // around the new PC.
    void setRunning(bool on);

    // Decodes the lines again. Unforced, it runs at most once per
    // kRunningListMs and not at all while the pane is hidden, and keeps the
    // centre: the host calls it on every refresh while the emulator runs.
    void rebuild(bool force);

    // Brings the PC line into view: re-centres on PC only when that line is
    // not among the decoded ones, otherwise just scrolls to it.
    void followPc();

signals:
    // After a toggle; selectId is the id of the breakpoint added, or -1.
    void breakpointsChanged(int selectId);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    uint32_t pc();
    uint32_t align(uint32_t a) const;
    bool     isExec(const Breakpoint& bp) const;
    bool     stopsAt(const Breakpoint& bp, uint32_t a) const;

    void centreOn(uint32_t addr, int rowHint);
    void fill();
    void scrollBy(int rows);
    int  visibleRows() const;
    void toggleBreakpointAt(int row);

    static constexpr int kBefore = 20;            // listing lines above the centre
    static constexpr int kAfter  = 60;            // and below it
    static constexpr int kRunningListMs = 100;    // the host's refresh period

    const Cpu      cpu_;
    IDebugBackend* backend_ = nullptr;
    bool           running_ = false;

    QTableWidget* listing_  = nullptr;
    QLineEdit*    gotoEdit_ = nullptr;

    // What the listing shows: centred on centre_, rows as last filled.
    std::vector<disasm::ListingLine> lines_;
    std::vector<uint32_t>            disabledAt_;   // lines with only disabled PC breakpoints of this CPU
    uint32_t      centre_    = 0;
    bool          hasCentre_ = false;
    int           rowHint_   = -1;                  // scroll the centre line to this row offset once
    QElapsedTimer listClock_;
};

} // namespace cpuview
