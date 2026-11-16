#include "CpuDebugView.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QScrollBar>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
#include <QRadioButton>
#include <QComboBox>
#include <QLabel>
#include <QTabWidget>
#include <QGroupBox>
#include <QSplitter>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QFont>
#include <QPalette>

#include <algorithm>
#include <tuple>

namespace {

enum ListCol { ColMark, ColAddr, ColHex, ColText, ListCols };
enum BpCol   { ColBpAddr, ColBpSize, ColBpType, BpCols };

constexpr uint32_t kBusMask = 0xFFFFFF;
constexpr uint32_t kSrS     = 0x2000;

// Row offsets for CpuDebugView::rowHint_ besides "this viewport row".
constexpr int kKeepScroll = -1;
constexpr int kCentreRow  = -2;

QString hex(uint32_t v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

// A colour part-way from a to b, so marks stay readable under any palette.
QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t);
}

QFont monoFont() { return QFont(QStringLiteral("Courier New"), 9); }

// Hex as typed into an address field: an optional "$" or "0x", then digits.
bool parseHexField(QString t, uint32_t& v)
{
    t = t.trimmed();
    if (t.startsWith(QLatin1Char('$'))) t.remove(0, 1);
    else if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) t.remove(0, 2);
    bool ok = false;
    v = t.toUInt(&ok, 16);
    return ok && !t.isEmpty();
}

bool isM68kExec(const Breakpoint& bp)
{
    return bp.type == BpType::PC && bp.cpu == Cpu::M68K && !bp.is_vdp;
}

// Where a breakpoint lives, for the Address and Type columns: the VDP memory
// its linear address falls in, else the processor whose bus it watches.
struct BpSpace { const char* name; uint32_t base; int digits; };

BpSpace spaceOf(const Breakpoint& bp)
{
    if (bp.is_vdp) {
        if (bp.start >= VDP_BP_VSRAM) return {"VSRAM", VDP_BP_VSRAM, 2};
        if (bp.start >= VDP_BP_CRAM)  return {"CRAM",  VDP_BP_CRAM,  2};
        return {"VRAM", VDP_BP_VRAM, 4};
    }
    if (bp.cpu == Cpu::Z80) return {"Z80", 0, 4};
    return {"68K", 0, 6};
}

QString bpTypeText(const Breakpoint& bp)
{
    const BpSpace s = spaceOf(bp);
    if (bp.type == BpType::PC)
        return bp.cpu == Cpu::Z80 ? QStringLiteral("Z80 PC") : QStringLiteral("PC");
    return QString::fromLatin1(s.name) + (bp.type == BpType::Read ? QStringLiteral(" R") : QStringLiteral(" W"));
}

bool sameBreakpoint(const Breakpoint& a, const Breakpoint& b)
{
    return a.id == b.id && a.type == b.type && a.cpu == b.cpu && a.is_vdp == b.is_vdp
        && a.enabled == b.enabled && a.start == b.start && a.end == b.end
        && a.condition == b.condition;
}

bool sameLine(const disasm::ListingLine& a, const disasm::ListingLine& b)
{
    return a.addr == b.addr && a.len == b.len && a.readable == b.readable && a.isPc == b.isPc
        && a.hasBreakpoint == b.hasBreakpoint && a.hex == b.hex && a.text == b.text;
}

} // namespace

CpuDebugView::CpuDebugView(QWidget* parent) : QWidget(parent)
{
    cpuTabs_ = new QTabWidget(this);
    cpuTabs_->addTab(buildCpuTab(), QStringLiteral("68000"));

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(cpuTabs_);
    split->addWidget(buildBreakpointPanel());
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setChildrenCollapsible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->addWidget(split);

    updatePanelForKind();
}

void CpuDebugView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    hasCentre_ = false;
    lines_.clear();
    bps_.clear();
}

