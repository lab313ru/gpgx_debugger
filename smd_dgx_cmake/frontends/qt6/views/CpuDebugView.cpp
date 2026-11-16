#include "CpuDebugView.h"
#include "CpuViewUtil.h"
#include "ListingPane.h"

#include <QTableWidget>
#include <QHeaderView>
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
#include <QStringList>

#include <algorithm>
#include <iterator>
#include <tuple>

using cpuview::hex;
using cpuview::monoFont;
using cpuview::parseHexField;

namespace {

enum BpCol { ColBpAddr, ColBpSize, ColBpType, BpCols };

constexpr uint32_t kSrS = 0x2000;

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
    const char* what = bp.type == BpType::PC ? " PC" : bp.type == BpType::Read ? " R" : " W";
    return QString::fromLatin1(s.name) + QString::fromLatin1(what);
}

bool sameBreakpoint(const Breakpoint& a, const Breakpoint& b)
{
    return a.id == b.id && a.type == b.type && a.cpu == b.cpu && a.is_vdp == b.is_vdp
        && a.enabled == b.enabled && a.start == b.start && a.end == b.end
        && a.condition == b.condition;
}

// The Z80's bus state in words, from the core's zstate: d0 set = /RESET
// released, d1 = the 68000 holds BUSREQ, d2 = WAIT. It runs only at 1. d0
// clear is either the 68000's reset or a lock-up: the core clears zstate
// itself when the Z80 reaches through its bank window into $800000-$DFFFFF
// outside the VDP and I/O (membnk.c), and nothing in zstate tells them apart.
// Short for the field, full for its tooltip.
QString z80BusText(uint8_t z, bool full)
{
    if (z == kZ80StateUnknown) return QStringLiteral("unknown");
    if (z == 1) return QStringLiteral("running");
    QStringList parts;
    if (!(z & 1)) parts << (full ? QStringLiteral("in reset by the 68000 ($A11200), or locked up by a "
                                                  "bank access to $800000-$DFFFFF")
                                 : QStringLiteral("reset or lock-up"));
    if (z & 2)    parts << (full ? QStringLiteral("bus taken by the 68000 ($A11100)")
                                 : QStringLiteral("bus taken"));
    if (z & 4)    parts << (full ? QStringLiteral("waiting for the 68000 bus")
                                 : QStringLiteral("waiting"));
    return parts.join(QStringLiteral(", "));
}

// A register field shows the core's value unless it holds typing not yet
// committed; a field that merely has focus, say to copy from, still follows.
void showField(QLineEdit* e, uint32_t v, int digits)
{
    if (e->hasFocus() && !e->isReadOnly() && e->isModified()) return;
    const QString t = hex(v, digits);
    if (e->text() != t) e->setText(t);
}

// The spin box takes what was typed only on Enter or focus out; until then
// its text differs from its value, and a refresh must not undo it.
void showSpin(QSpinBox* s, int v)
{
    if (s->hasFocus() && s->cleanText() != QString::number(s->value())) return;
    const QSignalBlocker block(s);
    s->setValue(v);
}

QLineEdit* makeRegEdit(int widthDigits)
{
    auto* e = new QLineEdit;
    e->setFont(monoFont());
    e->setAlignment(Qt::AlignRight);
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QLatin1Char('0')) * widthDigits + 12);
    e->setReadOnly(true);
    return e;
}

void makeHexInput(QLineEdit* e, int digits)
{
    e->setMaxLength(digits);
    e->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,%1}").arg(digits)), e));
}

} // namespace

