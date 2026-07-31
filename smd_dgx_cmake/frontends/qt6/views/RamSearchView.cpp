#include "RamSearchView.h"
#include "ViewSettings.h"

#include <QTableView>
#include <QHeaderView>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFont>
#include <QColor>

// ---------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------
QString RamSearchModel::format(uint32_t v) const
{
    switch (dispType_) {
    case 0:
        if (valueSize_ == 1) return QString::number((int)(int8_t)(v & 0xFF));
        if (valueSize_ == 2) return QString::number((int)(int16_t)(v & 0xFFFF));
        return QString::number((int)(int32_t)v);
    case 1:
        if (valueSize_ == 1) return QString::number(v & 0xFF);
        if (valueSize_ == 2) return QString::number(v & 0xFFFF);
        return QString::number(v);
    default:
        return QStringLiteral("%1").arg(v, valueSize_ * 2, 16, QLatin1Char('0')).toUpper();
    }
}

QVariant RamSearchModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() >= (int)entries_.size()) return {};
    const Entry& e = entries_[idx.row()];
    if (role == Qt::DisplayRole) {
        switch (idx.column()) {
        case 0: return QStringLiteral("%1").arg(0xFF0000 + e.off, 8, 16, QLatin1Char('0')).toUpper();
        case 1: return format(e.cur);
        case 2: return format(e.prev);
        case 3: return e.changes;
        }
    }
    if (role == Qt::ForegroundRole && idx.column() == 1 && e.cur != e.prev)
        return QColor(Qt::red);
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignCenter);
    return {};
}

QVariant RamSearchModel::headerData(int s, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (s) {
    case 0: return QStringLiteral("Address");
    case 1: return QStringLiteral("Value");
    case 2: return QStringLiteral("Previous");
    case 3: return QStringLiteral("Changes");
    }
    return {};
}

// ---------------------------------------------------------------------------
// view
// ---------------------------------------------------------------------------
RamSearchView::RamSearchView(QWidget* parent) : QWidget(parent)
{
    model_ = new RamSearchModel(this);
    buildUi();
}

