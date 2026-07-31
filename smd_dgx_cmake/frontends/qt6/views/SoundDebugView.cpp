#include "SoundDebugView.h"
#include "debugger/IDebugBackend.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QRadioButton>
#include <QTableWidget>
#include <QVBoxLayout>

// UI operator column -> register offset within the 0x10 stride (op1,op3,op2,op4
// hardware order; UI shows musical OP1..OP4).
static const int kOpOff[4] = { 0x0, 0x8, 0x4, 0xC };
// CH3 special mode per-operator frequency registers (part 1, fixed addresses).
static const int kCh3Lo[4] = { 0xA9, 0xAA, 0xA8, 0xA2 };
static const int kCh3Hi[4] = { 0xAD, 0xAE, 0xAC, 0xA6 };
static const char* kLfoHz[8] = { "3.98","5.56","6.02","6.37","6.88","9.63","48.1","72.2" };
static const char* kNoiseClk[4] = { "Clock/2","Clock/4","Clock/8","Tone 3" };
static const char* kOpRows[11] = {
    "Total Level","Sustain Level","Attack Rate","Decay Rate","Sustain Rate",
    "Release Rate","SSG-EG Mode","Detune","Multiple","Key Scale","AM Enable"
};

static QString hexStr(int v, int w)
{
    return QStringLiteral("%1").arg(v, w, 16, QLatin1Char('0')).toUpper();
}

// ---------------------------------------------------------------- AdsrCanvas

AdsrCanvas::AdsrCanvas(QWidget* parent) : QWidget(parent)
{
    setFixedSize(160, 80);   // original is 280x140; W = 2*H kept
}

void AdsrCanvas::setParams(int tl, int ar, int dr, int sl, int sr, int rr)
{
    if (tl == tl_ && ar == ar_ && dr == dr_ && sl == sl_ && sr == sr_ && rr == rr_ && valid_)
        return;
    tl_ = tl; ar_ = ar; dr_ = dr; sl_ = sl; sr_ = sr; rr_ = rr; valid_ = true;
    update();
}

void AdsrCanvas::setValid(bool v)
{
    if (valid_ == v) return;
    valid_ = v;
    update();
}

void AdsrCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(105, 105, 105));
    if (!valid_) return;

    const int W = width(), H = height();
    auto mapv = [](int v, int mx, int out) { return (mx > 0 && out > 0) ? v * out / mx : 0; };
    auto Y = [H](int y) { return (H - 1) - y; };   // y axis flipped, 0 = bottom

    const int total    = mapv(0x7F - tl_, 0x7F, H - 1);
    const int aw       = 0x1F - ar_;               // inverted: high AR = narrow
    const int maxW     = aw + dr_ + 31 + rr_;      // SUSTAIN_X = 31
    const int attack   = mapv(aw,  maxW, W - 1);
    const int decay    = mapv(dr_, maxW, W - 1);
    const int sustainX = mapv(31,  maxW, W - 1);
    const int release  = mapv(rr_, maxW, W - 1);
    const int susLvl   = mapv(0x0F - sl_, 0x0F, total);
    const int susRate  = mapv(sr_, 0x1F, susLvl);

    QPen dotted(QColor(47, 79, 79));
    dotted.setStyle(Qt::DotLine);

    p.setPen(dotted);                              // TL guide
    p.drawLine(0, Y(total), attack, Y(total));
    p.setPen(QColor(255, 0, 0));                   // attack
    p.drawLine(0, Y(0), attack, Y(total));

    p.setPen(dotted);                              // projection to right edge
    if (total >= susLvl)
        p.drawLine(attack, Y(total), W - 1, Y(total));
    else if (susRate == 0)
        p.drawLine(attack + decay, Y(susLvl), W - 1, Y(total));
    else
        p.drawLine(attack + decay + sustainX, Y(susLvl - susRate), W - 1, Y(total));

    p.setPen(QColor(0, 0, 255));                   // decay
    p.drawLine(attack, Y(total), attack + decay, Y(susLvl));
    p.setPen(QColor(0, 255, 0));                   // sustain slope
    p.drawLine(attack + decay, Y(susLvl), attack + decay + sustainX, Y(susLvl - susRate));
    p.setPen(QColor(255, 0, 255));                 // release
    p.drawLine(attack + decay + sustainX, Y(susLvl - susRate),
               attack + decay + sustainX + release, Y(0));
}

// ------------------------------------------------------------ SoundDebugView

