#pragma once
#include <QWidget>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QMouseEvent;
QT_END_NAMESPACE

// Scroll inspector: per-scanline horizontal scroll and per-column vertical
// scroll for both planes.
//
// Not a port of anything — Gens had no such view. Nearly every Mega Drive
// raster effect (water lines, parallax, screen shake, wobble) is "the scroll
// value changes per line", which is invisible in a register dump but obvious
// as a curve, so this plots the whole table at once.
//
// Sources, matching what the renderer itself reads:
//   H: VRAM at hscb = (reg[0x0D] << 10) & 0xFC00, 4 bytes per line entry
//      (plane A word then plane B word, big-endian). Which line entry applies
//      is decided by the mode mask in reg 0x0B bits 0-1.
//   V: VSRAM, 16-bit entries alternating A,B — one pair per 16-pixel column,
//      or just the first pair when reg 0x0B bit 2 says full-screen.
class ScrollView : public QWidget {
    Q_OBJECT
public:
    explicit ScrollView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b) { backend_ = b; }
    void refresh();

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Sample { int a = 0, b = 0; };

    QRect hRect() const;
    QRect vRect() const;
    void  drawPlot(QPainter& p, const QRect& r, const std::vector<Sample>& data,
                   const QString& title, const QString& xLabel, bool stepped) const;

    IDebugBackend* backend_ = nullptr;

    std::vector<Sample> hscroll_;   // one per scanline
    std::vector<Sample> vscroll_;   // one per 16-pixel column
    int      lines_    = 224;
    int      columns_  = 20;
    int      hMode_    = 0;         // reg 0x0B bits 0-1
    bool     vPerCol_  = false;     // reg 0x0B bit 2
    uint32_t hscb_     = 0;
    int      hoverIdx_ = -1;        // index under the cursor
    bool     hoverInH_ = false;
};