void RamSearchView::buildUi()
{
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setFont(QFont(QStringLiteral("Courier New"), 9));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(18);
    table_->horizontalHeader()->setStretchLastSection(true);

    // comparison operator
    cmpOp_ = new QComboBox(this);
    cmpOp_->addItems({
        QStringLiteral("Less Than"), QStringLiteral("Greater Than"),
        QStringLiteral("Less or Equal"), QStringLiteral("Greater or Equal"),
        QStringLiteral("Equal"), QStringLiteral("Not Equal"),
        QStringLiteral("Different By N"), QStringLiteral("Modulo N Is X"),
    });
    cmpOp_->setCurrentIndex(5); // Not Equal — classic first search vs previous
    opParamEdit_ = new QLineEdit(this);
    opParamEdit_->setPlaceholderText(QStringLiteral("N or N,X"));
    opParamEdit_->setFixedWidth(70);

    // compare-to
    cmpPrev_    = new QRadioButton(QStringLiteral("Previous Value"), this);
    cmpValue_   = new QRadioButton(QStringLiteral("Specific Value:"), this);
    cmpAddr_    = new QRadioButton(QStringLiteral("Specific Address:"), this);
    cmpChanges_ = new QRadioButton(QStringLiteral("Number of Changes:"), this);
    cmpPrev_->setChecked(true);
    cmpValueEdit_ = new QLineEdit(this);

    auto* cmpBox = new QGroupBox(QStringLiteral("Compare To / By"), this);
    auto* cg = new QGridLayout(cmpBox);
    cg->addWidget(cmpOp_, 0, 0);
    cg->addWidget(opParamEdit_, 0, 1);
    cg->addWidget(cmpPrev_, 1, 0, 1, 2);
    cg->addWidget(cmpValue_, 2, 0);
    cg->addWidget(cmpValueEdit_, 2, 1);
    cg->addWidget(cmpAddr_, 3, 0, 1, 2);
    cg->addWidget(cmpChanges_, 4, 0, 1, 2);

    // data size / type
    sizeBox_ = new QComboBox(this);
    sizeBox_->addItems({QStringLiteral("1 byte"), QStringLiteral("2 bytes"), QStringLiteral("4 bytes")});
    typeBox_ = new QComboBox(this);
    typeBox_->addItems({QStringLiteral("Signed"), QStringLiteral("Unsigned"), QStringLiteral("Hex")});
    misaligned_ = new QCheckBox(QStringLiteral("Check Misaligned"), this);
    autoSearch_ = new QCheckBox(QStringLiteral("Autosearch"), this);
    connect(sizeBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RamSearchView::onSizeOrAlignChanged);
    connect(misaligned_, &QCheckBox::toggled, this, &RamSearchView::onSizeOrAlignChanged);
    connect(typeBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int t) {
        model_->dispType_ = t; model_->resetAll();
    });

    auto* dataBox = new QGroupBox(QStringLiteral("Data"), this);
    auto* dg = new QGridLayout(dataBox);
    dg->addWidget(new QLabel(QStringLiteral("Size:"), this), 0, 0);  dg->addWidget(sizeBox_, 0, 1);
    dg->addWidget(new QLabel(QStringLiteral("Type:"), this), 1, 0);  dg->addWidget(typeBox_, 1, 1);
    dg->addWidget(misaligned_, 2, 0, 1, 2);
    dg->addWidget(autoSearch_, 3, 0, 1, 2);

    auto mkBtn = [this](const QString& t, void (RamSearchView::*fn)()) {
        auto* b = new QPushButton(t, this);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };
    auto* btnCol = new QVBoxLayout;
    btnCol->addWidget(mkBtn(QStringLiteral("Search"),              &RamSearchView::onSearch));
    btnCol->addWidget(mkBtn(QStringLiteral("Reset"),               &RamSearchView::onReset));
    btnCol->addWidget(mkBtn(QStringLiteral("Clear Change Counts"), &RamSearchView::onClearChanges));
    btnCol->addWidget(mkBtn(QStringLiteral("Undo"),                &RamSearchView::onUndo));
    btnCol->addWidget(mkBtn(QStringLiteral("Eliminate"),           &RamSearchView::onEliminate));
    btnCol->addWidget(mkBtn(QStringLiteral("Add Watch"),           &RamSearchView::onAddWatch));
    btnCol->addStretch();

    auto* side = new QVBoxLayout;
    side->addWidget(cmpBox);
    side->addWidget(dataBox);
    side->addLayout(btnCol);
    side->addStretch();

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(2,2,2,2);
    root->addWidget(table_, 1);
    root->addLayout(side);
}

// ---------------------------------------------------------------------------
// data plumbing
// ---------------------------------------------------------------------------
void RamSearchView::snapshot(std::vector<uint8_t>& out) const
{
    out = backend_->readRegion(regionId_, 0, regionSize_);
    if (out.size() < regionSize_) out.resize(regionSize_, 0);
}

uint32_t RamSearchView::valueAt(const std::vector<uint8_t>& mem, uint32_t off) const
{
    const int n = model_->valueSize_;
    uint32_t v = 0;
    for (int i = 0; i < n && off + i < mem.size(); ++i) v = (v << 8) | mem[off + i];
    return v;
}

int64_t RamSearchView::asSigned(uint32_t v) const
{
    switch (model_->valueSize_) {
    case 1: return (int8_t)(v & 0xFF);
    case 2: return (int16_t)(v & 0xFFFF);
    default: return (int32_t)v;
    }
}

void RamSearchView::rebuildAll()
{
    if (!backend_) return;
    snapshot(mem_);
    const int n = model_->valueSize_;
    const int step = misaligned_->isChecked() ? 1 : n;
    auto& es = model_->entries_;
    es.clear();
    es.reserve(regionSize_ / step);
    for (uint32_t off = 0; off + n <= regionSize_; off += step) {
        uint32_t v = valueAt(mem_, off);
        es.push_back({off, v, v, 0});
    }
    model_->resetAll();
}