// --------------------------------------------------------------------------
// layout
// --------------------------------------------------------------------------
QWidget* CpuDebugView::buildCpuTab()
{
    auto* tab = new QWidget;

    // listing, with Go to above it
    gotoEdit_ = new QLineEdit;
    gotoEdit_->setFont(monoFont());
    gotoEdit_->setAccessibleName(QStringLiteral("Go to address"));
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
    pcBtn->setAccessibleName(QStringLiteral("Go to PC"));
    connect(pcBtn, &QPushButton::clicked, this, [this] {
        if (backend_) centreOn(backend_->getM68kRegs().pc, kCentreRow);
    });

    auto* gotoRow = new QHBoxLayout;
    gotoRow->addWidget(new QLabel(QStringLiteral("Go to")));
    gotoRow->addWidget(gotoEdit_);
    gotoRow->addWidget(goBtn);
    gotoRow->addWidget(pcBtn);
    gotoRow->addStretch();

    listing_ = new QTableWidget(0, ListCols);
    listing_->setAccessibleName(QStringLiteral("Disassembly"));
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
        listing_->setColumnWidth(ColAddr, cw * 7 + 12);
        listing_->setColumnWidth(ColHex,  cw * 25 + 12);   // up to 5 words, the longest 68000 instruction
        listing_->setMinimumWidth(cw * 40);
    }
    listing_->installEventFilter(this);
    listing_->viewport()->installEventFilter(this);
    connect(listing_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { toggleBreakpointAt(row); });

    auto* listCol = new QVBoxLayout;
    listCol->setContentsMargins(0, 0, 0, 0);
    listCol->addLayout(gotoRow);
    listCol->addWidget(listing_, 1);

    // registers
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);
    auto place = [grid](int row, int col, const QString& name, QLineEdit* e) {
        grid->addWidget(new QLabel(name), row, col * 2, Qt::AlignRight);
        grid->addWidget(e, row, col * 2 + 1);
    };
    for (int i = 0; i < 8; ++i) {
        const QString d = QStringLiteral("D%1").arg(i), a = QStringLiteral("A%1").arg(i);
        place(i, 0, d, addRegField(d, 8, [i](const M68kRegs& r) { return r.d[i]; },
                                         [i](M68kRegs& r, uint32_t v) { r.d[i] = v; }));
        place(i, 1, a, addRegField(a, 8, [i](const M68kRegs& r) { return r.a[i]; },
                                         [i](M68kRegs& r, uint32_t v) { r.a[i] = v; }));
    }
    place(8, 0, QStringLiteral("PC"), addRegField(QStringLiteral("PC"), 6,
        [](const M68kRegs& r) { return r.pc; }, [](M68kRegs& r, uint32_t v) { r.pc = v; }));
    place(8, 1, QStringLiteral("SR"), addRegField(QStringLiteral("SR"), 4,
        [](const M68kRegs& r) { return r.sr; }, [](M68kRegs& r, uint32_t v) { r.sr = v & 0xFFFF; }));
    // SP is A7 under its other name: whichever of USP and ISP is active.
    auto* sp = addRegField(QStringLiteral("SP"), 8,
        [](const M68kRegs& r) { return r.a[7]; }, [](M68kRegs& r, uint32_t v) { r.a[7] = v; });
    sp->setToolTip(QStringLiteral("A7: the ISP in supervisor mode, the USP in user mode"));
    place(9, 0, QStringLiteral("SP"), sp);
    place(9, 1, QStringLiteral("USP"), addRegField(QStringLiteral("USP"), 8, [](const M68kRegs& r) { return r.usp; }));
    place(10, 1, QStringLiteral("ISP"), addRegField(QStringLiteral("ISP"), 8, [](const M68kRegs& r) { return r.isp; }));

    // SR, bit by bit
    auto addBit = [this](QHBoxLayout* row, const QString& name, int bit, const QString& tip) {
        auto* cb = new QCheckBox(name);
        cb->setAccessibleName(QStringLiteral("SR ") + name);
        cb->setToolTip(tip);
        // toggled, not clicked: the keyboard and accessibility tools toggle
        // too. showRegisters() blocks it while it mirrors the core.
        connect(cb, &QCheckBox::toggled, this, [this, bit](bool on) {
            writeRegisters([bit, on](M68kRegs& r) {
                r.sr = on ? (r.sr | (1u << bit)) : (r.sr & ~(1u << bit));
            });
        });
        row->addWidget(cb);
        srBits_.push_back({cb, bit});
    };
    auto* srTop = new QHBoxLayout;
    addBit(srTop, QStringLiteral("T"), 15, QStringLiteral("Trace (bit 15)"));
    addBit(srTop, QStringLiteral("S"), 13, QStringLiteral("Supervisor (bit 13)"));
    addBit(srTop, QStringLiteral("M"), 12, QStringLiteral("Master/interrupt state (bit 12); a 68000 has none and keeps it 0"));
    srTop->addSpacing(6);
    srTop->addWidget(new QLabel(QStringLiteral("I")));
    srMask_ = new QSpinBox;
    srMask_->setRange(0, 7);
    srMask_->setFont(monoFont());
    srMask_->setKeyboardTracking(false);
    srMask_->setAccessibleName(QStringLiteral("SR I"));
    srMask_->setToolTip(QStringLiteral("Interrupt mask (bits 10-8)"));
    connect(srMask_, &QSpinBox::valueChanged, this, [this](int v) {
        writeRegisters([v](M68kRegs& r) { r.sr = (r.sr & ~0x0700u) | (uint32_t(v) << 8); });
    });
    srTop->addWidget(srMask_);
    srTop->addStretch();
    auto* srLow = new QHBoxLayout;
    addBit(srLow, QStringLiteral("X"), 4, QStringLiteral("Extend (bit 4)"));
    addBit(srLow, QStringLiteral("N"), 3, QStringLiteral("Negative (bit 3)"));
    addBit(srLow, QStringLiteral("Z"), 2, QStringLiteral("Zero (bit 2)"));
    addBit(srLow, QStringLiteral("V"), 1, QStringLiteral("Overflow (bit 1)"));
    addBit(srLow, QStringLiteral("C"), 0, QStringLiteral("Carry (bit 0)"));
    srLow->addStretch();

    // run control; the keys in the labels belong to the host's Debug menu
    auto mkBtn = [this](const QString& text, const QString& name, std::function<void(IDebugBackend&)> fn) {
        auto* b = new QPushButton(text);
        b->setAccessibleName(name);
        connect(b, &QPushButton::clicked, this, [this, fn = std::move(fn)] { if (backend_) fn(*backend_); });
        return b;
    };
    btnStepInto_ = mkBtn(QStringLiteral("Step Into (F7)"), QStringLiteral("Step Into"),
                         [](IDebugBackend& be) { be.stepInto(Cpu::M68K); });
    btnStepOver_ = mkBtn(QStringLiteral("Step Over (F8)"), QStringLiteral("Step Over"),
                         [](IDebugBackend& be) { be.stepOver(Cpu::M68K); });
    btnStepOut_  = mkBtn(QStringLiteral("Step Out (Ctrl+F7)"), QStringLiteral("Step Out"),
                         [](IDebugBackend& be) { be.stepOut(Cpu::M68K); });
    btnRun_      = mkBtn(QStringLiteral("Run (F9)"), QStringLiteral("Run"),
                         [](IDebugBackend& be) { be.resume(); });
    btnPause_    = mkBtn(QStringLiteral("Pause (F5)"), QStringLiteral("Pause"),
                         [](IDebugBackend& be) { if (be.isRunning()) be.pause(); });
    auto* runGrid = new QGridLayout;
    runGrid->setSpacing(2);
    runGrid->addWidget(btnStepInto_, 0, 0);
    runGrid->addWidget(btnStepOver_, 0, 1);
    runGrid->addWidget(btnStepOut_,  1, 0);
    runGrid->addWidget(btnRun_,      2, 0);
    runGrid->addWidget(btnPause_,    2, 1);

    auto* regCol = new QVBoxLayout;
    regCol->setContentsMargins(0, 0, 0, 0);
    regCol->addLayout(grid);
    regCol->addLayout(srTop);
    regCol->addLayout(srLow);
    regCol->addSpacing(4);
    regCol->addLayout(runGrid);
    regCol->addStretch();

    auto* h = new QHBoxLayout(tab);
    h->setContentsMargins(2, 2, 2, 2);
    h->addLayout(listCol, 1);
    h->addLayout(regCol, 0);
    return tab;
}

