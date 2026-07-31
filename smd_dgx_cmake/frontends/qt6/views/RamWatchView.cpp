#include "RamWatchView.h"
#include "ViewSettings.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QGroupBox>
#include <QLabel>
#include <QFile>
#include <QTextStream>
#include <QFont>
#include <QColor>

// The watch list is the one piece of view state that represents work rather
// than preference: it is built up address by address over an investigation.
// It is round-tripped through the same .wch format the Open/Save buttons use,
// so the autosave is an ordinary file the user can also load by hand — and
// Gens can read it.
static QString autoWatchPath()
{
    return viewsettings::dataDir() + QStringLiteral("/autosave.wch");
}

RamWatchView::RamWatchView(QWidget* parent) : QWidget(parent)
{
    buildUi();
    if (QFile::exists(autoWatchPath()))
        loadFile(autoWatchPath(), true);
    // Not currentFile_: Save must not silently overwrite the autosave, and the
    // title should not claim the user opened it.
    currentFile_.clear();
    rebuildTable();
}

RamWatchView::~RamWatchView()
{
    if (watches_.isEmpty())
        QFile::remove(autoWatchPath());   // an emptied list must stay emptied
    else
        saveFile(autoWatchPath());
}

void RamWatchView::buildUi()
{
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Address"), QStringLiteral("Value"), QStringLiteral("Notes")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setFont(QFont(QStringLiteral("Courier New"), 9));
    connect(table_, &QTableWidget::cellDoubleClicked, this, &RamWatchView::onDoubleClicked);

    auto mkBtn = [this](const QString& t, void (RamWatchView::*fn)()) {
        auto* b = new QPushButton(t, this);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };

    auto* fileRow = new QHBoxLayout;
    fileRow->addWidget(mkBtn(QStringLiteral("New List"), &RamWatchView::onNewList));
    fileRow->addWidget(mkBtn(QStringLiteral("Open..."),  &RamWatchView::onOpen));
    fileRow->addWidget(mkBtn(QStringLiteral("Save..."),  &RamWatchView::onSave));
    fileRow->addWidget(mkBtn(QStringLiteral("Append..."),&RamWatchView::onAppend));
    fileRow->addStretch();

    auto* editCol = new QVBoxLayout;
    editCol->addWidget(mkBtn(QStringLiteral("New"),       &RamWatchView::onNewWatch));
    editCol->addWidget(mkBtn(QStringLiteral("Edit"),      &RamWatchView::onEditWatch));
    editCol->addWidget(mkBtn(QStringLiteral("Remove"),    &RamWatchView::onRemoveWatch));
    editCol->addWidget(mkBtn(QStringLiteral("Duplicate"), &RamWatchView::onDuplicateWatch));
    editCol->addWidget(mkBtn(QStringLiteral("Separator"), &RamWatchView::onSeparator));
    editCol->addWidget(mkBtn(QStringLiteral("Up"),        &RamWatchView::onMoveUp));
    editCol->addWidget(mkBtn(QStringLiteral("Down"),      &RamWatchView::onMoveDown));
    editCol->addStretch();

    auto* mid = new QHBoxLayout;
    mid->addWidget(table_, 1);
    mid->addLayout(editCol);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2,2,2,2);
    root->addLayout(fileRow);
    root->addLayout(mid, 1);
}

// --------------------------------------------------------------------------
// data
// --------------------------------------------------------------------------
bool RamWatchView::isValidWatchAddress(uint32_t a)
{
    a &= 0xFFFFFF;
    return (a >= 0xFF0000 && a < 0xFF0000 + 0x10000) ||
           (a >= 0xA00000 && a < 0xA00000 + 0x2000);
}

uint32_t RamWatchView::readValue(const WatchEntry& w) const
{
    if (!backend_ || w.isSeparator()) return 0;
    const int n = w.size == 'd' ? 4 : w.size == 'w' ? 2 : 1;
    auto b = backend_->readMemory(w.address, n);
    uint32_t v = 0;
    for (int i = 0; i < (int)b.size(); ++i) v = (v << 8) | b[i];
    return v;
}

