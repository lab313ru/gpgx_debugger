#pragma once
#include <QWidget>
#include <QString>
#include <QVector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QTableWidget;
QT_END_NAMESPACE

// Port of the Gens RAM Watch tool (ramwatch.cpp): ordered watch list over the
// 68k bus, .wch file interchange with Gens (open/append/save), separators,
// Edit Watch dialog. Adds in-place value poking via double-click on Value
// (the original had no memory write path).
// Omitted vs original: recent-files menu, drag&drop, auto-load option.
class RamWatchView : public QWidget {
    Q_OBJECT
public:
    explicit RamWatchView(QWidget* parent = nullptr);
    ~RamWatchView() override;               // autosaves the watch list
    void setBackend(IDebugBackend* b) { backend_ = b; }
    void refresh();

    struct WatchEntry {
        uint32_t address  = 0;
        uint32_t curValue = 0;
        QString  comment;
        char     size = 'b';   // 'b'/'w'/'d', 'S' = separator
        char     type = 's';   // 's'/'u'/'h', 'S' = separator
        bool     changed = false;
        bool isSeparator() const { return size == 'S'; }
    };

public slots:
    // RAM Search "add watch" entry point. size = 1/2/4, type = 0/1/2 (s/u/h).
    void addWatch(uint32_t addr, int size, int type);

private slots:
    void onNewList();
    void onOpen();
    void onSave();
    void onAppend();
    void onNewWatch();
    void onEditWatch();
    void onRemoveWatch();
    void onDuplicateWatch();
    void onSeparator();
    void onMoveUp();
    void onMoveDown();
    void onDoubleClicked(int row, int col);

private:
    void buildUi();
    void rebuildTable();
    void setRow(int row);
    int  selectedRow() const;
    uint32_t readValue(const WatchEntry& w) const;
    QString  valueText(const WatchEntry& w) const;
    // returns true if user accepted the dialog; fills w
    bool editDialog(WatchEntry& w, const QString& title);
    bool insertWatch(const WatchEntry& w, int index);
    bool saveFile(const QString& path);
    bool loadFile(const QString& path, bool clear);

    static bool isValidWatchAddress(uint32_t a);

    IDebugBackend*      backend_ = nullptr;
    QVector<WatchEntry> watches_;
    QString             currentFile_;
    QTableWidget*       table_ = nullptr;
};