CpuDebugView::CpuDebugView(QWidget* parent) : QWidget(parent)
{
    cpuTabs_ = new QTabWidget(this);
    cpuTabs_->addTab(buildCpuTab(), QStringLiteral("68000"));
    cpuTabs_->addTab(buildZ80Tab(), QStringLiteral("Z80"));

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(cpuTabs_);
    split->addWidget(buildBreakpointPanel());
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setChildrenCollapsible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->addWidget(split);

    for (auto* pane : {m68kList_, z80List_})
        connect(pane, &cpuview::ListingPane::breakpointsChanged, this, [this](int added) {
            if (added >= 0) selectBpId_ = added;
            refreshBreakpoints();
            rebuildListings(true);
        });
    // A PC breakpoint added from the panel is for the CPU in front, unless
    // the CPU box is changed after the tab.
    connect(cpuTabs_, &QTabWidget::currentChanged, this, [this] {
        const Cpu cpu = currentCpu();
        bpCpu_->setCurrentIndex(bpCpu_->findData(int(cpu)));
        (cpu == Cpu::Z80 ? z80List_ : m68kList_)->rebuild(true);
        emit currentCpuChanged(cpu);
    });

    updatePanelForKind();
    setControls(false, false);
}

void CpuDebugView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    m68kList_->setBackend(b);
    z80List_->setBackend(b);
    bps_.clear();
}

void CpuDebugView::setRunning(bool on)
{
    m68kList_->setRunning(on);
    z80List_->setRunning(on);
    running_ = on;
    refresh();
}

Cpu CpuDebugView::currentCpu() const
{
    return cpuTabs_->currentIndex() == 1 ? Cpu::Z80 : Cpu::M68K;
}

void CpuDebugView::showCpu(Cpu cpu)
{
    cpuTabs_->setCurrentIndex(cpu == Cpu::Z80 ? 1 : 0);
}

// --------------------------------------------------------------------------
// layout
// --------------------------------------------------------------------------
QLayout* CpuDebugView::buildRunButtons(Cpu cpu, QPushButton* (&btns)[BtnCount])
{
    // The keys in the labels belong to the host's Debug menu, whose steps act
    // on the tab in front, so the labels hold on either tab.
    const bool z80 = cpu == Cpu::Z80;
    const QString who = z80 ? QStringLiteral("Z80 ") : QString();
    auto mk = [this, &who](const QString& text, const QString& name, std::function<void(IDebugBackend&)> fn) {
        auto* b = new QPushButton(text);
        b->setAccessibleName(who + name);
        connect(b, &QPushButton::clicked, this, [this, fn = std::move(fn)] { if (backend_ && running_) fn(*backend_); });
        return b;
    };
    btns[BtnStepInto] = mk(QStringLiteral("Step Into (F7)"), QStringLiteral("Step Into"),
                           [cpu](IDebugBackend& be) { be.stepInto(cpu); });
    btns[BtnStepOver] = mk(QStringLiteral("Step Over (F8)"), QStringLiteral("Step Over"),
                           [cpu](IDebugBackend& be) { be.stepOver(cpu); });
    btns[BtnStepOut]  = mk(QStringLiteral("Step Out (Ctrl+F7)"), QStringLiteral("Step Out"),
                           [cpu](IDebugBackend& be) { be.stepOut(cpu); });
    btns[BtnRun]      = mk(QStringLiteral("Run (F9)"), QStringLiteral("Run"),
                           [](IDebugBackend& be) { be.resume(); });
    btns[BtnPause]    = mk(QStringLiteral("Pause (F5)"), QStringLiteral("Pause"),
                           [](IDebugBackend& be) { be.pause(); });
    if (z80) {
        const QString tip = QStringLiteral(
            "Steps the Z80. While the 68000 holds it in reset or has its bus, "
            "the step finishes only once the 68000 lets the Z80 run.");
        for (int i : {BtnStepInto, BtnStepOver, BtnStepOut}) btns[i]->setToolTip(tip);
    }
    auto* runGrid = new QGridLayout;
    runGrid->setSpacing(2);
    runGrid->addWidget(btns[BtnStepInto], 0, 0);
    runGrid->addWidget(btns[BtnStepOver], 0, 1);
    runGrid->addWidget(btns[BtnStepOut],  1, 0);
    runGrid->addWidget(btns[BtnRun],      2, 0);
    runGrid->addWidget(btns[BtnPause],    2, 1);
    return runGrid;
}

