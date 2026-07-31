#pragma once
#include <QWidget>
#include <QAbstractTableModel>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QTableView;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QRadioButton;
QT_END_NAMESPACE

// Port of the Gens RAM Search tool over 68k work RAM (region "RAM 68K").
// Sizes 1/2/4 (+misaligned), signed/unsigned/hex display, comparison operators
// (<, >, <=, >=, ==, !=, different-by N, modulo N == X) against previous
// value / specific value / specific address / number of changes.
// Undo is one level deep (the original kept a deeper stack).
class RamSearchModel : public QAbstractTableModel {
    Q_OBJECT
public:
    struct Entry { uint32_t off; uint32_t cur; uint32_t prev; uint32_t changes; };

    explicit RamSearchModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& = QModelIndex()) const override { return (int)entries_.size(); }
    int columnCount(const QModelIndex& = QModelIndex()) const override { return 4; }
    QVariant data(const QModelIndex& idx, int role) const override;
    QVariant headerData(int s, Qt::Orientation o, int role) const override;

    std::vector<Entry> entries_;
    int  valueSize_ = 1;        // 1/2/4
    int  dispType_  = 0;        // 0=signed 1=unsigned 2=hex
    void resetAll() { beginResetModel(); endResetModel(); }
    QString format(uint32_t v) const;
};

class RamSearchView : public QWidget {
    Q_OBJECT
public:
    explicit RamSearchView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b) { backend_ = b; }
    void refresh();

signals:
    void addWatchRequested(uint32_t addr, int size, int type);

private slots:
    void onSearch();
    void onReset();
    void onClearChanges();
    void onUndo();
    void onEliminate();
    void onAddWatch();
    void onSizeOrAlignChanged();

private:
    void buildUi();
    void snapshot(std::vector<uint8_t>& out) const;
    uint32_t valueAt(const std::vector<uint8_t>& mem, uint32_t off) const;
    int64_t  asSigned(uint32_t v) const;
    bool     matches(const RamSearchModel::Entry& e) const;
    void     rebuildAll();

    IDebugBackend* backend_ = nullptr;
    RamSearchModel* model_  = nullptr;
    std::vector<RamSearchModel::Entry> undo_;
    std::vector<uint8_t> mem_;      // last snapshot of the region
    uint32_t regionBase_ = 0xFF0000;
    int      regionId_   = 1;       // "RAM 68K"
    uint32_t regionSize_ = 0x10000;

    QTableView*   table_    = nullptr;
    QComboBox*    cmpOp_    = nullptr;
    QRadioButton* cmpPrev_  = nullptr;
    QRadioButton* cmpValue_ = nullptr;
    QRadioButton* cmpAddr_  = nullptr;
    QRadioButton* cmpChanges_ = nullptr;
    QLineEdit*    cmpValueEdit_ = nullptr;
    QLineEdit*    opParamEdit_  = nullptr;  // N for different-by / "N,X" for modulo
    QComboBox*    sizeBox_  = nullptr;
    QComboBox*    typeBox_  = nullptr;
    QCheckBox*    misaligned_ = nullptr;
    QCheckBox*    autoSearch_ = nullptr;
};