QString RamWatchView::valueText(const WatchEntry& w) const
{
    if (w.isSeparator()) return QString();
    const uint32_t v = w.curValue;
    switch (w.type) {
    case 's':
        if (w.size == 'b') return QString::number((int)(int8_t)(v & 0xFF));
        if (w.size == 'w') return QString::number((int)(int16_t)(v & 0xFFFF));
        return QString::number((int)(int32_t)v);
    case 'u':
        if (w.size == 'b') return QString::number(v & 0xFF);
        if (w.size == 'w') return QString::number(v & 0xFFFF);
        return QString::number(v);
    default: {
        const int digits = w.size == 'd' ? 8 : w.size == 'w' ? 4 : 2;
        return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
    }
    }
}

void RamWatchView::refresh()
{
    if (!backend_) return;
    for (int i = 0; i < watches_.size(); ++i) {
        auto& w = watches_[i];
        if (w.isSeparator()) continue;
        uint32_t v = readValue(w);
        w.changed = (v != w.curValue);
        w.curValue = v;
        if (auto* item = table_->item(i, 1)) {
            item->setText(valueText(w));
            item->setForeground(w.changed ? QColor(Qt::red) : table_->palette().text().color());
        }
    }
}

void RamWatchView::setRow(int row)
{
    const auto& w = watches_[row];
    auto put = [&](int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        table_->setItem(row, col, item);
    };
    if (w.isSeparator()) {
        put(0, QString()); put(1, QString());
        put(2, QStringLiteral("----------------------------"));
    } else {
        put(0, QStringLiteral("%1").arg(w.address, 8, 16, QLatin1Char('0')).toUpper());
        put(1, valueText(w));
        put(2, w.comment);
    }
}

void RamWatchView::rebuildTable()
{
    table_->setRowCount(watches_.size());
    for (int i = 0; i < watches_.size(); ++i) setRow(i);
}

int RamWatchView::selectedRow() const
{
    auto sel = table_->selectionModel()->selectedRows();
    return sel.isEmpty() ? -1 : sel.first().row();
}

bool RamWatchView::insertWatch(const WatchEntry& w, int index)
{
    if (!w.isSeparator())
        for (const auto& e : watches_)
            if (!e.isSeparator() && e.address == w.address && e.size == w.size && e.type == w.type)
                return false; // silent duplicate skip, like the original
    if (index < 0 || index > watches_.size()) index = watches_.size();
    watches_.insert(index, w);
    rebuildTable();
    return true;
}

// --------------------------------------------------------------------------
// edit dialog
// --------------------------------------------------------------------------
bool RamWatchView::editDialog(WatchEntry& w, const QString& title)
{
    QDialog dlg(this);
    dlg.setWindowTitle(title);

    auto* addr = new QLineEdit(QStringLiteral("%1").arg(w.address, 0, 16).toUpper(), &dlg);
    auto* note = new QLineEdit(w.comment, &dlg);

    auto* sizeBox = new QGroupBox(QStringLiteral("Size"), &dlg);
    QRadioButton* sb = new QRadioButton(QStringLiteral("1 byte"), sizeBox);
    QRadioButton* sw = new QRadioButton(QStringLiteral("2 bytes"), sizeBox);
    QRadioButton* sd = new QRadioButton(QStringLiteral("4 bytes"), sizeBox);
    (w.size == 'd' ? sd : w.size == 'w' ? sw : sb)->setChecked(true);
    auto* sl = new QVBoxLayout(sizeBox); sl->addWidget(sb); sl->addWidget(sw); sl->addWidget(sd);

    auto* typeBox = new QGroupBox(QStringLiteral("Type"), &dlg);
    QRadioButton* ts = new QRadioButton(QStringLiteral("Signed"), typeBox);
    QRadioButton* tu = new QRadioButton(QStringLiteral("Unsigned"), typeBox);
    QRadioButton* th = new QRadioButton(QStringLiteral("Hex"), typeBox);
    (w.type == 'h' ? th : w.type == 'u' ? tu : ts)->setChecked(true);
    auto* tl = new QVBoxLayout(typeBox); tl->addWidget(ts); tl->addWidget(tu); tl->addWidget(th);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* form = new QVBoxLayout(&dlg);
    auto* r1 = new QHBoxLayout; r1->addWidget(new QLabel(QStringLiteral("Address (hex):"), &dlg)); r1->addWidget(addr);
    auto* r2 = new QHBoxLayout; r2->addWidget(new QLabel(QStringLiteral("Note:"), &dlg)); r2->addWidget(note);
    auto* r3 = new QHBoxLayout; r3->addWidget(sizeBox); r3->addWidget(typeBox);
    form->addLayout(r1); form->addLayout(r2); form->addLayout(r3); form->addWidget(bb);

    for (;;) {
        if (dlg.exec() != QDialog::Accepted) return false;
        bool ok = false;
        uint32_t a = addr->text().toUInt(&ok, 16);
        if ((a & 0xFF000000) == 0xFF000000) a &= 0xFFFFFF;
        if (!ok || !isValidWatchAddress(a)) {
            QMessageBox::warning(this, QStringLiteral("Invalid address"),
                QStringLiteral("Address must be 68k RAM (FF0000-FFFFFF) or Z80 RAM (A00000-A01FFF)."));
            continue;
        }
        w.address = a;
        w.comment = note->text();
        w.size = sd->isChecked() ? 'd' : sw->isChecked() ? 'w' : 'b';
        w.type = th->isChecked() ? 'h' : tu->isChecked() ? 'u' : 's';
        // even-address rule for word/dword like real 68k accesses
        if (w.size != 'b' && (w.address & 1)) w.address &= ~1u;
        return true;
    }
}