QWidget* CpuDebugView::buildCpuTab()
{
    auto* tab = new QWidget;
    m68kList_ = new cpuview::ListingPane(Cpu::M68K);

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

    auto* regCol = new QVBoxLayout;
    regCol->setContentsMargins(0, 0, 0, 0);
    regCol->addLayout(grid);
    regCol->addLayout(srTop);
    regCol->addLayout(srLow);
    regCol->addSpacing(4);
    regCol->addLayout(buildRunButtons(Cpu::M68K, m68kBtns_));
    regCol->addStretch();

    auto* h = new QHBoxLayout(tab);
    h->setContentsMargins(2, 2, 2, 2);
    h->addWidget(m68kList_, 1);
    h->addLayout(regCol, 0);
    return tab;
}

QWidget* CpuDebugView::buildZ80Tab()
{
    auto* tab = new QWidget;
    z80List_ = new cpuview::ListingPane(Cpu::Z80);

    // registers: the main set beside the alternate one, then the rest
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);
    auto place = [grid](int row, int col, const QString& name, QWidget* w) {
        grid->addWidget(new QLabel(name), row, col * 2, Qt::AlignRight);
        grid->addWidget(w, row, col * 2 + 1, Qt::AlignLeft);
    };
    using Get = uint16_t Z80Regs::*;
    struct Pair { const char* name; Get reg; };
    const Pair pairs[] = {
        {"AF", &Z80Regs::af}, {"AF'", &Z80Regs::af2},
        {"BC", &Z80Regs::bc}, {"BC'", &Z80Regs::bc2},
        {"DE", &Z80Regs::de}, {"DE'", &Z80Regs::de2},
        {"HL", &Z80Regs::hl}, {"HL'", &Z80Regs::hl2},
        {"IX", &Z80Regs::ix}, {"IY",  &Z80Regs::iy},
        {"SP", &Z80Regs::sp}, {"PC",  &Z80Regs::pc},
    };
    for (int i = 0; i < int(std::size(pairs)); ++i) {
        const Get reg = pairs[i].reg;
        const QString name = QString::fromLatin1(pairs[i].name);
        place(i / 2, i % 2, name, addZ80Field(name, 4,
            [reg](const Z80Regs& r) { return uint32_t(r.*reg); },
            [reg](Z80Regs& r, uint32_t v) { r.*reg = uint16_t(v); }));
    }
    place(6, 0, QStringLiteral("I"), addZ80Field(QStringLiteral("I"), 2,
        [](const Z80Regs& r) { return uint32_t(r.i); }, [](Z80Regs& r, uint32_t v) { r.i = uint8_t(v); }));
    place(6, 1, QStringLiteral("R"), addZ80Field(QStringLiteral("R"), 2,
        [](const Z80Regs& r) { return uint32_t(r.r); }, [](Z80Regs& r, uint32_t v) { r.r = uint8_t(v); }));
    z80Im_ = new QSpinBox;
    z80Im_->setRange(0, 2);
    z80Im_->setFont(monoFont());
    z80Im_->setKeyboardTracking(false);
    z80Im_->setAccessibleName(QStringLiteral("Z80 IM"));
    z80Im_->setToolTip(QStringLiteral("Interrupt mode"));
    connect(z80Im_, &QSpinBox::valueChanged, this, [this](int v) {
        writeZ80Registers([v](Z80Regs& r) { r.im = uint8_t(v); });
    });
    place(7, 0, QStringLiteral("IM"), z80Im_);
    auto* halt = addZ80Field(QStringLiteral("HALT"), 1, [](const Z80Regs& r) { return uint32_t(r.halt ? 1 : 0); });
    halt->setToolTip(QStringLiteral("1 while the Z80 waits in HALT for an interrupt"));
    place(7, 1, QStringLiteral("HALT"), halt);

    // F, bit by bit
    auto* flags = new QHBoxLayout;
    struct Flag { const char* name; int bit; const char* tip; };
    const Flag fl[] = {
        {"S", 7, "Sign (bit 7)"}, {"Z", 6, "Zero (bit 6)"}, {"Y", 5, "Copy of result bit 5 (bit 5)"},
        {"H", 4, "Half carry (bit 4)"}, {"X", 3, "Copy of result bit 3 (bit 3)"},
        {"P/V", 2, "Parity or overflow (bit 2)"}, {"N", 1, "Add/subtract (bit 1)"}, {"C", 0, "Carry (bit 0)"},
    };
    for (const Flag& f : fl) {
        const int bit = f.bit;
        const QString name = QString::fromLatin1(f.name);
        flags->addWidget(addZ80Bit(name, QStringLiteral("Z80 flag ") + name, QString::fromLatin1(f.tip),
            [bit](const Z80Regs& r) { return ((r.af >> bit) & 1) != 0; },
            [bit](Z80Regs& r, bool on) {
                r.af = uint16_t(on ? (r.af | (1u << bit)) : (r.af & ~(1u << bit)));
            }));
    }
    flags->addStretch();
    auto* iff = new QHBoxLayout;
    iff->addWidget(addZ80Bit(QStringLiteral("IFF1"), QStringLiteral("Z80 IFF1"),
        QStringLiteral("Interrupts enabled"),
        [](const Z80Regs& r) { return r.iff1 != 0; }, [](Z80Regs& r, bool on) { r.iff1 = on; }));
    iff->addWidget(addZ80Bit(QStringLiteral("IFF2"), QStringLiteral("Z80 IFF2"),
        QStringLiteral("Where IFF1 is kept across an NMI"),
        [](const Z80Regs& r) { return r.iff2 != 0; }, [](Z80Regs& r, bool on) { r.iff2 = on; }));
    iff->addStretch();

    // read-only, as USP and ISP are on the 68000 tab: the bank window and the bus
    auto mkInfo = [](const QString& name, const QString& tip) {
        auto* e = new QLineEdit;
        e->setFont(monoFont());
        e->setAccessibleName(name);
        e->setToolTip(tip);
        e->setReadOnly(true);
        return e;
    };
    z80Bank_ = mkInfo(QStringLiteral("Z80 bank"), QStringLiteral(
        "Where the window at Z80 $8000-$FFFF looks on the 68000 bus"));
    z80Bus_ = mkInfo(QStringLiteral("Z80 bus state"), QString());
    z80Held_ = new QLabel(QStringLiteral(
        "The Z80 is stopped: a Z80 step finishes only once the 68000 lets it run "
        "(or resets it, after a lock-up)."));
    z80Held_->setWordWrap(true);
    z80Held_->setVisible(false);
    auto* info = new QGridLayout;
    info->setHorizontalSpacing(4);
    info->setVerticalSpacing(2);
    info->addWidget(new QLabel(QStringLiteral("Bank")), 0, 0, Qt::AlignRight);
    info->addWidget(z80Bank_, 0, 1);
    info->addWidget(new QLabel(QStringLiteral("Bus")), 1, 0, Qt::AlignRight);
    info->addWidget(z80Bus_, 1, 1);
    info->setColumnStretch(1, 1);

    auto* regCol = new QVBoxLayout;
    regCol->setContentsMargins(0, 0, 0, 0);
    regCol->addLayout(grid);
    regCol->addLayout(flags);
    regCol->addLayout(iff);
    regCol->addLayout(info);
    regCol->addSpacing(4);
    regCol->addLayout(buildRunButtons(Cpu::Z80, z80Btns_));
    regCol->addWidget(z80Held_);
    regCol->addStretch();

    auto* regBox = new QWidget;
    regBox->setLayout(regCol);
    // The note wraps instead of widening the column.
    regBox->setMaximumWidth(regBox->sizeHint().width());

    auto* h = new QHBoxLayout(tab);
    h->setContentsMargins(2, 2, 2, 2);
    h->addWidget(z80List_, 1);
    h->addWidget(regBox, 0);
    return tab;
}