void RamSearchView::refresh()
{
    if (!backend_ || model_->entries_.empty()) return;
    snapshot(mem_);
    for (auto& e : model_->entries_) {
        uint32_t v = valueAt(mem_, e.off);
        if (v != e.cur) { ++e.changes; e.cur = v; }
    }
    if (autoSearch_->isChecked()) onSearch();
    else model_->resetAll();
}

bool RamSearchView::matches(const RamSearchModel::Entry& e) const
{
    // left side: current value (or its change count)
    int64_t lhs, rhs;
    const bool signedCmp = (model_->dispType_ == 0);
    auto conv = [&](uint32_t v) -> int64_t { return signedCmp ? asSigned(v) : (int64_t)v; };

    if (cmpChanges_->isChecked()) {
        lhs = e.changes;
        rhs = cmpValueEdit_->text().toLongLong(nullptr, 0);
    } else {
        lhs = conv(e.cur);
        if (cmpPrev_->isChecked()) rhs = conv(e.prev);
        else if (cmpAddr_->isChecked()) {
            bool ok = false;
            uint32_t a = cmpValueEdit_->text().toUInt(&ok, 16);
            if (a >= regionBase_) a -= regionBase_;
            rhs = ok ? conv(valueAt(mem_, a)) : 0;
        } else {
            bool ok = false;
            const QString t = cmpValueEdit_->text().trimmed();
            rhs = model_->dispType_ == 2 ? (int64_t)t.toUInt(&ok, 16) : t.toLongLong(&ok, 10);
            if (!ok) rhs = 0;
        }
    }

    switch (cmpOp_->currentIndex()) {
    case 0: return lhs <  rhs;
    case 1: return lhs >  rhs;
    case 2: return lhs <= rhs;
    case 3: return lhs >= rhs;
    case 4: return lhs == rhs;
    case 5: return lhs != rhs;
    case 6: { // different by N
        int64_t n = opParamEdit_->text().toLongLong(nullptr, 0);
        return lhs - rhs == n || rhs - lhs == n;
    }
    case 7: { // modulo N is X
        const QStringList p = opParamEdit_->text().split(QLatin1Char(','));
        int64_t n = p.value(0).toLongLong(nullptr, 0);
        int64_t x = p.value(1).toLongLong(nullptr, 0);
        return n != 0 && ((lhs % n) + n) % n == x;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
void RamSearchView::onSearch()
{
    if (!backend_) return;
    if (model_->entries_.empty()) rebuildAll();
    undo_ = model_->entries_;
    auto& es = model_->entries_;
    std::vector<RamSearchModel::Entry> kept;
    kept.reserve(es.size());
    for (auto& e : es)
        if (matches(e)) { auto k = e; k.prev = k.cur; kept.push_back(k); }
    es.swap(kept);
    model_->resetAll();
}

void RamSearchView::onReset()       { undo_ = model_->entries_; rebuildAll(); }
void RamSearchView::onClearChanges(){ for (auto& e : model_->entries_) e.changes = 0; model_->resetAll(); }

void RamSearchView::onUndo()
{
    if (undo_.empty()) return;
    model_->entries_.swap(undo_);
    undo_.clear();
    model_->resetAll();
}

void RamSearchView::onEliminate()
{
    auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;
    undo_ = model_->entries_;
    std::vector<bool> kill(model_->entries_.size(), false);
    for (const auto& idx : sel) kill[idx.row()] = true;
    std::vector<RamSearchModel::Entry> kept;
    for (size_t i = 0; i < model_->entries_.size(); ++i)
        if (!kill[i]) kept.push_back(model_->entries_[i]);
    model_->entries_.swap(kept);
    model_->resetAll();
}

void RamSearchView::onAddWatch()
{
    auto sel = table_->selectionModel()->selectedRows();
    for (const auto& idx : sel) {
        const auto& e = model_->entries_[idx.row()];
        emit addWatchRequested(regionBase_ + e.off, model_->valueSize_, model_->dispType_);
    }
}

void RamSearchView::onSizeOrAlignChanged()
{
    model_->valueSize_ = sizeBox_->currentIndex() == 2 ? 4 : sizeBox_->currentIndex() == 1 ? 2 : 1;
    rebuildAll();
}
