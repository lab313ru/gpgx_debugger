#include "VdpRegView.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QFont>
#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t maskBits(int n, int c = 1) { return ((1u << c) - 1u) << n; }
constexpr uint32_t getBits(uint32_t x, int n, int c) { return (x & maskBits(n, c)) >> n; }

// Mode flags (Gens vdp_ram.cpp semantics)
bool mode4(const uint8_t* r) { return !(r[0x01] & maskBits(2)); }  // M5 clear => SMS mode 4
bool evram(const uint8_t* r) { return  (r[0x01] & maskBits(7)) != 0; }
bool h40(const uint8_t* r)   { return  (r[0x0C] & maskBits(0)) != 0; }

uint32_t dmaSrc(const uint8_t* r)   // byte address, incl. bit6 of reg 0x17 (Gens quirk)
{
    return (uint32_t(r[0x15]) << 1) + (uint32_t(r[0x16]) << 9) + (getBits(r[0x17], 0, 7) << 17);
}

QString hexU(uint32_t v, int w)
{
    return QStringLiteral("%1").arg(v, w, 16, QLatin1Char('0')).toUpper();
}

const int kModeRegs[4] = { 0x00, 0x01, 0x0B, 0x0C };

// Bit descriptions indexed by bit number 0..7 (displayed 7 -> 0).
const char* const kSet1Bits[8] = {
    "ES (External Sync)",
    "M2 (HV Counter Latch)",
    "PS (Palette Select)",
    "SS/HSM (See Info)",
    "IE1 (Enable HINT)",
    "LCB (Left Column Blank)",
    "HSI (Horizontal Scroll Inhibit)",
    "VSI (Vertical Scroll Inhibit)",
};
const char* const kSet2Bits[8] = {
    "MAG (Sprite Zoom)",
    "SZ (Sprite Size)",
    "M5 (Enable Mode 5)",
    "M3 (Enable V30 Mode)",
    "M1 (Enable DMA)",
    "IE0 (Enable VINT)",
    "DISP (Enable Display)",
    "EVRAM (Extended VRAM)",
};
const char* const kSet3Bits[8] = {
    "LSCR (Line Scrolling)",
    "HSCR (Horizontal Scroll Mode)",
    "VSCR (Vertical Scroll Mode)",
    "IE2 (Enable EXINT)",
    "----",
    "----",
    "Unknown",
    "Unknown",
};
const char* const kSet4Bits[8] = {
    "RS1 (H40 Mode Enable)",
    "LSM0 (Enable Interlacing)",
    "LSM1 (Interlace Double)",
    "STE (Shadow/Highlight)",
    "U3 (Enable SPA/B)",
    "U2 (Disable HSYNC)",
    "U1 (Output Pixel Clock)",
    "RS0 (EDCLK Enable)",
};
const char* const* const kModeBits[4] = { kSet1Bits, kSet2Bits, kSet3Bits, kSet4Bits };

QScrollArea* wrapScroll(QWidget* page)
{
    auto* sc = new QScrollArea;
    sc->setWidget(page);
    sc->setWidgetResizable(true);
    sc->setFrameShape(QFrame::NoFrame);
    return sc;
}

} // namespace

VdpRegView::VdpRegView(QWidget* parent) : QWidget(parent)
{
    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildModeTab(),  QStringLiteral("Mode Registers"));
    tabs_->addTab(buildOtherTab(), QStringLiteral("Other Registers"));
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(tabs_);
    tabs_->setEnabled(false);
    syncControls();
}

void VdpRegView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    first_   = true;
    if (backend_) {
        refresh();
    } else {
        tabs_->setEnabled(false);
        std::memset(regs_, 0, sizeof(regs_));
        std::memset(diff_, 0, sizeof(diff_));
        syncControls();
    }
}

void VdpRegView::refresh()
{
    if (!backend_) { tabs_->setEnabled(false); return; }
    tabs_->setEnabled(true);
    const VdpState st = backend_->getVdpState();
    std::memcpy(regs_, st.reg, sizeof(regs_));
    for (int i = 0; i < 0x18; ++i)
        diff_[i] = first_ ? 0 : uint8_t(regs_[i] ^ prev_[i]);
    std::memcpy(prev_, regs_, sizeof(prev_));
    first_ = false;
    syncControls();
}