QLineEdit* CpuDebugView::addRegField(const QString& name, int digits,
                                     std::function<uint32_t(const M68kRegs&)> read,
                                     std::function<void(M68kRegs&, uint32_t)> write)
{
    auto* e = makeRegEdit(8);
    e->setAccessibleName(name);
    if (write) {
        makeHexInput(e, 8);
        connect(e, &QLineEdit::editingFinished, this, [this, e] { commitRegField(e); });
    }
    regFields_.push_back({e, digits, std::move(read), std::move(write)});
    return e;
}

QLineEdit* CpuDebugView::addZ80Field(const QString& name, int digits,
                                     std::function<uint32_t(const Z80Regs&)> read,
                                     std::function<void(Z80Regs&, uint32_t)> write)
{
    auto* e = makeRegEdit(5);
    e->setAccessibleName(QStringLiteral("Z80 ") + name);
    if (write) {
        makeHexInput(e, digits);
        connect(e, &QLineEdit::editingFinished, this, [this, e] { commitZ80Field(e); });
    }
    z80Fields_.push_back({e, digits, std::move(read), std::move(write)});
    return e;
}

QCheckBox* CpuDebugView::addZ80Bit(const QString& text, const QString& name, const QString& tip,
                                   std::function<bool(const Z80Regs&)> read,
                                   std::function<void(Z80Regs&, bool)> write)
{
    auto* cb = new QCheckBox(text);
    cb->setAccessibleName(name);
    cb->setToolTip(tip);
    // toggled, as for SR: showZ80Registers() blocks it while it mirrors the core.
    connect(cb, &QCheckBox::toggled, this, [this, write](bool on) {
        writeZ80Registers([&write, on](Z80Regs& r) { write(r, on); });
    });
    z80Bits_.push_back({cb, std::move(read), std::move(write)});
    return cb;
}

