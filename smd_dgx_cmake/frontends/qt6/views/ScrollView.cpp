#include "ScrollView.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>
#include <algorithm>

namespace {
// reg 0x0B bits 0-1 -> which line entry of the H table applies, straight from
// the core's hscroll_mask_table.
const uint8_t kHMask[4] = { 0x00, 0x07, 0xF8, 0xFF };
const char* const kHModeName[4] = { "full screen", "8-line (prohibited)", "per cell", "per line" };

const QColor kPlaneA(0x4F, 0xC3, 0xF7);
const QColor kPlaneB(0xFF, 0xB7, 0x4D);
const QColor kGrid  (0x3A, 0x3A, 0x3A);
const QColor kInk   (0xE0, 0xE0, 0xE0);
const QColor kCanvas(0x1E, 0x1E, 0x1E);

constexpr int kMargin = 6;
constexpr int kAxisW  = 44;   // room for value labels
constexpr int kHeadH  = 52;   // mode line + legend + room for the plot title
} // namespace

ScrollView::ScrollView(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(320, 260);
    setFont(QFont(QStringLiteral("Courier New"), 9));
}

// ---------------------------------------------------------------------------
void ScrollView::refresh()
{
    if (!backend_) return;
    const VdpState v = backend_->getVdpState();
    if (!v.vram || !v.vsram) return;

    hMode_   = v.reg[0x0B] & 0x03;
    vPerCol_ = (v.reg[0x0B] >> 2) & 1;
    hscb_    = (uint32_t(v.reg[0x0D]) << 10) & 0xFC00;

    lines_   = ((v.reg[0x01] >> 3) & 1) ? 240 : 224;   // V30 vs V28
    const bool h40 = v.reg[0x0C] & 1;
    columns_ = h40 ? 20 : 16;                          // one entry per 16 px

    // Horizontal: VRAM is stored word-swapped on a little-endian host, so each
    // logical byte lives at ^1 (see the contract in DebugState.h).
    auto vram8 = [&](uint32_t addr) { return v.vram[(addr & 0xFFFF) ^ 1]; };
    hscroll_.assign(lines_, {});
    for (int line = 0; line < lines_; ++line) {
        const uint32_t off = hscb_ + ((line & kHMask[hMode_]) << 2);
        hscroll_[line].a = ((vram8(off) << 8) | vram8(off + 1)) & 0x3FF;
        hscroll_[line].b = ((vram8(off + 2) << 8) | vram8(off + 3)) & 0x3FF;
    }

    // Vertical: VSRAM entries are native 16-bit words, A and B interleaved.
    auto vs16 = [&](int idx) {
        return uint16_t(v.vsram[idx * 2] | (v.vsram[idx * 2 + 1] << 8)) & 0x7FF;
    };
    vscroll_.assign(columns_, {});
    for (int c = 0; c < columns_; ++c) {
        const int idx = vPerCol_ ? c * 2 : 0;          // full-screen: one pair for all
        vscroll_[c].a = vs16(idx);
        vscroll_[c].b = vs16(idx + 1);
    }

    update();
}

// ---------------------------------------------------------------------------
QRect ScrollView::hRect() const
{
    const int h = (height() - kHeadH - kMargin * 3) * 2 / 3;
    return QRect(kMargin + kAxisW, kHeadH, width() - kMargin * 2 - kAxisW, h);
}

QRect ScrollView::vRect() const
{
    const QRect hr = hRect();
    const int top = hr.bottom() + kMargin * 2;
    return QRect(hr.left(), top, hr.width(), height() - top - kMargin - 14);
}