QLineEdit* CpuDebugView::addRegField(const QString& name, int digits,
                                     std::function<uint32_t(const M68kRegs&)> read,
                                     std::function<void(M68kRegs&, uint32_t)> write)
{
    auto* e = new QLineEdit;
    e->setFont(monoFont());
    e->setAccessibleName(name);
    e->setAlignment(Qt::AlignRight);
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 8 + 12);
    e->setReadOnly(true);
    if (write) {
        e->setMaxLength(8);
        e->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,8}")), e));
        connect(e, &QLineEdit::editingFinished, this, [this, e] { commitRegField(e); });
    }
    regFields_.push_back({e, digits, std::move(read), std::move(write)});
    return e;
}

QWidget* CpuDebugView::buildBreakpointPanel()
{
    auto* box = new QGroupBox(QStringLiteral("Breakpoints"));

    static const char* const kKinds[] = {"PC", "68K RAM", "Z80 RAM", "VRAM", "CRAM", "VSRAM"};
    auto* kinds = new QGridLayout;
    kinds->setSpacing(2);
    for (int i = 0; i < 6; ++i) {
        kindRadio_[i] = new QRadioButton(QString::fromLatin1(kKinds[i]));
        kinds->addWidget(kindRadio_[i], i % 3, i / 3);
    }
    kindRadio_[0]->setChecked(true);
    for (auto* r : kindRadio_)      // after the default: the handler reads widgets made below
        connect(r, &QRadioButton::toggled, this, [this](bool on) { if (on) updatePanelForKind(); });

    bpAddr_ = new QLineEdit;
    bpAddr_->setFont(monoFont());
    bpAddr_->setAccessibleName(QStringLiteral("Breakpoint address"));
    bpAddr_->setMaxLength(10);
    bpAddr_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(\\$|0[xX])?[0-9A-Fa-f]{1,8}")), bpAddr_));
    connect(bpAddr_, &QLineEdit::returnPressed, this, &CpuDebugView::addBreakpointFromPanel);

    bpSize_ = new QComboBox;
    bpSize_->setAccessibleName(QStringLiteral("Breakpoint size"));
    for (int n : {1, 2, 4}) bpSize_->addItem(QString::number(n), n);

    bpRead_  = new QCheckBox(QStringLiteral("Read"));
    bpWrite_ = new QCheckBox(QStringLiteral("Write"));
    bpWrite_->setChecked(true);

    auto* addrRow = new QHBoxLayout;
    addrRow->addWidget(new QLabel(QStringLiteral("Address")));
    addrRow->addWidget(bpAddr_, 1);
    addrRow->addWidget(new QLabel(QStringLiteral("Size")));
    addrRow->addWidget(bpSize_);
    auto* rwRow = new QHBoxLayout;
    rwRow->addWidget(bpRead_);
    rwRow->addWidget(bpWrite_);
    rwRow->addStretch();

    auto* addBtn = new QPushButton(QStringLiteral("Add"));
    auto* delBtn = new QPushButton(QStringLiteral("Delete"));
    auto* clrBtn = new QPushButton(QStringLiteral("Clear"));
    addBtn->setAccessibleName(QStringLiteral("Add breakpoint"));
    delBtn->setAccessibleName(QStringLiteral("Delete breakpoint"));
    clrBtn->setAccessibleName(QStringLiteral("Clear breakpoints"));
    clrBtn->setToolTip(QStringLiteral("Remove every breakpoint, including those another client set"));
    connect(addBtn, &QPushButton::clicked, this, &CpuDebugView::addBreakpointFromPanel);
    connect(delBtn, &QPushButton::clicked, this, &CpuDebugView::deleteSelectedBreakpoints);
    connect(clrBtn, &QPushButton::clicked, this, [this] {
        if (!backend_) return;
        backend_->clearBreakpoints();
        refreshBreakpoints();
        rebuildListing(true);
    });
    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addWidget(clrBtn);

    bpTable_ = new QTableWidget(0, BpCols);
    bpTable_->setAccessibleName(QStringLiteral("Breakpoint list"));
    bpTable_->setFont(monoFont());
    bpTable_->setHorizontalHeaderLabels({QStringLiteral("Address"), QStringLiteral("Size"), QStringLiteral("Type")});
    bpTable_->horizontalHeader()->setStretchLastSection(true);
    bpTable_->verticalHeader()->setVisible(false);
    bpTable_->verticalHeader()->setDefaultSectionSize(bpTable_->fontMetrics().height() + 4);
    bpTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bpTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    bpTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bpTable_->setShowGrid(false);
    {
        const int cw = bpTable_->fontMetrics().horizontalAdvance(QLatin1Char('0'));
        bpTable_->setColumnWidth(ColBpAddr, cw * 8 + 30);   // room for the check box
        bpTable_->setColumnWidth(ColBpSize, cw * 5 + 12);
    }
    connect(bpTable_, &QTableWidget::itemChanged, this, &CpuDebugView::onBreakpointItemChanged);

    bpMsg_ = new QLabel;
    bpMsg_->setWordWrap(true);

    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    v->addLayout(kinds);
    v->addLayout(addrRow);
    v->addLayout(rwRow);
    v->addLayout(btnRow);
    v->addWidget(bpTable_, 1);
    v->addWidget(bpMsg_);
    return box;
}