QWidget* CpuDebugView::buildBreakpointPanel()
{
    auto* box = new QGroupBox(QStringLiteral("Breakpoints"));

    static const char* const kKinds[] = {"PC", "68K RAM", "Z80 RAM", "VRAM", "CRAM", "VSRAM"};
    auto* kinds = new QGridLayout;
    kinds->setSpacing(2);
    for (int i = 0; i < 6; ++i) {
        kindRadio_[i] = new QRadioButton(QString::fromLatin1(kKinds[i]));
        kinds->addWidget(kindRadio_[i], i / 3, i % 3);
    }
    kindRadio_[int(BpKind::Pc)]->setToolTip(QStringLiteral(
        "Stops before the instruction at the address runs, on the CPU chosen beside Read and Write"));
    kindRadio_[int(BpKind::Ram68k)]->setToolTip(QStringLiteral(
        "Reads and writes anywhere on the 68000 bus: RAM, ROM, I/O, the VDP ports"));
    kindRadio_[int(BpKind::RamZ80)]->setToolTip(QStringLiteral(
        "Reads and writes anywhere on the Z80 bus: RAM, YM2612, bank, PSG, the 68000 window"));
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
    bpRead_->setChecked(true);
    bpWrite_->setChecked(true);

    bpCpu_ = new QComboBox;
    bpCpu_->setAccessibleName(QStringLiteral("Breakpoint CPU"));
    bpCpu_->setToolTip(QStringLiteral("Whose PC a PC breakpoint watches; follows the tab in front"));
    bpCpu_->addItem(QStringLiteral("68000"), int(Cpu::M68K));
    bpCpu_->addItem(QStringLiteral("Z80"),   int(Cpu::Z80));
    connect(bpCpu_, &QComboBox::currentIndexChanged, this, [this] { updatePanelForKind(); });

    auto* addrRow = new QHBoxLayout;
    addrRow->addWidget(new QLabel(QStringLiteral("Address")));
    addrRow->addWidget(bpAddr_, 1);
    addrRow->addWidget(new QLabel(QStringLiteral("Size")));
    addrRow->addWidget(bpSize_);
    auto* rwRow = new QHBoxLayout;
    rwRow->addWidget(bpRead_);
    rwRow->addWidget(bpWrite_);
    rwRow->addStretch();
    rwRow->addWidget(new QLabel(QStringLiteral("CPU")));
    rwRow->addWidget(bpCpu_);

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
        rebuildListings(true);
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
    bpTable_->installEventFilter(this);         // Delete

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
    const bool running = backend_ && running_;
    const bool paused  = running && backend_->isPaused();
    setControls(running, paused);
    if (!running) return;

    showRegisters(backend_->getM68kRegs());
    showZ80Registers(backend_->getZ80Regs());
    refreshBreakpoints();
    rebuildListings(paused);
}