SoundDebugView::SoundDebugView(QWidget* parent) : QWidget(parent)
{
    buildUi();
    clearAll();
}

void SoundDebugView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    refresh();
}

void SoundDebugView::buildUi()
{
    const QFont mono(QStringLiteral("Courier New"), 9);

    auto mkVal = [&](int minW = 40) {
        auto* l = new QLabel(QStringLiteral("--"), this);
        l->setFont(mono);
        l->setMinimumWidth(minW);
        return l;
    };
    auto mkChk = [&](const QString& t) {
        auto* c = new QCheckBox(t, this);
        c->setEnabled(false);          // read-only view
        return c;
    };
    auto mkBar = [&]() {
        auto* b = new QProgressBar(this);
        b->setRange(0, 15);
        b->setValue(0);
        b->setTextVisible(true);
        b->setFormat(QString());
        b->setFixedSize(110, 16);
        return b;
    };

    // Channel select
    auto* chBox  = new QGroupBox(QStringLiteral("Channel Select"), this);
    auto* chGrid = new QGridLayout(chBox);
    chGroup_ = new QButtonGroup(this);
    for (int i = 0; i < 6; ++i) {
        auto* r = new QRadioButton(QStringLiteral("Channel %1").arg(i + 1), chBox);
        chGroup_->addButton(r, i);
        chGrid->addWidget(r, i % 3, i / 3);
        if (i == 0) r->setChecked(true);
    }
    connect(chGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        selChannel_ = id;
        refresh();
    });

    // Channel registers
    auto* crBox  = new QGroupBox(QStringLiteral("Channel Registers"), this);
    auto* crForm = new QFormLayout(crBox);
    crForm->addRow(QStringLiteral("F-Number:"),       fnum_  = mkVal());
    crForm->addRow(QStringLiteral("Block:"),          block_ = mkVal());
    crForm->addRow(QStringLiteral("Algorithm:"),      alg_   = mkVal());
    crForm->addRow(QStringLiteral("OP1 Feedback:"),   fb_    = mkVal());
    crForm->addRow(QStringLiteral("AM Sensitivity:"), ams_   = mkVal());
    crForm->addRow(QStringLiteral("PM Sensitivity:"), pms_   = mkVal());
    auto* lr = new QHBoxLayout;
    lr->addWidget(outL_ = mkChk(QStringLiteral("Left")));
    lr->addWidget(outR_ = mkChk(QStringLiteral("Right")));
    lr->addStretch();
    crForm->addRow(QStringLiteral("Output:"), lr);

    // LFO
    auto* lfoBox  = new QGroupBox(QStringLiteral("LFO"), this);
    auto* lfoForm = new QFormLayout(lfoBox);
    lfoForm->addRow(lfoEn_ = mkChk(QStringLiteral("Enabled")));
    lfoForm->addRow(QStringLiteral("Frequency:"),      lfoFreq_ = mkVal());
    lfoForm->addRow(QStringLiteral("Real Frequency:"), lfoHz_   = mkVal(64));

    // DAC
    auto* dacBox  = new QGroupBox(QStringLiteral("DAC"), this);
    auto* dacForm = new QFormLayout(dacBox);
    dacForm->addRow(dacEn_ = mkChk(QStringLiteral("Enabled")));
    dacForm->addRow(QStringLiteral("Data:"), dacData_ = mkVal());

    // Timers ("Freq" fields of the original are periods in ms)
    auto* tBox  = new QGroupBox(QStringLiteral("Timers"), this);
    auto* tGrid = new QGridLayout(tBox);
    tGrid->addWidget(new QLabel(QStringLiteral("A"), tBox), 0, 1, Qt::AlignHCenter);
    tGrid->addWidget(new QLabel(QStringLiteral("B"), tBox), 0, 2, Qt::AlignHCenter);
    tGrid->addWidget(new QLabel(QStringLiteral("Loaded"),  tBox), 1, 0);
    tGrid->addWidget(new QLabel(QStringLiteral("Enabled"), tBox), 2, 0);
    tGrid->addWidget(new QLabel(QStringLiteral("Overflow"), tBox), 3, 0);
    tGrid->addWidget(new QLabel(QStringLiteral("Period"),  tBox), 4, 0);
    for (int t = 0; t < 2; ++t) {
        tGrid->addWidget(tLoad_[t] = mkChk(QString()), 1, 1 + t, Qt::AlignHCenter);
        tGrid->addWidget(tEn_[t]   = mkChk(QString()), 2, 1 + t, Qt::AlignHCenter);
        auto* na = new QLabel(QStringLiteral("N/A"), tBox);   // YM2612.Status not exposed
        na->setFont(mono);
        tGrid->addWidget(na, 3, 1 + t, Qt::AlignHCenter);
        tGrid->addWidget(tPeriod_[t] = mkVal(64), 4, 1 + t);
    }

    // Operator registers
    auto* opBox = new QGroupBox(QStringLiteral("Operator Registers"), this);
    auto* opLay = new QVBoxLayout(opBox);
    opTable_ = new QTableWidget(11, 4, opBox);
    opTable_->setHorizontalHeaderLabels({ QStringLiteral("OP1"), QStringLiteral("OP2"),
                                          QStringLiteral("OP3"), QStringLiteral("OP4") });
    QStringList rows;
    for (const char* n : kOpRows) rows << QLatin1StringView(n);
    opTable_->setVerticalHeaderLabels(rows);
    opTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    opTable_->setSelectionMode(QAbstractItemView::NoSelection);
    opTable_->setFont(mono);
    opTable_->verticalHeader()->setDefaultSectionSize(20);
    opTable_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    opTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int c = 0; c < 4; ++c) opTable_->setColumnWidth(c, 44);
    for (int r = 0; r < 11; ++r)
        for (int c = 0; c < 4; ++c) {
            auto* it = new QTableWidgetItem;
            it->setTextAlignment(Qt::AlignCenter);
            if (r == 10) {                          // AM Enable row = checkbox cells
                it->setFlags(Qt::ItemIsEnabled);
                it->setCheckState(Qt::Unchecked);
            } else {
                it->setText(QStringLiteral("--"));
            }
            opTable_->setItem(r, c, it);
        }
    opTable_->setMinimumSize(100 + 4 * 44 + 6, 11 * 20 + 30);
    opLay->addWidget(opTable_);

    // Channel 3 special
    auto* c3Box  = new QGroupBox(QStringLiteral("Channel 3 Special"), this);
    auto* c3Grid = new QGridLayout(c3Box);
    c3Grid->addWidget(new QLabel(QStringLiteral("Mode:"), c3Box), 0, 0);
    c3Grid->addWidget(ch3Mode_ = mkVal(), 0, 1);
    c3Grid->addWidget(new QLabel(QStringLiteral("F-Num"), c3Box), 1, 1);
    c3Grid->addWidget(new QLabel(QStringLiteral("Block"), c3Box), 1, 2);
    for (int op = 0; op < 4; ++op) {
        c3Grid->addWidget(new QLabel(QStringLiteral("OP%1").arg(op + 1), c3Box), 2 + op, 0);
        c3Grid->addWidget(ch3Fnum_[op]  = mkVal(), 2 + op, 1);
        c3Grid->addWidget(ch3Block_[op] = mkVal(), 2 + op, 2);
    }

    // Key on/off — reg shadow only keeps the last 0x28 command
    auto* keyBox = new QGroupBox(QStringLiteral("Key On/Off (last 0x28 write)"), this);
    auto* keyLay = new QVBoxLayout(keyBox);
    keyLay->addWidget(keyCh_ = mkVal(96));
    auto* keyOps = new QHBoxLayout;
    for (int op = 0; op < 4; ++op)
        keyOps->addWidget(keyOp_[op] = mkChk(QStringLiteral("Op %1").arg(op + 1)));
    keyOps->addStretch();
    keyLay->addLayout(keyOps);

    // Operator envelopes
    auto* envBox  = new QGroupBox(QStringLiteral("Operator Envelopes"), this);
    auto* envGrid = new QGridLayout(envBox);
    for (int op = 0; op < 4; ++op) {
        envGrid->addWidget(new QLabel(QStringLiteral("OP%1").arg(op + 1), envBox), op, 0);
        envGrid->addWidget(adsr_[op] = new AdsrCanvas(envBox), op, 1);
    }

    // PSG
    auto* psgBox  = new QGroupBox(QStringLiteral("PSG"), this);
    auto* psgGrid = new QGridLayout(psgBox);
    psgGrid->addWidget(new QLabel(QStringLiteral("Hz"),     psgBox), 0, 1);
    psgGrid->addWidget(new QLabel(QStringLiteral("Value"),  psgBox), 0, 2);
    psgGrid->addWidget(new QLabel(QStringLiteral("Volume"), psgBox), 0, 3);
    for (int i = 0; i < 3; ++i) {
        psgGrid->addWidget(new QLabel(QStringLiteral("Tone %1").arg(i + 1), psgBox), 1 + i, 0);
        psgGrid->addWidget(psgHz_[i]  = mkVal(48), 1 + i, 1);
        psgGrid->addWidget(psgVal_[i] = mkVal(40), 1 + i, 2);
        psgGrid->addWidget(psgBar_[i] = mkBar(),   1 + i, 3);
    }
    psgGrid->addWidget(new QLabel(QStringLiteral("Noise"), psgBox), 4, 0);
    psgGrid->addWidget(noiseClock_ = mkVal(56), 4, 1);
    psgGrid->addWidget(noiseType_  = mkVal(56), 4, 2);
    psgGrid->addWidget(psgBar_[3]  = mkBar(),   4, 3);

    // Assemble
    auto* col1 = new QVBoxLayout;
    col1->addWidget(chBox);
    col1->addWidget(crBox);
    col1->addWidget(lfoBox);
    col1->addWidget(dacBox);
    col1->addWidget(tBox);
    col1->addStretch();

    auto* col2 = new QVBoxLayout;
    col2->addWidget(opBox);
    col2->addWidget(c3Box);
    col2->addWidget(keyBox);
    col2->addStretch();

    auto* col3 = new QVBoxLayout;
    col3->addWidget(envBox);
    col3->addWidget(psgBox);
    col3->addStretch();

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->addLayout(col1);
    root->addLayout(col2);
    root->addLayout(col3);
    root->addStretch();
}