// --------------------------------------------------------------------------
// refresh
// --------------------------------------------------------------------------
void CpuDebugView::refresh()
{
    if (!backend_) return;
    const bool running = backend_->isRunning();
    const bool paused  = running && backend_->isPaused();

    btnStepInto_->setEnabled(paused);
    btnStepOver_->setEnabled(paused);
    btnStepOut_->setEnabled(paused);
    btnRun_->setEnabled(paused);
    btnPause_->setEnabled(running && !paused);
    if (!running) return;

    showRegisters(backend_->getM68kRegs(), paused);
    refreshBreakpoints();
    rebuildListing(paused);
}

void CpuDebugView::followPc()
{
    if (!backend_) return;
    refresh();
    if (!backend_->isRunning()) return;

    // Stepping through code keeps the listing still and moves the mark; it
    // jumps only when PC leaves the lines decoded around the current centre,
    // or lands between them (a stream decoded from another start).
    const auto at = std::find_if(lines_.begin(), lines_.end(),
                                 [](const disasm::ListingLine& l) { return l.isPc; });
    const int row = int(at - lines_.begin());
    if (at == lines_.end() || row < 3 || row + 3 >= int(lines_.size()) || !listing_->isVisible()) {
        centreOn(backend_->getM68kRegs().pc, kCentreRow);
        return;
    }
    const int top = listing_->verticalScrollBar()->value();
    const int visible = visibleRows();
    if (row < top || row >= top + visible)
        listing_->verticalScrollBar()->setValue(row - visible / 2);
}