QCheckBox* VdpRegView::addCheck(int reg, int bit, const QString& label)
{
    auto* cb = new QCheckBox(label);
    connect(cb, &QCheckBox::clicked, this, [this, reg, bit](bool on) {
        if (!backend_) return;
        setRegBits(reg, bit, 1, on ? 1u : 0u);
        refresh();
    });
    bits_.push_back({cb, reg, bit});
    return cb;
}

QLineEdit* VdpRegView::addField(int width, std::vector<int> deps,
                                std::function<uint32_t(const uint8_t*)> read,
                                std::function<void(uint32_t)> write)
{
    auto* e = new QLineEdit;
    e->setFont(QFont(QStringLiteral("Courier New"), 9));
    e->setAlignment(Qt::AlignCenter);
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 8 + 12);
    if (write) {
        e->setMaxLength(6);
        e->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,6}")), e));
        connect(e, &QLineEdit::editingFinished, this, [this, e]() { commitField(e); });
    } else {
        e->setReadOnly(true);
    }
    fields_.push_back({e, width, std::move(deps), std::move(read), std::move(write)});
    return e;
}

void VdpRegView::commitField(QLineEdit* edit)
{
    if (!backend_ || !edit->isModified()) return;
    for (auto& f : fields_) {
        if (f.edit != edit) continue;
        bool ok = false;
        const uint32_t v = edit->text().toUInt(&ok, 16);
        if (ok && f.write) f.write(v);
        edit->setModified(false);
        refresh();
        return;
    }
}

void VdpRegView::setRegBits(int reg, int lo, int count, uint32_t v)
{
    if (!backend_) return;
    uint8_t x = backend_->getVdpState().reg[reg];
    x = uint8_t((x & ~maskBits(lo, count)) | ((v & maskBits(0, count)) << lo));
    backend_->setVdpReg(reg, x);
}

void VdpRegView::writeDmaSrc(uint32_t byteAddr)
{
    // Gens semantics: writing the source clears DMD1 and overwrites DMD0 (A23).
    backend_->setVdpReg(0x15, uint8_t((byteAddr >> 1)  & 0xFF));
    backend_->setVdpReg(0x16, uint8_t((byteAddr >> 9)  & 0xFF));
    backend_->setVdpReg(0x17, uint8_t((byteAddr >> 17) & 0x7F));
}

QWidget* VdpRegView::buildModeTab()
{
    auto* page = new QWidget;
    auto* g = new QGridLayout(page);
    g->setContentsMargins(8, 8, 8, 8);
    g->setHorizontalSpacing(20);
    g->setVerticalSpacing(2);
    for (int col = 0; col < 4; ++col) {
        const int reg = kModeRegs[col];
        auto* hdr = new QLabel;
        hdr->setFont(QFont(QStringLiteral("Courier New"), 9, QFont::Bold));
        modeHdr_[col] = hdr;
        g->addWidget(hdr, 0, col);
        for (int bit = 7; bit >= 0; --bit) {
            auto* cb = addCheck(reg, bit, QStringLiteral("%1: %2").arg(bit)
                                    .arg(QLatin1StringView(kModeBits[col][bit])));
            g->addWidget(cb, 1 + (7 - bit), col);
        }
    }
    g->setRowStretch(9, 1);
    g->setColumnStretch(4, 1);
    return wrapScroll(page);
}