void CpuDebugView::followPc()
{
    if (!backend_) return;
    refresh();
    if (!running_) return;
    m68kList_->followPc();
    z80List_->followPc();
}

void CpuDebugView::rebuildListings(bool force)
{
    m68kList_->rebuild(force);
    z80List_->rebuild(force);
}

bool CpuDebugView::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == bpTable_ && ev->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Delete) {
        deleteSelectedBreakpoints();
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

// --------------------------------------------------------------------------
// registers
// --------------------------------------------------------------------------
void CpuDebugView::setControls(bool running, bool paused)
{
    for (auto* btns : {m68kBtns_, z80Btns_}) {
        btns[BtnStepInto]->setEnabled(paused);
        btns[BtnStepOver]->setEnabled(paused);
        btns[BtnStepOut]->setEnabled(paused);
        btns[BtnRun]->setEnabled(paused);
        btns[BtnPause]->setEnabled(running && !paused);
    }
    for (const auto& f : regFields_) f.edit->setReadOnly(!paused || !f.write);
    for (const auto& b : srBits_) b.box->setEnabled(paused);
    srMask_->setEnabled(paused);
    for (const auto& f : z80Fields_) f.edit->setReadOnly(!paused || !f.write);
    for (const auto& b : z80Bits_) b.box->setEnabled(paused);
    z80Im_->setEnabled(paused);
}

void CpuDebugView::showRegisters(const M68kRegs& r)
{
    for (const auto& f : regFields_) showField(f.edit, f.read(r), f.digits);
    for (const auto& b : srBits_) {
        const QSignalBlocker block(b.box);
        b.box->setChecked((r.sr >> b.bit) & 1);
    }
    showSpin(srMask_, int((r.sr >> 8) & 7));
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
    // What the core holds, also when the text did not parse.
    edit->setText(hex(f->read(backend_->getM68kRegs()), f->digits));
}

void CpuDebugView::writeRegisters(const std::function<void(M68kRegs&)>& change)
{
    // Read, change and write in one go on the parked core, the way the
    // bridge's wreg68k does, so a half-applied set never runs.
    if (backend_ && running_ && backend_->isPaused()) {
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

void CpuDebugView::showZ80Registers(const Z80Regs& r)
{
    for (const auto& f : z80Fields_) showField(f.edit, f.read(r), f.digits);
    for (const auto& b : z80Bits_) {
        const QSignalBlocker block(b.box);
        b.box->setChecked(b.read(r));
    }
    showSpin(z80Im_, r.im);

    const uint32_t bank = r.bank & 0xFFFFFF;
    const QString bankText = QLatin1Char('$') + hex(bank, 6) + QStringLiteral("-$")
                           + hex((bank + 0x7FFF) & 0xFFFFFF, 6);
    if (z80Bank_->text() != bankText) { z80Bank_->setText(bankText); z80Bank_->setCursorPosition(0); }
    const QString bus = z80BusText(r.zstate, false);
    if (z80Bus_->text() != bus) {
        z80Bus_->setText(bus);
        z80Bus_->setCursorPosition(0);
        z80Bus_->setToolTip(QStringLiteral("What the 68000 does to the Z80 through $A11100 (BUSREQ) "
                                           "and $A11200 (RESET). Now: ") + z80BusText(r.zstate, true));
    }
    z80Held_->setVisible(r.zstate != 1 && r.zstate != kZ80StateUnknown);
}

void CpuDebugView::commitZ80Field(QLineEdit* edit)
{
    if (!backend_ || !edit->isModified() || edit->isReadOnly()) return;
    edit->setModified(false);
    const auto f = std::find_if(z80Fields_.begin(), z80Fields_.end(),
                                [edit](const Z80Field& zf) { return zf.edit == edit; });
    if (f == z80Fields_.end() || !f->write) return;
    bool ok = false;
    const uint32_t v = edit->text().toUInt(&ok, 16);
    if (ok) {
        const auto write = f->write;
        writeZ80Registers([&write, v](Z80Regs& r) { write(r, v); });
    }
    edit->setText(hex(f->read(backend_->getZ80Regs()), f->digits));
}

void CpuDebugView::writeZ80Registers(const std::function<void(Z80Regs&)>& change)
{
    // As for the 68000: read, change and write in one go on the parked core.
    // setZ80Regs leaves zstate and the bank alone.
    if (backend_ && running_ && backend_->isPaused()) {
        backend_->runSafely([&] {
            Z80Regs r = backend_->getZ80Regs();
            change(r);
            backend_->setZ80Regs(r);
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

    // In place: IDA and MCP clients know their breakpoints by id. If another
    // client has just removed it, the rebuild below drops the row.
    backend_->setBreakpointEnabled(bps_[row].id, on);
    // Not from inside this signal: the rebuild deletes the item emitting it.
    QMetaObject::invokeMethod(this, [this] {
        refreshBreakpoints();
        rebuildListings(true);
    }, Qt::QueuedConnection);
}

CpuDebugView::BpKind CpuDebugView::panelKind() const
{
    for (int i = 0; i < 6; ++i)
        if (kindRadio_[i]->isChecked()) return BpKind(i);
    return BpKind::Pc;
}

Cpu CpuDebugView::panelCpu() const
{
    return bpCpu_->currentData().toInt() == int(Cpu::Z80) ? Cpu::Z80 : Cpu::M68K;
}

void CpuDebugView::updatePanelForKind()
{
    const BpKind k = panelKind();
    bpRead_->setEnabled(k != BpKind::Pc);
    bpWrite_->setEnabled(k != BpKind::Pc);
    bpCpu_->setEnabled(k == BpKind::Pc);
    static const char* const kHints[] = {
        "$000000-$FFFFFF",
        "$000000-$FFFFFF",
        "Z80 address $0000-$FFFF",
        "$0000-$FFFF",
        "$00-$7F",
        "$00-$7F",
    };
    const bool z80Pc = k == BpKind::Pc && panelCpu() == Cpu::Z80;
    bpAddr_->setPlaceholderText(QString::fromLatin1(z80Pc ? "Z80 address $0000-$FFFF" : kHints[int(k)]));
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
    const bool z80Pc = k == BpKind::Pc && panelCpu() == Cpu::Z80;

    // Read and write breakpoints watch the whole bus, as the CPU hooks see it.
    Breakpoint bp;
    uint32_t base = 0, hi = 0;                  // the space, and where the addresses typed into it end
    switch (k) {
    case BpKind::Pc:
        if (z80Pc) { bp.cpu = Cpu::Z80; hi = 0x10000; }
        else       hi = 0x1000000;
        break;
    case BpKind::Ram68k: hi = 0x1000000; break;
    case BpKind::RamZ80: bp.cpu = Cpu::Z80; hi = 0x10000; break;
    case BpKind::Vram:   bp.is_vdp = true; base = VDP_BP_VRAM;  hi = 0x10000; break;
    case BpKind::Cram:   bp.is_vdp = true; base = VDP_BP_CRAM;  hi = 0x80;    break;
    case BpKind::Vsram:  bp.is_vdp = true; base = VDP_BP_VSRAM; hi = 0x80;    break;
    }
    if (uint64_t(a) + size > hi) {
        panelMessage(QStringLiteral("Out of range: ") + bpAddr_->placeholderText() + QLatin1Char('.'));
        return;
    }
    // Z80 instructions start at any byte.
    if (k == BpKind::Pc && !z80Pc && (a & 1)) {
        panelMessage(QStringLiteral("Instructions start at even addresses."));
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
    rebuildListings(true);
}

void CpuDebugView::deleteSelectedBreakpoints()
{
    if (!backend_) return;
    for (const auto& idx : bpTable_->selectionModel()->selectedRows())
        if (idx.row() < int(bps_.size())) backend_->removeBreakpoint(bps_[idx.row()].id);
    refreshBreakpoints();
    rebuildListings(true);
}