// --------------------------------------------------------------------------
// listing
// --------------------------------------------------------------------------
void CpuDebugView::centreOn(uint32_t addr, int rowHint)
{
    centre_ = addr & kBusMask & ~1u;
    hasCentre_ = true;
    rowHint_ = rowHint;
    rebuildListing(true);
}

void CpuDebugView::rebuildListing(bool force)
{
    if (!backend_ || !backend_->isRunning()) return;
    if (!force && listClock_.isValid() && listClock_.elapsed() < kRunningListMs) return;
    listClock_.restart();

    if (!hasCentre_) {
        centre_ = backend_->getM68kRegs().pc & kBusMask & ~1u;
        hasCentre_ = true;
        rowHint_ = kCentreRow;
    }
    std::vector<disasm::ListingLine> lines = disasm::listAround(*backend_, centre_, kBefore, kAfter);

    // The listing marks enabled breakpoints only; a disabled one still
    // deserves a hint on its line.
    std::vector<uint32_t> disabled;
    const std::vector<Breakpoint> bps = backend_->getBreakpoints();
    for (const auto& l : lines) {
        if (l.hasBreakpoint) continue;
        for (const auto& bp : bps)
            if (isM68kExec(bp) && !bp.enabled && bp.start <= l.addr && l.addr <= bp.end) {
                disabled.push_back(l.addr);
                break;
            }
    }

    const bool same = lines.size() == lines_.size() && disabled == disabledAt_
                   && std::equal(lines.begin(), lines.end(), lines_.begin(), sameLine);
    if (!same) {
        lines_ = std::move(lines);
        disabledAt_ = std::move(disabled);
        fillListing();
    }

    // Scroll once the table is on screen and has its height; until then a
    // hidden dock keeps the request.
    if (rowHint_ == kKeepScroll || !listing_->isVisible()) return;
    const auto at = std::find_if(lines_.begin(), lines_.end(),
                                 [this](const disasm::ListingLine& l) { return l.addr >= centre_; });
    const int row = int(at - lines_.begin());
    const int visible = visibleRows();
    const int offset = rowHint_ == kCentreRow ? visible / 2 : std::min(rowHint_, visible - 1);
    listing_->verticalScrollBar()->setValue(row - offset);
    rowHint_ = kKeepScroll;
}