QWidget* VdpRegView::buildOtherTab()
{
    auto* page = new QWidget;
    auto lbl = [](const QString& t) { return new QLabel(t); };

    // ---- Base addresses (raw byte + decoded effective VRAM address) ----
    auto* baseGrp = new QGroupBox(QStringLiteral("Base Addresses"));
    auto* bg = new QGridLayout(baseGrp);
    bg->addWidget(lbl(QStringLiteral("Raw")),       0, 1, Qt::AlignHCenter);
    bg->addWidget(lbl(QStringLiteral("Effective")), 0, 2, Qt::AlignHCenter);
    int row = 1;
    auto baseRow = [&](const QString& name, QLineEdit* raw, QLineEdit* eff) {
        bg->addWidget(lbl(name), row, 0);
        bg->addWidget(raw, row, 1);
        bg->addWidget(eff, row, 2);
        ++row;
    };

    baseRow(QStringLiteral("0x02 Scroll A Base"),
        addField(2, {0x02},
            [](const uint8_t* r) { return uint32_t(r[0x02]); },
            [this](uint32_t v) { backend_->setVdpReg(0x02, uint8_t(v)); }),
        addField(5, {0x01, 0x02},
            [](const uint8_t* r) -> uint32_t {
                if (mode4(r)) return getBits(r[0x02], 1, 3) << 11;
                return getBits(r[0x02], 3, evram(r) ? 4 : 3) << 13;
            },
            [this](uint32_t v) { backend_->setVdpReg(0x02, uint8_t((v >> 10) & 0xFF)); }));

    baseRow(QStringLiteral("0x03 Window Base"),
        addField(2, {0x03},
            [](const uint8_t* r) { return uint32_t(r[0x03]); },
            [this](uint32_t v) { backend_->setVdpReg(0x03, uint8_t(v)); }),
        addField(5, {0x01, 0x03, 0x0C},
            [](const uint8_t* r) -> uint32_t {
                if (h40(r)) return getBits(r[0x03], 2, evram(r) ? 5 : 4) << 12;
                return getBits(r[0x03], 1, evram(r) ? 6 : 5) << 11;
            },
            [this](uint32_t v) { backend_->setVdpReg(0x03, uint8_t((v >> 10) & 0xFF)); }));

    baseRow(QStringLiteral("0x04 Scroll B Base"),
        addField(2, {0x04},
            [](const uint8_t* r) { return uint32_t(r[0x04]); },
            [this](uint32_t v) { backend_->setVdpReg(0x04, uint8_t(v)); }),
        addField(5, {0x01, 0x04},
            [](const uint8_t* r) { return getBits(r[0x04], 0, evram(r) ? 4 : 3) << 13; },
            [this](uint32_t v) { backend_->setVdpReg(0x04, uint8_t((v >> 13) & 0xFF)); }));

    baseRow(QStringLiteral("0x05 Sprite Table Base"),
        addField(2, {0x05},
            [](const uint8_t* r) { return uint32_t(r[0x05]); },
            [this](uint32_t v) { backend_->setVdpReg(0x05, uint8_t(v)); }),
        addField(5, {0x01, 0x05, 0x0C},
            [](const uint8_t* r) -> uint32_t {
                if (mode4(r)) return getBits(r[0x05], 1, 6) << 8;
                if (h40(r))   return getBits(r[0x05], 1, evram(r) ? 7 : 6) << 10;
                return getBits(r[0x05], 0, evram(r) ? 8 : 7) << 9;
            },
            [this](uint32_t v) {
                const bool m4 = mode4(backend_->getVdpState().reg);
                backend_->setVdpReg(0x05, uint8_t((v >> (m4 ? 7 : 9)) & 0xFF));
            }));

    baseRow(QStringLiteral("0x06 Sprite Pattern Base"),
        addField(2, {0x06},
            [](const uint8_t* r) { return uint32_t(r[0x06]); },
            [this](uint32_t v) { backend_->setVdpReg(0x06, uint8_t(v)); }),
        addField(5, {0x01, 0x06},
            [](const uint8_t* r) -> uint32_t {
                if (mode4(r)) return getBits(r[0x06], 2, 1) << 13;
                if (evram(r)) return getBits(r[0x06], 5, 1) << 16;
                return 0;   // Gens showed a stale value here; the port shows 0
            },
            [this](uint32_t v) {
                const bool m4 = mode4(backend_->getVdpState().reg);
                backend_->setVdpReg(0x06, uint8_t((v >> (m4 ? 13 : 16)) & 0xFF));
            }));

    baseRow(QStringLiteral("0x0D HScroll Base"),
        addField(2, {0x0D},
            [](const uint8_t* r) { return uint32_t(r[0x0D]); },
            [this](uint32_t v) { backend_->setVdpReg(0x0D, uint8_t(v)); }),
        addField(5, {0x01, 0x0D},
            [](const uint8_t* r) { return getBits(r[0x0D], 0, evram(r) ? 7 : 6) << 10; },
            [this](uint32_t v) { backend_->setVdpReg(0x0D, uint8_t((v >> 10) & 0xFF)); }));

    // ---- 0x07 Background color ----
    auto* bgcGrp = new QGroupBox(QStringLiteral("0x07 Background Color"));
    auto* bcg = new QGridLayout(bgcGrp);
    auto* bitRow = new QHBoxLayout;
    bitRow->addWidget(addCheck(0x07, 7, QStringLiteral("7")));
    bitRow->addWidget(addCheck(0x07, 6, QStringLiteral("6")));
    bitRow->addStretch();
    bcg->addWidget(lbl(QStringLiteral("Unused bits")), 0, 0);
    bcg->addLayout(bitRow, 0, 1);
    bcg->addWidget(lbl(QStringLiteral("Background Palette Row")), 1, 0);
    bcg->addWidget(addField(1, {0x07},
        [](const uint8_t* r) { return getBits(r[0x07], 4, 2); },
        [this](uint32_t v) { setRegBits(0x07, 4, 2, v); }), 1, 1);
    bcg->addWidget(lbl(QStringLiteral("Background Palette Column")), 2, 0);
    bcg->addWidget(addField(1, {0x07},
        [](const uint8_t* r) { return getBits(r[0x07], 0, 4); },
        [this](uint32_t v) { setRegBits(0x07, 0, 4, v); }), 2, 1);

    // ---- Whole-byte registers ----
    auto* scGrp = new QGroupBox(QStringLiteral("Scroll / Counters"));
    auto* scg = new QGridLayout(scGrp);
    int scRow = 0;
    auto plainReg = [&](int idx, const QString& name) {
        scg->addWidget(lbl(name), scRow, 0);
        scg->addWidget(addField(2, {idx},
            [idx](const uint8_t* r) { return uint32_t(r[idx]); },
            [this, idx](uint32_t v) { backend_->setVdpReg(idx, uint8_t(v)); }), scRow, 1);
        ++scRow;
    };
    plainReg(0x08, QStringLiteral("0x08 Background Scroll X"));
    plainReg(0x09, QStringLiteral("0x09 Background Scroll Y"));
    plainReg(0x0A, QStringLiteral("0x0A HINT Line Counter"));
    plainReg(0x0F, QStringLiteral("0x0F Auto Increment Data"));

    // ---- 0x0E pattern bases (128K VRAM) ----
    auto* patGrp = new QGroupBox(QStringLiteral("0x0E Pattern Bases (128K VRAM)"));
    auto* pg = new QGridLayout(patGrp);
    pg->addWidget(lbl(QStringLiteral("Bits 5-7")), 0, 0);
    pg->addWidget(addField(1, {0x0E},
        [](const uint8_t* r) { return getBits(r[0x0E], 5, 3); },
        [this](uint32_t v) { setRegBits(0x0E, 5, 3, v); }), 0, 1);
    pg->addWidget(lbl(QStringLiteral("Scroll A Pattern Base")), 1, 0);
    pg->addWidget(addField(1, {0x0E},
        [](const uint8_t* r) { return uint32_t(r[0x0E] & 0x0F); },
        [this](uint32_t v) {
            const uint8_t cur = backend_->getVdpState().reg[0x0E];
            backend_->setVdpReg(0x0E, uint8_t((cur & 0xF0) | (v & 0x0F)));
        }), 1, 1);
    pg->addWidget(addField(5, {0x01, 0x0E},
        [](const uint8_t* r) -> uint32_t {
            return evram(r) ? getBits(r[0x0E], 0, 1) << 16 : 0;
        },
        [this](uint32_t v) { setRegBits(0x0E, 0, 1, v >> 16); }), 1, 2);
    pg->addWidget(lbl(QStringLiteral("Bits 1-3")), 2, 0);
    pg->addWidget(addField(1, {0x0E},
        [](const uint8_t* r) { return getBits(r[0x0E], 1, 3); },
        [this](uint32_t v) { setRegBits(0x0E, 1, 3, v); }), 2, 1);
    pg->addWidget(lbl(QStringLiteral("Scroll B Pattern Base")), 3, 0);
    pg->addWidget(addField(1, {0x0E},
        [](const uint8_t* r) { return uint32_t((r[0x0E] >> 4) & 0x0F); },
        [this](uint32_t v) {
            const uint8_t cur = backend_->getVdpState().reg[0x0E];
            backend_->setVdpReg(0x0E, uint8_t((cur & 0x0F) | ((v & 0x0F) << 4)));
        }), 3, 1);
    pg->addWidget(addField(5, {0x01, 0x0E},
        [](const uint8_t* r) -> uint32_t {
            // nonzero only when both PA and PB pattern bits are set (Gens/HW quirk)
            return evram(r) ? (getBits(r[0x0E], 0, 1) << 16) & (getBits(r[0x0E], 4, 1) << 16) : 0;
        },
        [this](uint32_t v) {
            const uint32_t d = v >> 16;
            setRegBits(0x0E, 0, 1, d);
            if (d) setRegBits(0x0E, 4, 1, d);
        }), 3, 2);

    // ---- 0x10 scroll size ----
    auto* szGrp = new QGroupBox(QStringLiteral("0x10 Scroll Size"));
    auto* sg = new QGridLayout(szGrp);
    sg->addWidget(lbl(QStringLiteral("Bits 6-7")), 0, 0);
    sg->addWidget(addField(1, {0x10},
        [](const uint8_t* r) { return getBits(r[0x10], 6, 2); },
        [this](uint32_t v) { setRegBits(0x10, 6, 2, v); }), 0, 1);
    sg->addWidget(lbl(QStringLiteral("VSZ (Vertical Scroll Size)")), 1, 0);
    sg->addWidget(addField(1, {0x10},
        [](const uint8_t* r) { return getBits(r[0x10], 4, 2); },
        [this](uint32_t v) { setRegBits(0x10, 4, 2, v); }), 1, 1);
    sg->addWidget(addField(0, {0x10},
        [](const uint8_t* r) { return uint32_t(0x20 + ((r[0x10] >> 4) & 0x3) * 32); }), 1, 2);
    sg->addWidget(lbl(QStringLiteral("cells")), 1, 3);
    sg->addWidget(lbl(QStringLiteral("Bits 2-3")), 2, 0);
    sg->addWidget(addField(1, {0x10},
        [](const uint8_t* r) { return getBits(r[0x10], 2, 2); },
        [this](uint32_t v) { setRegBits(0x10, 2, 2, v); }), 2, 1);
    sg->addWidget(lbl(QStringLiteral("HSZ (Horizontal Scroll Size)")), 3, 0);
    sg->addWidget(addField(1, {0x10},
        [](const uint8_t* r) { return getBits(r[0x10], 0, 2); },
        [this](uint32_t v) { setRegBits(0x10, 0, 2, v); }), 3, 1);
    sg->addWidget(addField(0, {0x10},
        [](const uint8_t* r) { return uint32_t(0x20 + (r[0x10] & 0x3) * 32); }), 3, 2);
    sg->addWidget(lbl(QStringLiteral("cells")), 3, 3);

    // ---- 0x11 / 0x12 window position ----
    auto* wxGrp = new QGroupBox(QStringLiteral("0x11 Window X"));
    auto* wxg = new QGridLayout(wxGrp);
    wxg->addWidget(addCheck(0x11, 7, QStringLiteral("Align Window Right")), 0, 0, 1, 2);
    wxg->addWidget(lbl(QStringLiteral("Bits 5-6")), 1, 0);
    wxg->addWidget(addField(1, {0x11},
        [](const uint8_t* r) { return getBits(r[0x11], 5, 2); },
        [this](uint32_t v) { setRegBits(0x11, 5, 2, v); }), 1, 1);
    wxg->addWidget(lbl(QStringLiteral("Window Base Point X")), 2, 0);
    wxg->addWidget(addField(1, {0x11},
        [](const uint8_t* r) { return getBits(r[0x11], 0, 5); },
        [this](uint32_t v) { setRegBits(0x11, 0, 5, v); }), 2, 1);

    auto* wyGrp = new QGroupBox(QStringLiteral("0x12 Window Y"));
    auto* wyg = new QGridLayout(wyGrp);
    wyg->addWidget(addCheck(0x12, 7, QStringLiteral("Align Window Down")), 0, 0, 1, 2);
    wyg->addWidget(lbl(QStringLiteral("Bits 5-6")), 1, 0);
    wyg->addWidget(addField(1, {0x12},
        [](const uint8_t* r) { return getBits(r[0x12], 5, 2); },
        [this](uint32_t v) { setRegBits(0x12, 5, 2, v); }), 1, 1);
    wyg->addWidget(lbl(QStringLiteral("Window Base Point Y")), 2, 0);
    wyg->addWidget(addField(1, {0x12},
        [](const uint8_t* r) { return getBits(r[0x12], 0, 5); },
        [this](uint32_t v) { setRegBits(0x12, 0, 5, v); }), 2, 1);

    // ---- DMA ----
    auto* dmaGrp = new QGroupBox(QStringLiteral("DMA (0x13-0x17)"));
    auto* dg = new QGridLayout(dmaGrp);
    dg->addWidget(lbl(QStringLiteral("0x13-0x14 DMA Length (words)")), 0, 0);
    dg->addWidget(addField(4, {0x13, 0x14},
        [](const uint8_t* r) { return uint32_t(r[0x13] | (r[0x14] << 8)); },
        [this](uint32_t v) {
            backend_->setVdpReg(0x13, uint8_t(v & 0xFF));
            backend_->setVdpReg(0x14, uint8_t((v >> 8) & 0xFF));
        }), 0, 1);
    dg->addWidget(lbl(QStringLiteral("0x15-0x17 DMA Source (words)")), 1, 0);
    dg->addWidget(addField(6, {0x15, 0x16, 0x17},
        [](const uint8_t* r) { return dmaSrc(r) >> 1; },
        [this](uint32_t v) { writeDmaSrc(v << 1); }), 1, 1);
    dg->addWidget(lbl(QStringLiteral("Effective (bytes)")), 2, 0);
    dg->addWidget(addField(6, {0x15, 0x16, 0x17},
        [](const uint8_t* r) { return dmaSrc(r); },
        [this](uint32_t v) { writeDmaSrc(v); }), 2, 1);
    auto* dmdRow = new QHBoxLayout;
    dmdRow->addWidget(addCheck(0x17, 7, QStringLiteral("DMD1 (DMA Mode Bit 1)")));
    dmdRow->addWidget(addCheck(0x17, 6, QStringLiteral("DMD0 (DMA Mode Bit 0)")));
    dmdRow->addStretch();
    dg->addLayout(dmdRow, 3, 0, 1, 2);

    // ---- assemble ----
    auto* left = new QVBoxLayout;
    left->addWidget(baseGrp);
    left->addWidget(bgcGrp);
    left->addWidget(scGrp);
    left->addStretch();
    auto* winRow = new QHBoxLayout;
    winRow->addWidget(wxGrp);
    winRow->addWidget(wyGrp);
    auto* right = new QVBoxLayout;
    right->addWidget(patGrp);
    right->addWidget(szGrp);
    right->addLayout(winRow);
    right->addWidget(dmaGrp);
    right->addStretch();
    auto* cols = new QHBoxLayout(page);
    cols->setContentsMargins(8, 8, 8, 8);
    cols->addLayout(left);
    cols->addLayout(right);
    cols->addStretch();
    return wrapScroll(page);
}