// --------------------------------------------------------------------------
// slots
// --------------------------------------------------------------------------
void RamWatchView::onNewList() { watches_.clear(); currentFile_.clear(); rebuildTable(); }

void RamWatchView::onNewWatch()
{
    WatchEntry w;
    w.address = 0xFF0000;
    if (editDialog(w, QStringLiteral("New Watch"))) {
        w.curValue = readValue(w);
        insertWatch(w, selectedRow() >= 0 ? selectedRow() + 1 : -1);
    }
}

void RamWatchView::onEditWatch()
{
    int r = selectedRow();
    if (r < 0 || watches_[r].isSeparator()) return;
    WatchEntry w = watches_[r];
    if (editDialog(w, QStringLiteral("Edit Watch"))) {
        w.curValue = readValue(w);
        watches_[r] = w;
        setRow(r);
    }
}

void RamWatchView::onRemoveWatch()
{
    int r = selectedRow();
    if (r < 0) return;
    watches_.remove(r);
    rebuildTable();
}

void RamWatchView::onDuplicateWatch()
{
    int r = selectedRow();
    if (r < 0 || watches_[r].isSeparator()) return;
    WatchEntry w = watches_[r];
    if (editDialog(w, QStringLiteral("Duplicate Watch"))) {
        w.curValue = readValue(w);
        insertWatch(w, r + 1);
    }
}

void RamWatchView::onSeparator()
{
    WatchEntry w; w.size = 'S'; w.type = 'S';
    int r = selectedRow();
    watches_.insert(r >= 0 ? r + 1 : watches_.size(), w);
    rebuildTable();
}

void RamWatchView::onMoveUp()
{
    int r = selectedRow();
    if (r <= 0) return;
    watches_.swapItemsAt(r, r - 1);
    rebuildTable();
    table_->selectRow(r - 1);
}

void RamWatchView::onMoveDown()
{
    int r = selectedRow();
    if (r < 0 || r >= watches_.size() - 1) return;
    watches_.swapItemsAt(r, r + 1);
    rebuildTable();
    table_->selectRow(r + 1);
}