void CpuDebugView::fillListing()
{
    const QSignalBlocker block(listing_);
    const int cur = listing_->currentRow();
    const uint32_t curAddr = (cur >= 0 && cur < listing_->rowCount())
                           ? listing_->item(cur, ColAddr)->data(Qt::UserRole).toUInt() : 0xFFFFFFFFu;

    const QPalette& pal = listing_->palette();
    const QColor pcBack = mix(pal.color(QPalette::Base), pal.color(QPalette::Highlight), 0.35f);
    const QColor dim    = pal.color(QPalette::PlaceholderText);

    listing_->setRowCount(int(lines_.size()));
    int select = -1;
    for (int i = 0; i < int(lines_.size()); ++i) {
        const auto& l = lines_[i];
        const bool disabledBp = std::binary_search(disabledAt_.begin(), disabledAt_.end(), l.addr);
        QString mark = l.hasBreakpoint ? QStringLiteral("●") : disabledBp ? QStringLiteral("○") : QString();
        if (l.isPc) mark += QStringLiteral("▶");

        const QString cells[ListCols] = {
            mark, hex(l.addr, 6), QString::fromStdString(l.hex), QString::fromStdString(l.text),
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

int CpuDebugView::visibleRows() const
{
    return std::max(1, listing_->viewport()->height() / listing_->verticalHeader()->defaultSectionSize());
}

void CpuDebugView::scrollListingBy(int rows)
{
    if (lines_.empty() || rows == 0) return;
    const int visible = visibleRows();
    // Re-centre on the edge line and keep it where it sits on screen, so the
    // new lines appear beyond it and nothing visibly jumps.
    if (rows < 0) centreOn(lines_.front().addr, std::min(-rows, visible - 1));
    else          centreOn(lines_.back().addr, std::max(0, visible - 1 - rows));
}

void CpuDebugView::toggleBreakpointAt(int row)
{
    if (!backend_ || row < 0 || row >= int(lines_.size())) return;
    const uint32_t a = lines_[row].addr;

    // Off removes whatever marks the line; on adds one for this address only.
    bool removed = false;
    for (const auto& bp : backend_->getBreakpoints())
        if (isM68kExec(bp) && bp.start <= a && a <= bp.end) {
            backend_->removeBreakpoint(bp.id);
            removed = true;
        }
    if (!removed) {
        Breakpoint bp;
        bp.type  = BpType::PC;
        bp.cpu   = Cpu::M68K;
        bp.start = bp.end = a;
        selectBpId_ = backend_->addBreakpoint(bp);
    }
    refreshBreakpoints();
    rebuildListing(true);
}

bool CpuDebugView::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == listing_->viewport() && ev->type() == QEvent::Wheel) {
        const auto* we = static_cast<QWheelEvent*>(ev);
        const QScrollBar* sb = listing_->verticalScrollBar();
        const int dy = we->angleDelta().y();
        if (dy > 0 && sb->value() == sb->minimum()) { scrollListingBy(-3); return true; }
        if (dy < 0 && sb->value() == sb->maximum()) { scrollListingBy(3);  return true; }
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
            scrollListingBy(step);
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

void CpuDebugView::changeEvent(QEvent* ev)
{
    // The PC line's colour is worked out from the palette when a row is made.
    if (ev->type() == QEvent::PaletteChange && listing_) fillListing();
    QWidget::changeEvent(ev);
}

// --------------------------------------------------------------------------
// registers
// --------------------------------------------------------------------------
void CpuDebugView::showRegisters(const M68kRegs& r, bool paused)
{
    for (const auto& f : regFields_) {
        f.edit->setReadOnly(!paused || !f.write);
        if (f.edit->hasFocus()) continue;          // the user may be typing into it
        const QString t = hex(f.read(r), f.digits);
        if (f.edit->text() != t) f.edit->setText(t);
    }
    for (const auto& b : srBits_) {
        const QSignalBlocker block(b.box);
        b.box->setEnabled(paused);
        b.box->setChecked((r.sr >> b.bit) & 1);
    }
    srMask_->setEnabled(paused);
    if (!srMask_->hasFocus()) {
        const QSignalBlocker block(srMask_);
        srMask_->setValue(int((r.sr >> 8) & 7));
    }
}

void CpuDebugView::commitRegField(QLineEdit* edit)
{
    if (!backend_ || !edit->isModified() || edit->isReadOnly()) return;
    edit->setModified(false);
    const auto f = std::find_if(regFields_.begin(), regFields_.end(),
                                [edit](const RegField& rf) { return rf.edit == edit; });
    if (f == regFields_.end() || !f->write) return;
    bool ok = false;
    const uint32_t v = edit->text().toUInt(&ok, 16);
    if (ok) {
        const auto write = f->write;
        writeRegisters([&write, v](M68kRegs& r) { write(r, v); });
    }
    // Still focused, so refresh() leaves it alone: show what the core holds.
    edit->setText(hex(f->read(backend_->getM68kRegs()), f->digits));
}

void CpuDebugView::writeRegisters(const std::function<void(M68kRegs&)>& change)
{
    // Read, change and write in one go on the parked core, the way the
    // bridge's wreg68k does, so a half-applied set never runs.
    if (backend_ && backend_->isRunning() && backend_->isPaused()) {
        backend_->runSafely([&] {
            M68kRegs r = backend_->getM68kRegs();
            const bool super = (r.sr & kSrS) != 0;
            change(r);
            // setM68kRegs writes USP and ISP after A7 and SR, and the active
            // one of the pair is A7 itself: give it A7's value, or it undoes an
            // edit of A7. The mode A7 was read in decides which one that is.
            if (super) r.isp = r.a[7];
            else       r.usp = r.a[7];
            backend_->setM68kRegs(r);
        });
    }
    followPc();
}

// --------------------------------------------------------------------------
// breakpoints
// --------------------------------------------------------------------------
void CpuDebugView::refreshBreakpoints()
{
    if (!backend_) return;
    std::vector<Breakpoint> bps = backend_->getBreakpoints();
    std::sort(bps.begin(), bps.end(), [](const Breakpoint& a, const Breakpoint& b) {
        return std::make_tuple(a.is_vdp, a.cpu, a.start, a.type, a.id)
             < std::make_tuple(b.is_vdp, b.cpu, b.start, b.type, b.id);
    });
    const bool same = bps.size() == bps_.size()
                   && std::equal(bps.begin(), bps.end(), bps_.begin(), sameBreakpoint);
    if (same && selectBpId_ < 0) return;

    std::vector<int> selected;
    for (const auto& idx : bpTable_->selectionModel()->selectedRows())
        if (idx.row() < int(bps_.size())) selected.push_back(bps_[idx.row()].id);
    if (selectBpId_ >= 0) selected = {selectBpId_};
    selectBpId_ = -1;

    bps_ = std::move(bps);
    const QSignalBlocker block(bpTable_);
    bpTable_->clearSelection();
    bpTable_->setRowCount(int(bps_.size()));
    for (int i = 0; i < int(bps_.size()); ++i) {
        const Breakpoint& bp = bps_[i];
        const BpSpace s = spaceOf(bp);
        const uint32_t size = bp.end >= bp.start ? bp.end - bp.start + 1 : 1;

        auto* addr = new QTableWidgetItem(hex(bp.start - s.base, s.digits));
        addr->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        addr->setCheckState(bp.enabled ? Qt::Checked : Qt::Unchecked);
        auto* sz   = new QTableWidgetItem(QString::number(size));
        auto* type = new QTableWidgetItem(bpTypeText(bp));
        if (!bp.condition.empty()) {
            const QString tip = QStringLiteral("if ") + QString::fromStdString(bp.condition);
            addr->setToolTip(tip);
            type->setToolTip(tip);
            type->setText(type->text() + QStringLiteral(" ?"));
        }
        bpTable_->setItem(i, ColBpAddr, addr);
        bpTable_->setItem(i, ColBpSize, sz);
        bpTable_->setItem(i, ColBpType, type);
        if (std::find(selected.begin(), selected.end(), bp.id) != selected.end())
            bpTable_->selectionModel()->select(bpTable_->model()->index(i, 0),
                                               QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
}

void CpuDebugView::onBreakpointItemChanged(QTableWidgetItem* item)
{
    if (!backend_ || item->column() != ColBpAddr) return;
    const int row = item->row();
    if (row < 0 || row >= int(bps_.size())) return;
    const bool on = item->checkState() == Qt::Checked;
    if (on == bps_[row].enabled) return;

    // The interface has no toggle: put back a copy that differs only in
    // `enabled`. It comes back under a new id.
    Breakpoint copy = bps_[row];
    copy.enabled = on;
    backend_->removeBreakpoint(copy.id);
    selectBpId_ = backend_->addBreakpoint(copy);
    // Not from inside this signal: the rebuild deletes the item emitting it.
    QMetaObject::invokeMethod(this, [this] {
        refreshBreakpoints();
        rebuildListing(true);
    }, Qt::QueuedConnection);
}

CpuDebugView::BpKind CpuDebugView::panelKind() const
{
    for (int i = 0; i < 6; ++i)
        if (kindRadio_[i]->isChecked()) return BpKind(i);
    return BpKind::Pc;
}

void CpuDebugView::updatePanelForKind()
{
    const BpKind k = panelKind();
    bpRead_->setEnabled(k != BpKind::Pc);
    bpWrite_->setEnabled(k != BpKind::Pc);
    static const char* const kHints[] = {
        "$000000-$FFFFFF",
        "$FF0000-$FFFFFF, or an offset into it",
        "Z80 address $0000-$1FFF",
        "$0000-$FFFF",
        "$00-$7F",
        "$00-$7F",
    };
    bpAddr_->setPlaceholderText(QString::fromLatin1(kHints[int(k)]));
}

void CpuDebugView::panelMessage(const QString& text)
{
    bpMsg_->setText(text);
}

void CpuDebugView::addBreakpointFromPanel()
{
    if (!backend_) return;
    uint32_t a = 0;
    if (!parseHexField(bpAddr_->text(), a)) { panelMessage(QStringLiteral("Type a hex address.")); return; }
    const uint32_t size = bpSize_->currentData().toUInt();
    const BpKind k = panelKind();

    Breakpoint bp;
    uint32_t base = 0, lo = 0, hi = 0;          // the space, and the addresses typed into it
    switch (k) {
    case BpKind::Pc:     hi = 0x1000000; break;
    case BpKind::Ram68k: if (a < 0x10000) a += 0xFF0000; lo = 0xFF0000; hi = 0x1000000; break;
    case BpKind::RamZ80: bp.cpu = Cpu::Z80; hi = 0x2000; break;
    case BpKind::Vram:   bp.is_vdp = true; base = VDP_BP_VRAM;  hi = 0x10000; break;
    case BpKind::Cram:   bp.is_vdp = true; base = VDP_BP_CRAM;  hi = 0x80;    break;
    case BpKind::Vsram:  bp.is_vdp = true; base = VDP_BP_VSRAM; hi = 0x80;    break;
    }
    if (a < lo || uint64_t(a) + size > hi) {
        panelMessage(QStringLiteral("Out of range: ") + bpAddr_->placeholderText() + QLatin1Char('.'));
        return;
    }
    bp.start = base + a;
    bp.end   = bp.start + size - 1;

    std::vector<BpType> types;
    if (k == BpKind::Pc) types.push_back(BpType::PC);
    else {
        if (bpRead_->isChecked())  types.push_back(BpType::Read);
        if (bpWrite_->isChecked()) types.push_back(BpType::Write);
    }
    if (types.empty()) { panelMessage(QStringLiteral("Tick Read, Write or both.")); return; }

    for (BpType t : types) {
        bp.type = t;
        selectBpId_ = backend_->addBreakpoint(bp);
    }
    panelMessage(QString());
    refreshBreakpoints();
    rebuildListing(true);
}

void CpuDebugView::deleteSelectedBreakpoints()
{
    if (!backend_) return;
    for (const auto& idx : bpTable_->selectionModel()->selectedRows())
        if (idx.row() < int(bps_.size())) backend_->removeBreakpoint(bps_[idx.row()].id);
    refreshBreakpoints();
    rebuildListing(true);
}