void VdpRegView::syncControls()
{
    static const QString kRed = QStringLiteral("color:red;");

    for (int i = 0; i < 4; ++i) {
        const int reg = kModeRegs[i];
        modeHdr_[i]->setText(QStringLiteral("0x%1 (%2)  =  %3")
                                 .arg(hexU(reg, 2)).arg(reg).arg(hexU(regs_[reg], 2)));
        const QString ss = diff_[reg] ? kRed : QString();
        if (modeHdr_[i]->styleSheet() != ss) modeHdr_[i]->setStyleSheet(ss);
    }

    for (const auto& b : bits_) {
        b.box->setChecked((regs_[b.reg] >> b.bit) & 1);
        const QString ss = ((diff_[b.reg] >> b.bit) & 1) ? kRed : QString();
        if (b.box->styleSheet() != ss) b.box->setStyleSheet(ss);
    }

    for (const auto& f : fields_) {
        if (!(f.edit->hasFocus() && f.edit->isModified())) {
            const uint32_t v = f.read(regs_);
            const QString  t = f.width ? hexU(v, f.width) : QString::number(v);
            if (f.edit->text() != t) f.edit->setText(t);
        }
        const bool ch = std::any_of(f.deps.begin(), f.deps.end(),
                                    [this](int r) { return diff_[r] != 0; });
        const QString ss = ch ? kRed : QString();
        if (f.edit->styleSheet() != ss) f.edit->setStyleSheet(ss);
    }
}
