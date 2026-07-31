#include "SaveStateView.h"

#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QFileInfo>
#include <QScopeGuard>
#include <QFont>
#include <QSet>

namespace {
const char* const kIndexFile = "index.tsv";
const char* const kNoGroup   = "Ungrouped";
} // namespace

SaveStateView::SaveStateView(QWidget* parent) : QWidget(parent) { buildUi(); }

void SaveStateView::buildUi()
{
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({ QStringLiteral("State"), QStringLiteral("Taken") });
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setRootIsDecorated(true);
    tree_->setFont(QFont(QStringLiteral("Courier New"), 9));
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &SaveStateView::onSelectionChanged);
    connect(tree_, &QTreeWidget::itemActivated,        this, &SaveStateView::onItemActivated);

    auto mk = [this](const QString& text, void (SaveStateView::*fn)()) {
        auto* b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };
    saveBtn_ = mk(QStringLiteral("Save..."), &SaveStateView::onSave);
    loadBtn_ = mk(QStringLiteral("Load"),    &SaveStateView::onLoad);
    renBtn_  = mk(QStringLiteral("Rename"),  &SaveStateView::onRename);
    delBtn_  = mk(QStringLiteral("Delete"),  &SaveStateView::onDelete);

    auto* row = new QHBoxLayout;
    row->addWidget(saveBtn_);
    row->addWidget(loadBtn_);
    row->addWidget(renBtn_);
    row->addWidget(delBtn_);
    row->addStretch();

    hintLabel_ = new QLabel(this);
    hintLabel_->setWordWrap(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->addLayout(row);
    root->addWidget(tree_, 1);
    root->addWidget(hintLabel_);

    updateButtons();
}

// ---------------------------------------------------------------------------
void SaveStateView::setStatesDir(const QString& dir)
{
    if (dir_ == dir) return;
    dir_ = dir;
    indexStamp_ = QDateTime();
    loadIndex();
    rebuildTree();
    updateButtons();
}

QString SaveStateView::pathOf(const Entry& e) const
{
    return QDir(dir_).filePath(e.file);
}

// Names become file names, so keep them to something every filesystem accepts.
QString SaveStateView::sanitize(const QString& s)
{
    QString out;
    for (QChar c : s) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            out += c;
        else if (c.isSpace())
            out += QLatin1Char('_');
    }
    if (out.isEmpty()) out = QStringLiteral("state");
    return out.left(48);
}

void SaveStateView::loadIndex()
{
    entries_.clear();
    if (dir_.isEmpty()) return;

    QFile f(QDir(dir_).filePath(QLatin1String(kIndexFile)));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QStringList p = ts.readLine().split(QLatin1Char('\t'));
        if (p.size() < 4) continue;
        Entry e;
        e.file = p[0]; e.group = p[1]; e.name = p[2]; e.taken = p[3];
        // Drop entries whose file went away behind our back.
        if (QFile::exists(QDir(dir_).filePath(e.file)))
            entries_.push_back(e);
    }
}

bool SaveStateView::saveIndex()
{
    if (dir_.isEmpty()) return false;
    const QScopeGuard stamp([this] {          // our own write must not look foreign
        const QFileInfo fi(QDir(dir_).filePath(QLatin1String(kIndexFile)));
        indexStamp_ = fi.exists() ? fi.lastModified() : QDateTime();
    });
    QDir().mkpath(dir_);
    QFile f(QDir(dir_).filePath(QLatin1String(kIndexFile)));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream ts(&f);
    for (const Entry& e : entries_)
        ts << e.file << '\t' << e.group << '\t' << e.name << '\t' << e.taken << '\n';
    return true;
}

void SaveStateView::rebuildTree()
{
    tree_->clear();
    QMap<QString, QTreeWidgetItem*> groups;

    for (int i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        const QString g = e.group.isEmpty() ? QLatin1String(kNoGroup) : e.group;
        QTreeWidgetItem*& parent = groups[g];
        if (!parent) {
            parent = new QTreeWidgetItem(tree_, { g });
            parent->setFirstColumnSpanned(true);
        }
        auto* item = new QTreeWidgetItem(parent, { e.name, e.taken });
        item->setData(0, Qt::UserRole, i);        // index into entries_
    }

    // Expand only now: expanding an item that has no children yet does
    // nothing, and the states would then sit hidden under a collapsed group.
    tree_->expandAll();
}

SaveStateView::Entry* SaveStateView::selectedEntry()
{
    auto* item = tree_->currentItem();
    if (!item) return nullptr;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return nullptr;             // a group header
    const int idx = v.toInt();
    if (idx < 0 || idx >= entries_.size()) return nullptr;
    return &entries_[idx];
}

