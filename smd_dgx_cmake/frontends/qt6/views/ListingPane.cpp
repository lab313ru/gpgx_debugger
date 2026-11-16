#include "ListingPane.h"
#include "CpuViewUtil.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QScrollBar>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPalette>

#include <algorithm>

namespace cpuview {

namespace {

enum ListCol { ColMark, ColAddr, ColHex, ColText, ListCols };

constexpr uint32_t kBusMask = 0xFFFFFF;
constexpr uint32_t kZ80Mask = 0xFFFF;

// Row offsets for ListingPane::rowHint_ besides "this viewport row".
constexpr int kKeepScroll = -1;
constexpr int kCentreRow  = -2;

bool sameLine(const disasm::ListingLine& a, const disasm::ListingLine& b)
{
    return a.addr == b.addr && a.len == b.len && a.readable == b.readable && a.isPc == b.isPc
        && a.hasBreakpoint == b.hasBreakpoint && a.hex == b.hex && a.text == b.text;
}

} // namespace

ListingPane::ListingPane(Cpu cpu, QWidget* parent) : QWidget(parent), cpu_(cpu)
{
    const bool z80 = cpu_ == Cpu::Z80;
    // The 68000 pane keeps the names it always had; the Z80 one says whose it is.
    const QString who = z80 ? QStringLiteral("Z80 ") : QString();

    gotoEdit_ = new QLineEdit;
    gotoEdit_->setFont(monoFont());
    gotoEdit_->setAccessibleName(z80 ? QStringLiteral("Z80 go to address") : QStringLiteral("Go to address"));
    gotoEdit_->setPlaceholderText(QStringLiteral("address"));
    gotoEdit_->setMaxLength(10);
    gotoEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(\\$|0[xX])?[0-9A-Fa-f]{1,8}")), gotoEdit_));
    gotoEdit_->setFixedWidth(gotoEdit_->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 10 + 12);
    connect(gotoEdit_, &QLineEdit::returnPressed, this, [this] {
        uint32_t a = 0;
        if (parseHexField(gotoEdit_->text(), a)) centreOn(a, kCentreRow);
        listing_->setFocus();
    });
    auto* goBtn = new QPushButton(QStringLiteral("Go"));
    connect(goBtn, &QPushButton::clicked, gotoEdit_, &QLineEdit::returnPressed);
    auto* pcBtn = new QPushButton(QStringLiteral("PC"));
    pcBtn->setToolTip(QStringLiteral("Back to the line at the program counter"));
    pcBtn->setAccessibleName(QStringLiteral("Go to ") + who + QStringLiteral("PC"));
    connect(pcBtn, &QPushButton::clicked, this, [this] {
        if (backend_) centreOn(pc(), kCentreRow);
    });

    auto* gotoRow = new QHBoxLayout;
    gotoRow->addWidget(new QLabel(QStringLiteral("Go to")));
    gotoRow->addWidget(gotoEdit_);
    gotoRow->addWidget(goBtn);
    gotoRow->addWidget(pcBtn);
    gotoRow->addStretch();

    listing_ = new QTableWidget(0, ListCols);
    listing_->setAccessibleName(z80 ? QStringLiteral("Z80 disassembly") : QStringLiteral("Disassembly"));
    listing_->setFont(monoFont());
    listing_->setHorizontalHeaderLabels({QString(), QStringLiteral("Address"),
                                         QStringLiteral("Bytes"), QStringLiteral("Instruction")});
    listing_->verticalHeader()->setVisible(false);
    listing_->verticalHeader()->setMinimumSectionSize(1);
    listing_->verticalHeader()->setDefaultSectionSize(listing_->fontMetrics().height() + 4);
    listing_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    listing_->setSelectionBehavior(QAbstractItemView::SelectRows);
    listing_->setSelectionMode(QAbstractItemView::SingleSelection);
    listing_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    listing_->setShowGrid(false);
    listing_->setWordWrap(false);
    listing_->setToolTip(QStringLiteral("Double-click or F2: toggle a breakpoint on the line"));
    {
        const int cw = listing_->fontMetrics().horizontalAdvance(QLatin1Char('0'));
        auto* hh = listing_->horizontalHeader();
        hh->setStretchLastSection(true);
        hh->setSectionResizeMode(QHeaderView::Interactive);
        listing_->setColumnWidth(ColMark, cw * 3 + 8);
        // 68000: six address digits, up to 5 words, the longest instruction.
        // Z80: four digits, up to 4 bytes ("DD CB 05 06").
        listing_->setColumnWidth(ColAddr, cw * (z80 ? 5 : 7) + 12);
        listing_->setColumnWidth(ColHex,  cw * (z80 ? 12 : 25) + 12);
        listing_->setMinimumWidth(cw * 40);
    }
    listing_->installEventFilter(this);
    listing_->viewport()->installEventFilter(this);
    connect(listing_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { toggleBreakpointAt(row); });

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->addLayout(gotoRow);
    col->addWidget(listing_, 1);
}

void ListingPane::setBackend(IDebugBackend* b)
{
    backend_ = b;
    hasCentre_ = false;
    lines_.clear();
}

void ListingPane::setRunning(bool on)
{
    if (on && !running_) {
        hasCentre_ = false;
        lines_.clear();
        disabledAt_.clear();
        listClock_.invalidate();
    }
    running_ = on;
}

uint32_t ListingPane::pc()
{
    return cpu_ == Cpu::Z80 ? backend_->getZ80Regs().pc : backend_->getM68kRegs().pc;
}

uint32_t ListingPane::align(uint32_t a) const
{
    return cpu_ == Cpu::Z80 ? a & kZ80Mask : a & kBusMask & ~1u;
}

bool ListingPane::isExec(const Breakpoint& bp) const
{
    return bp.type == BpType::PC && bp.cpu == cpu_ && !bp.is_vdp;
}

// Where the backend stops for an execution breakpoint. The 68000 matches it
// against the opcode word, so one on a + 1 stops at a as well; the Z80 matches
// the instruction's first byte.
bool ListingPane::stopsAt(const Breakpoint& bp, uint32_t a) const
{
    if (cpu_ == Cpu::Z80) return bp.start <= a && a <= bp.end;
    return bp.start <= a + 1 && a <= bp.end;
}

// --------------------------------------------------------------------------
// listing
// --------------------------------------------------------------------------
void ListingPane::followPc()
{
    if (!backend_ || !running_) return;

    // Stepping through code keeps the listing still and moves the mark; it
    // jumps only when PC leaves the lines decoded around the current centre,
    // or lands between them (a stream decoded from another start).
    const auto at = std::find_if(lines_.begin(), lines_.end(),
                                 [](const disasm::ListingLine& l) { return l.isPc; });
    const int row = int(at - lines_.begin());
    if (at == lines_.end() || row < 3 || row + 3 >= int(lines_.size()) || !listing_->isVisible()) {
        centreOn(pc(), kCentreRow);
        return;
    }
    const int top = listing_->verticalScrollBar()->value();
    const int visible = visibleRows();
    if (row < top || row >= top + visible)
        listing_->verticalScrollBar()->setValue(row - visible / 2);
}

void ListingPane::centreOn(uint32_t addr, int rowHint)
{
    centre_ = align(addr);
    hasCentre_ = true;
    rowHint_ = rowHint;
    rebuild(true);
}

void ListingPane::rebuild(bool force)
{
    if (!backend_ || !running_) return;
    if (!force && (!isVisible() || (listClock_.isValid() && listClock_.elapsed() < kRunningListMs))) return;
    listClock_.restart();

    if (!hasCentre_) {
        centre_ = align(pc());
        hasCentre_ = true;
        rowHint_ = kCentreRow;
    }
    std::vector<disasm::ListingLine> lines = cpu_ == Cpu::Z80
        ? disasm::listAroundZ80(*backend_, uint16_t(centre_), kBefore, kAfter)
        : disasm::listAround(*backend_, centre_, kBefore, kAfter);

    // The listing marks enabled breakpoints only; a disabled one still
    // deserves a hint on its line.
    std::vector<uint32_t> disabled;
    const std::vector<Breakpoint> bps = backend_->getBreakpoints();
    for (const auto& l : lines) {
        if (l.hasBreakpoint) continue;
        for (const auto& bp : bps)
            if (isExec(bp) && !bp.enabled && stopsAt(bp, l.addr)) {
                disabled.push_back(l.addr);
                break;
            }
    }

    const bool same = lines.size() == lines_.size() && disabled == disabledAt_
                   && std::equal(lines.begin(), lines.end(), lines_.begin(), sameLine);
    if (!same) {
        lines_ = std::move(lines);
        disabledAt_ = std::move(disabled);
        fill();
    }

    // Scroll once the table is on screen and has its height; until then a
    // hidden dock or tab keeps the request.
    if (rowHint_ == kKeepScroll || !listing_->isVisible()) return;
    const auto at = std::find_if(lines_.begin(), lines_.end(),
                                 [this](const disasm::ListingLine& l) { return l.addr >= centre_; });
    const int row = int(at - lines_.begin());
    const int visible = visibleRows();
    const int offset = rowHint_ == kCentreRow ? visible / 2 : std::min(rowHint_, visible - 1);
    listing_->verticalScrollBar()->setValue(row - offset);
    rowHint_ = kKeepScroll;
}

void ListingPane::fill()
{
    const QSignalBlocker block(listing_);
    const int cur = listing_->currentRow();
    const uint32_t curAddr = (cur >= 0 && cur < listing_->rowCount())
                           ? listing_->item(cur, ColAddr)->data(Qt::UserRole).toUInt() : 0xFFFFFFFFu;

    const QPalette& pal = listing_->palette();
    const QColor pcBack = mix(pal.color(QPalette::Base), pal.color(QPalette::Highlight), 0.35f);
    const QColor dim    = pal.color(QPalette::PlaceholderText);
    const int digits = cpu_ == Cpu::Z80 ? 4 : 6;

    listing_->setRowCount(int(lines_.size()));
    int select = -1;
    for (int i = 0; i < int(lines_.size()); ++i) {
        const auto& l = lines_[i];
        const bool disabledBp = std::binary_search(disabledAt_.begin(), disabledAt_.end(), l.addr);
        QString mark = l.hasBreakpoint ? QStringLiteral("●") : disabledBp ? QStringLiteral("○") : QString();
        if (l.isPc) mark += QStringLiteral("▶");

        const QString cells[ListCols] = {
            mark, hex(l.addr, digits), QString::fromStdString(l.hex), QString::fromStdString(l.text),
        };
        for (int c = 0; c < ListCols; ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (l.isPc) item->setBackground(pcBack);
            if (!l.readable) item->setForeground(dim);
            listing_->setItem(i, c, item);
        }
        listing_->item(i, ColAddr)->setData(Qt::UserRole, l.addr);
        if (l.addr == curAddr) select = i;
    }
    if (select >= 0) listing_->setCurrentCell(select, ColAddr);
    // The view sizes its scroll range lazily; a scroll straight after this
    // would be clamped to the old row count.
    listing_->doItemsLayout();
}

int ListingPane::visibleRows() const
{
    return std::max(1, listing_->viewport()->height() / listing_->verticalHeader()->defaultSectionSize());
}

void ListingPane::scrollBy(int rows)
{
    if (lines_.empty() || rows == 0) return;
    const int visible = visibleRows();
    // Re-centre on the edge line and keep it where it sits on screen, so the
    // new lines appear beyond it and nothing visibly jumps.
    if (rows < 0) centreOn(lines_.front().addr, std::min(-rows, visible - 1));
    else          centreOn(lines_.back().addr, std::max(0, visible - 1 - rows));
}

void ListingPane::toggleBreakpointAt(int row)
{
    if (!backend_ || row < 0 || row >= int(lines_.size())) return;
    const uint32_t a = lines_[row].addr;

    // Off removes whatever marks the line; on adds one for this address only.
    bool removed = false;
    for (const auto& bp : backend_->getBreakpoints())
        if (isExec(bp) && stopsAt(bp, a)) {
            backend_->removeBreakpoint(bp.id);
            removed = true;
        }
    int added = -1;
    if (!removed) {
        Breakpoint bp;
        bp.type  = BpType::PC;
        bp.cpu   = cpu_;
        bp.start = bp.end = a;
        added = backend_->addBreakpoint(bp);
    }
    rebuild(true);
    emit breakpointsChanged(added);
}

bool ListingPane::eventFilter(QObject* obj, QEvent* ev)
{
    // The PC line's colour is worked out from the table's palette when a row
    // is made. The table gets a new palette after this widget does, so it is
    // the table's change that counts.
    if (obj == listing_ && ev->type() == QEvent::PaletteChange) fill();
    if (obj == listing_->viewport() && ev->type() == QEvent::Wheel) {
        const auto* we = static_cast<QWheelEvent*>(ev);
        const QScrollBar* sb = listing_->verticalScrollBar();
        const int dy = we->angleDelta().y();
        if (dy > 0 && sb->value() == sb->minimum()) { scrollBy(-3); return true; }
        if (dy < 0 && sb->value() == sb->maximum()) { scrollBy(3);  return true; }
    }
    if (obj == listing_ && ev->type() == QEvent::KeyPress) {
        const auto* ke = static_cast<QKeyEvent*>(ev);
        const int row  = listing_->currentRow();
        const int last = listing_->rowCount() - 1;
        if (ke->key() == Qt::Key_F2) { toggleBreakpointAt(row); return true; }

        // At either end of the decoded lines, arrows and paging decode more
        // instead of stopping.
        int step = 0;
        const int page = std::max(1, visibleRows() - 1);
        if      (ke->key() == Qt::Key_Up       && row == 0)    step = -1;
        else if (ke->key() == Qt::Key_Down     && row == last) step = 1;
        else if (ke->key() == Qt::Key_PageUp   && row == 0)    step = -page;
        else if (ke->key() == Qt::Key_PageDown && row == last) step = page;
        if (step != 0 && !lines_.empty()) {
            const uint32_t from = lines_[row].addr;
            scrollBy(step);
            const auto at = std::find_if(lines_.begin(), lines_.end(),
                                         [from](const disasm::ListingLine& l) { return l.addr == from; });
            if (at != lines_.end()) {
                const int r = std::clamp(int(at - lines_.begin()) + step, 0, int(lines_.size()) - 1);
                listing_->setCurrentCell(r, ColAddr);
            }
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

} // namespace cpuview