void SoundDebugView::refresh()
{
    if (!backend_) { clearAll(); return; }
    const SoundState s = backend_->getSoundState();
    updateFm(s);
    updatePsg(s);
}

void SoundDebugView::updateFm(const SoundState& s)
{
    const int part = selChannel_ / 3;
    const int base = selChannel_ % 3;
    auto rd  = [&](int addr) { return (int)s.fm[part][addr]; };
    auto rd0 = [&](int addr) { return (int)s.fm[0][addr]; };

    // Channel registers
    const int b4 = rd(0xB4 + base);
    fnum_ ->setText(hexStr(rd(0xA0 + base) | ((rd(0xA4 + base) & 7) << 8), 3));
    block_->setText(hexStr((rd(0xA4 + base) >> 3) & 7, 1));
    alg_  ->setText(hexStr(rd(0xB0 + base) & 7, 1));
    fb_   ->setText(hexStr((rd(0xB0 + base) >> 3) & 7, 1));
    ams_  ->setText(hexStr((b4 >> 4) & 3, 1));
    pms_  ->setText(hexStr(b4 & 7, 1));
    outL_ ->setChecked(b4 & 0x80);
    outR_ ->setChecked(b4 & 0x40);

    // Operators
    for (int op = 0; op < 4; ++op) {
        const int o   = base + kOpOff[op];
        const int tl  = rd(0x40 + o) & 0x7F;
        const int sl  = (rd(0x80 + o) >> 4) & 0xF;
        const int ar  = rd(0x50 + o) & 0x1F;
        const int dr  = rd(0x60 + o) & 0x1F;
        const int sr  = rd(0x70 + o) & 0x1F;
        const int rr  = rd(0x80 + o) & 0xF;
        opTable_->item(0, op)->setText(hexStr(tl, 2));
        opTable_->item(1, op)->setText(hexStr(sl, 1));
        opTable_->item(2, op)->setText(hexStr(ar, 2));
        opTable_->item(3, op)->setText(hexStr(dr, 2));
        opTable_->item(4, op)->setText(hexStr(sr, 2));
        opTable_->item(5, op)->setText(hexStr(rr, 1));
        opTable_->item(6, op)->setText(hexStr(rd(0x90 + o) & 0xF, 1));
        opTable_->item(7, op)->setText(hexStr((rd(0x30 + o) >> 4) & 7, 1));
        opTable_->item(8, op)->setText(hexStr(rd(0x30 + o) & 0xF, 1));
        opTable_->item(9, op)->setText(hexStr((rd(0x50 + o) >> 6) & 3, 1));
        opTable_->item(10, op)->setCheckState((rd(0x60 + o) & 0x80) ? Qt::Checked : Qt::Unchecked);
        adsr_[op]->setParams(tl, ar, dr, sl, sr, rr);
    }

    // LFO
    const int r22 = rd0(0x22);
    lfoEn_  ->setChecked(r22 & 0x08);
    lfoFreq_->setText(hexStr(r22 & 7, 2));
    lfoHz_  ->setText(QStringLiteral("%1 Hz").arg(QLatin1StringView(kLfoHz[r22 & 7])));

    // DAC (shadow is hook-maintained, so 0x2A holds the real last sample byte)
    dacEn_  ->setChecked(rd0(0x2B) & 0x80);
    dacData_->setText(hexStr(rd0(0x2A), 2));

    // Timers
    const int r27 = rd0(0x27);
    tLoad_[0]->setChecked(r27 & 1);
    tLoad_[1]->setChecked(r27 & 2);
    tEn_[0]  ->setChecked(r27 & 4);
    tEn_[1]  ->setChecked(r27 & 8);
    const int ta = (rd0(0x24) << 2) | (rd0(0x25) & 3);
    tPeriod_[0]->setText(QStringLiteral("%1 ms").arg(QString::number((0x400 - ta) * 0.01877, 'g', 4)));
    tPeriod_[1]->setText(QStringLiteral("%1 ms").arg(QString::number((0x100 - rd0(0x26)) * 0.30034, 'g', 4)));

    // Channel 3 special
    ch3Mode_->setText(hexStr((r27 >> 6) & 3, 1));
    for (int op = 0; op < 4; ++op) {
        ch3Fnum_[op] ->setText(hexStr(rd0(kCh3Lo[op]) | ((rd0(kCh3Hi[op]) & 7) << 8), 3));
        ch3Block_[op]->setText(hexStr((rd0(kCh3Hi[op]) >> 3) & 7, 1));
    }

    // Key on/off: decode the last 0x28 command byte (bits 0-2 channel, 4-7 op mask)
    const int key  = rd0(0x28);
    const int code = key & 7;
    if ((code & 3) == 3)
        keyCh_->setText(QStringLiteral("--"));
    else
        keyCh_->setText(QStringLiteral("Channel %1").arg((code & 3) + ((code & 4) ? 3 : 0) + 1));
    for (int op = 0; op < 4; ++op)
        keyOp_[op]->setChecked(key & (0x10 << op));
}