void SaveStateView::updateButtons()
{
    const bool live = backend_ && backend_->isRunning();
    const bool haveDir = !dir_.isEmpty();
    const bool sel  = tree_ && tree_->currentItem()
                      && tree_->currentItem()->data(0, Qt::UserRole).isValid();

    saveBtn_->setEnabled(live && haveDir);
    loadBtn_->setEnabled(live && sel);
    renBtn_->setEnabled(sel);
    delBtn_->setEnabled(sel);

    if (!haveDir)      hintLabel_->setText(QStringLiteral("No database path yet."));
    else if (!live)    hintLabel_->setText(QStringLiteral("Start the emulator to save or load."));
    else               hintLabel_->clear();
}

void SaveStateView::refresh()
{
    // Re-read the index when it changed underneath us. This widget is not the
    // only writer: the numbered slots, the MCP tools, a second instance of the
    // view or a previous session all touch the same directory, and a list that
    // silently lags behind them is worse than no list.
    if (!dir_.isEmpty()) {
        const QFileInfo fi(QDir(dir_).filePath(QLatin1String(kIndexFile)));
        const QDateTime stamp = fi.exists() ? fi.lastModified() : QDateTime();
        if (stamp != indexStamp_) {
            indexStamp_ = stamp;
            loadIndex();
            rebuildTree();
        }
    }
    updateButtons();
}

void SaveStateView::onSelectionChanged() { updateButtons(); }

void SaveStateView::onItemActivated(QTreeWidgetItem* item, int)
{
    if (item && item->data(0, Qt::UserRole).isValid()) onLoad();
}

// ---------------------------------------------------------------------------
void SaveStateView::onSave()
{
    if (!backend_ || dir_.isEmpty()) return;

    // Offer the groups that already exist, so grouping needs no bookkeeping.
    QSet<QString> known;
    for (const Entry& e : entries_)
        if (!e.group.isEmpty()) known.insert(e.group);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Save state"));
    auto* name  = new QLineEdit(&dlg);
    auto* group = new QComboBox(&dlg);
    group->setEditable(true);
    group->addItem(QString());
    for (const QString& g : known) group->addItem(g);
    if (auto* cur = selectedEntry()) group->setCurrentText(cur->group);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* form = new QFormLayout(&dlg);
    form->addRow(QStringLiteral("Name:"),  name);
    form->addRow(QStringLiteral("Group:"), group);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted) return;
    if (name->text().trimmed().isEmpty()) return;

    QDir().mkpath(dir_);
    Entry e;
    e.name  = name->text().trimmed();
    e.group = group->currentText().trimmed();
    e.taken = QDateTime::currentDateTime().toString(Qt::ISODate);

    // Unique file name; the display name may repeat, the file must not.
    const QString stem = sanitize(e.name);
    for (int n = 0; ; ++n) {
        e.file = n ? QStringLiteral("%1_%2.gpx").arg(stem).arg(n)
                   : QStringLiteral("%1.gpx").arg(stem);
        if (!QFile::exists(pathOf(e))) break;
    }

    const QByteArray path = QFile::encodeName(pathOf(e));
    bool ok = false;
    if (!backend_->runSafely([&] { ok = backend_->saveState(path.constData()); }) || !ok) {
        QMessageBox::warning(this, QStringLiteral("Save state"),
                             QStringLiteral("Could not write %1").arg(pathOf(e)));
        return;
    }

    entries_.push_back(e);
    saveIndex();
    rebuildTree();
    updateButtons();
}

void SaveStateView::onLoad()
{
    Entry* e = selectedEntry();
    if (!e || !backend_) return;

    const QByteArray path = QFile::encodeName(pathOf(*e));
    bool ok = false;
    if (!backend_->runSafely([&] { ok = backend_->loadState(path.constData()); }) || !ok)
        QMessageBox::warning(this, QStringLiteral("Load state"),
                             QStringLiteral("Could not load %1").arg(e->name));
}

void SaveStateView::onRename()
{
    Entry* e = selectedEntry();
    if (!e) return;

    QSet<QString> known;
    for (const Entry& x : entries_)
        if (!x.group.isEmpty()) known.insert(x.group);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Rename state"));
    auto* name  = new QLineEdit(e->name, &dlg);
    auto* group = new QComboBox(&dlg);
    group->setEditable(true);
    group->addItem(QString());
    for (const QString& g : known) group->addItem(g);
    group->setCurrentText(e->group);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* form = new QFormLayout(&dlg);
    form->addRow(QStringLiteral("Name:"),  name);
    form->addRow(QStringLiteral("Group:"), group);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted) return;
    if (name->text().trimmed().isEmpty()) return;

    // Only the metadata changes; the state file keeps its name.
    e->name  = name->text().trimmed();
    e->group = group->currentText().trimmed();
    saveIndex();
    rebuildTree();
    updateButtons();
}

void SaveStateView::onDelete()
{
    Entry* e = selectedEntry();
    if (!e) return;

    if (QMessageBox::question(this, QStringLiteral("Delete state"),
                              QStringLiteral("Delete \"%1\"?").arg(e->name))
        != QMessageBox::Yes)
        return;

    QFile::remove(pathOf(*e));
    entries_.removeAt(int(e - entries_.constData()));
    saveIndex();
    rebuildTree();
    updateButtons();
}