void ScrollView::drawPlot(QPainter& p, const QRect& r, const std::vector<Sample>& data,
                          const QString& title, const QString& xLabel, bool stepped) const
{
    if (data.empty() || r.height() < 20) {
        p.setPen(kInk);
        p.drawText(r.left(), r.top() - 4, title);
        return;
    }

    int lo = data[0].a, hi = data[0].a;
    for (const auto& s : data) {
        lo = std::min({ lo, s.a, s.b });
        hi = std::max({ hi, s.a, s.b });
    }
    // A table that never varies is the common case (no raster effect). Drawing
    // it against a 0..1 scale would pin the line to the bottom edge and look
    // like an empty box, so centre it and label the one value instead.
    const bool flat = (hi == lo);

    // Say so outright: a constant table means there is no raster effect here,
    // which is otherwise indistinguishable from a broken view.
    p.setPen(kInk);
    p.drawText(r.left(), r.top() - 4,
               flat ? title + QStringLiteral("  — constant") : title);

    p.fillRect(r, QColor(0x14, 0x14, 0x14));
    p.setPen(kGrid);
    p.drawRect(r);

    const QFontMetrics fm(font());
    auto axisLabel = [&](int y, int value) {
        p.setPen(kInk);
        p.drawText(QRect(0, y - fm.height() / 2, kAxisW + kMargin - 4, fm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(value));
    };

    const int n = int(data.size());
    auto xAt = [&](int i) { return r.left() + (n == 1 ? 0 : i * (r.width() - 1) / (n - 1)); };
    auto yAt = [&](int val) {
        if (flat) return r.top() + r.height() / 2;
        return r.bottom() - int((val - lo) * qint64(r.height() - 1) / (hi - lo));
    };

    if (flat) {
        axisLabel(r.top() + r.height() / 2, lo);
        p.setPen(kGrid);
        p.drawLine(r.left(), yAt(lo), r.right(), yAt(lo));
    } else {
        axisLabel(r.top(), hi);
        axisLabel(r.bottom(), lo);
    }

    for (int plane = 0; plane < 2; ++plane) {
        p.setPen(QPen(plane ? kPlaneB : kPlaneA, 1));
        QPoint prev;
        for (int i = 0; i < n; ++i) {
            const int val = plane ? data[i].b : data[i].a;
            const QPoint cur(xAt(i), yAt(val));
            if (i) {
                if (stepped) {                   // hold each value across its cell
                    p.drawLine(prev, QPoint(cur.x(), prev.y()));
                    p.drawLine(QPoint(cur.x(), prev.y()), cur);
                } else {
                    p.drawLine(prev, cur);
                }
            }
            prev = cur;
        }
    }

    // cursor — only in the plot the mouse is actually over (H is the smooth
    // one, V the stepped one)
    const bool hoverHere = (hoverInH_ != stepped);
    if (hoverIdx_ >= 0 && hoverIdx_ < n && hoverHere) {
        const int x = xAt(hoverIdx_);
        p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF, 90), 1, Qt::DashLine));
        p.drawLine(x, r.top(), x, r.bottom());
        p.setPen(kInk);
        p.drawText(r.left() + 4, r.top() + fm.height(),
                   QStringLiteral("%1 %2   A=%3  B=%4")
                       .arg(xLabel).arg(hoverIdx_)
                       .arg(data[hoverIdx_].a).arg(data[hoverIdx_].b));
    }
}

void ScrollView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kCanvas);
    p.setFont(font());

    if (!backend_ || hscroll_.empty()) {
        p.setPen(kInk);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("no emulator"));
        return;
    }

    // header: the decoded modes, which is what makes the curves make sense
    p.setPen(kInk);
    p.drawText(kMargin, 14,
               QStringLiteral("H: %1   table %2   |   V: %3   |   %4 lines, %5 cols")
                   .arg(QLatin1String(kHModeName[hMode_]))
                   .arg(QStringLiteral("%1").arg(hscb_, 4, 16, QLatin1Char('0')).toUpper())
                   .arg(vPerCol_ ? QStringLiteral("per 2-cell column") : QStringLiteral("full screen"))
                   .arg(lines_).arg(columns_));

    p.setPen(kPlaneA); p.drawText(kMargin, 28, QStringLiteral("— plane A"));
    p.setPen(kPlaneB); p.drawText(kMargin + 80, 28, QStringLiteral("— plane B"));

    drawPlot(p, hRect(), hscroll_, QStringLiteral("H scroll per scanline"),
             QStringLiteral("line"), false);
    drawPlot(p, vRect(), vscroll_, QStringLiteral("V scroll per column"),
             QStringLiteral("col"), true);
}

// ---------------------------------------------------------------------------
void ScrollView::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->pos();
    const QRect hr = hRect(), vr = vRect();
    int idx = -1;
    bool inH = false;

    auto pick = [&](const QRect& r, int n) {
        if (n <= 1 || r.width() <= 1) return 0;
        return std::clamp(int((pos.x() - r.left()) * qint64(n - 1) / (r.width() - 1)), 0, n - 1);
    };

    if (hr.contains(pos))      { idx = pick(hr, int(hscroll_.size())); inH = true; }
    else if (vr.contains(pos)) { idx = pick(vr, int(vscroll_.size())); inH = false; }

    if (idx != hoverIdx_ || inH != hoverInH_) {
        hoverIdx_ = idx;
        hoverInH_ = inH;
        update();
    }
}

void ScrollView::leaveEvent(QEvent*)
{
    if (hoverIdx_ != -1) { hoverIdx_ = -1; update(); }
}