void SoundDebugView::updatePsg(const SoundState& s)
{
    for (int i = 0; i < 3; ++i) {
        const int per = s.psg[i * 2] & 0x3FF;
        psgVal_[i]->setText(QString::number(per));
        psgHz_[i] ->setText(QString::number(per ? (int)(3579545.0 / (per * 32)) : 0));
        const int att = s.psg[i * 2 + 1] & 0xF;
        psgBar_[i]->setValue(15 - att);
        psgBar_[i]->setFormat(QStringLiteral("att %1").arg(att));
    }
    const int nc = s.psg[6];
    noiseClock_->setText(QLatin1StringView(kNoiseClk[nc & 3]));
    noiseType_ ->setText(((nc >> 2) & 1) ? QStringLiteral("White") : QStringLiteral("Periodic"));
    const int natt = s.psg[7] & 0xF;
    psgBar_[3]->setValue(15 - natt);
    psgBar_[3]->setFormat(QStringLiteral("att %1").arg(natt));
}

void SoundDebugView::clearAll()
{
    const QString dash = QStringLiteral("--");
    for (QLabel* l : { fnum_, block_, alg_, fb_, ams_, pms_, lfoFreq_, lfoHz_, dacData_,
                       tPeriod_[0], tPeriod_[1], ch3Mode_, keyCh_,
                       ch3Fnum_[0], ch3Fnum_[1], ch3Fnum_[2], ch3Fnum_[3],
                       ch3Block_[0], ch3Block_[1], ch3Block_[2], ch3Block_[3],
                       psgHz_[0], psgHz_[1], psgHz_[2],
                       psgVal_[0], psgVal_[1], psgVal_[2],
                       noiseType_, noiseClock_ })
        l->setText(dash);
    for (QCheckBox* c : { outL_, outR_, lfoEn_, dacEn_,
                          tLoad_[0], tLoad_[1], tEn_[0], tEn_[1],
                          keyOp_[0], keyOp_[1], keyOp_[2], keyOp_[3] })
        c->setChecked(false);
    for (int r = 0; r < 11; ++r)
        for (int c = 0; c < 4; ++c) {
            if (r == 10) opTable_->item(r, c)->setCheckState(Qt::Unchecked);
            else         opTable_->item(r, c)->setText(dash);
        }
    for (auto* b : psgBar_) { b->setValue(0); b->setFormat(QString()); }
    for (auto* a : adsr_) a->setValid(false);
}