void RamWatchView::onDoubleClicked(int row, int col)
{
    if (row < 0 || row >= watches_.size()) return;
    auto& w = watches_[row];
    if (w.isSeparator()) return;
    if (col == 1) {
        // poke value (extension over the original)
        if (!backend_) return;
        bool ok = false;
        QString cur = valueText(w);
        QString s = QInputDialog::getText(this, QStringLiteral("Set Value"),
            QStringLiteral("New value for %1 (%2):")
                .arg(w.address, 6, 16, QLatin1Char('0')).arg(QChar::fromLatin1(w.type)), QLineEdit::Normal, cur, &ok);
        if (!ok || s.isEmpty()) return;
        uint32_t v = w.type == 'h' ? s.toUInt(&ok, 16) : (uint32_t)s.toLongLong(&ok, 10);
        if (!ok) return;
        const int n = w.size == 'd' ? 4 : w.size == 'w' ? 2 : 1;
        uint8_t buf[4];
        for (int i = 0; i < n; ++i) buf[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
        backend_->writeMemory(w.address, buf, n);
        refresh();
    } else {
        onEditWatch();
    }
}

void RamWatchView::addWatch(uint32_t addr, int size, int type)
{
    WatchEntry w;
    w.address = addr & 0xFFFFFF;
    w.size = size == 4 ? 'd' : size == 2 ? 'w' : 'b';
    w.type = type == 2 ? 'h' : type == 1 ? 'u' : 's';
    bool ok = false;
    QString note = QInputDialog::getText(this, QStringLiteral("Add Watch"),
        QStringLiteral("Note for %1:").arg(w.address, 6, 16, QLatin1Char('0')), QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    w.comment = note;
    w.curValue = readValue(w);
    insertWatch(w, -1);
}

// --------------------------------------------------------------------------
// .wch file interchange (exact Gens format)
// --------------------------------------------------------------------------
bool RamWatchView::saveFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream ts(&f);
    ts << "0\n" << watches_.size() << "\n";
    for (int i = 0; i < watches_.size(); ++i) {
        const auto& w = watches_[i];
        ts << QStringLiteral("%1").arg(i, 5, 16, QLatin1Char('0')).toUpper() << '\t'
           << QStringLiteral("%1").arg(w.address, 8, 16, QLatin1Char('0')).toUpper() << '\t'
           << QChar::fromLatin1(w.size) << '\t' << QChar::fromLatin1(w.type) << '\t' << 0 << '\t'
           << (w.isSeparator() && w.comment.isEmpty()
                  ? QStringLiteral("----------------------------") : w.comment)
           << '\n';
    }
    currentFile_ = path;
    return true;
}

bool RamWatchView::loadFile(const QString& path, bool clear)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QTextStream ts(&f);
    QString mode = ts.readLine();
    if (mode.startsWith(QLatin1Char('1')) || mode.startsWith(QLatin1Char('2'))) {
        QMessageBox::warning(this, QStringLiteral("Possible Device Mismatch"),
            QStringLiteral("Watches for %1 addresses will be ignored.")
                .arg(mode.startsWith(QLatin1Char('2')) ? QStringLiteral("32X") : QStringLiteral("SegaCD")));
    }
    int count = ts.readLine().trimmed().toInt();
    if (clear) { watches_.clear(); currentFile_ = path; }
    for (int i = 0; i < count && !ts.atEnd(); ) {
        QString line = ts.readLine();
        if (line.isEmpty()) continue;
        ++i;
        QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 5) continue;
        WatchEntry w;
        w.address = parts[1].toUInt(nullptr, 16) & 0xFFFFFF;
        w.size = parts[2].isEmpty() ? 'b' : parts[2][0].toLatin1();
        w.type = parts[3].isEmpty() ? 's' : parts[3][0].toLatin1();
        w.comment = line.section(QLatin1Char('\t'), -1);   // after the LAST tab, like the original
        if (w.isSeparator()) w.comment.clear();
        w.curValue = readValue(w);
        insertWatch(w, -1);
    }
    rebuildTable();
    return true;
}

void RamWatchView::onOpen()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open Watches"), QString(),
                                             QStringLiteral("GENs Watchlist (*.wch);;All Files (*)"));
    if (!p.isEmpty() && !loadFile(p, true))
        QMessageBox::warning(this, QStringLiteral("RAM Watch"), QStringLiteral("Failed to load %1").arg(p));
}

void RamWatchView::onAppend()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Append Watches"), QString(),
                                             QStringLiteral("GENs Watchlist (*.wch);;All Files (*)"));
    if (!p.isEmpty() && !loadFile(p, false))
        QMessageBox::warning(this, QStringLiteral("RAM Watch"), QStringLiteral("Failed to load %1").arg(p));
}

void RamWatchView::onSave()
{
    QString p = currentFile_;
    if (p.isEmpty())
        p = QFileDialog::getSaveFileName(this, QStringLiteral("Save Watches"), QString(),
                                         QStringLiteral("GENs Watchlist (*.wch);;All Files (*)"));
    if (!p.isEmpty() && !saveFile(p))
        QMessageBox::warning(this, QStringLiteral("RAM Watch"), QStringLiteral("Failed to save %1").arg(p));
}
