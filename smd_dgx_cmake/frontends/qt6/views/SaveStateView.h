#pragma once
#include <QWidget>
#include <QString>
#include <QVector>
#include <QDateTime>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;
QT_END_NAMESPACE

// Save-state manager: named, grouped snapshots instead of ten numbered slots.
//
// Slots are fine for "undo the last thirty seconds" and useless for keeping a
// dozen scenes around, which is what a reverser actually wants — you come back
// to "boss room, low health" a week later and a bare `.gp3` tells you nothing.
//
// Storage: one directory next to the database, one file per state, plus a
// tab-separated index carrying the display name, the group and when it was
// taken. The states themselves stay plain emulator save states, so the numbered
// slots and the MCP tools keep working on the same files.
class SaveStateView : public QWidget {
    Q_OBJECT
public:
    explicit SaveStateView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b) { backend_ = b; updateButtons(); }

    // Where states live. Hosts differ (IDA knows the database path, the
    // standalone app the ROM path), so the host supplies it.
    void setStatesDir(const QString& dir);

    void refresh();          // called by the host's timer; only touches buttons

private slots:
    void onSave();
    void onLoad();
    void onRename();
    void onDelete();
    void onSelectionChanged();
    void onItemActivated(QTreeWidgetItem* item, int column);

private:
    struct Entry {
        QString file;        // file name inside dir_
        QString name;
        QString group;
        QString taken;       // ISO timestamp
    };

    void buildUi();
    void loadIndex();
    bool saveIndex();
    void rebuildTree();
    void updateButtons();
    Entry* selectedEntry();
    QString pathOf(const Entry& e) const;
    static QString sanitize(const QString& s);

    IDebugBackend*  backend_ = nullptr;
    QString         dir_;
    QDateTime       indexStamp_;      // index mtime we last read
    QVector<Entry>  entries_;

    QTreeWidget* tree_    = nullptr;
    QPushButton* saveBtn_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* renBtn_  = nullptr;
    QPushButton* delBtn_  = nullptr;
    QLabel*      hintLabel_ = nullptr;
};
