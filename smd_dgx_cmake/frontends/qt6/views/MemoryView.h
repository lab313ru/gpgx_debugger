#pragma once
#include <QAbstractScrollArea>
#include <cstdint>
#include <vector>
#include "debugger/DebugState.h"

class IDebugBackend;
QT_BEGIN_NAMESPACE
class QComboBox;
class QCheckBox;
class QLabel;
QT_END_NAMESPACE

// Full hex editor, functional port of the Gens r57shell-mod hex editor.
// Region selector, 16 bytes/row hex grid + ASCII column, in-place editing
// (hex nibbles / chars), selection, copy/paste, goto, dump to file,
// symbolic names for register regions. All data access goes through
// IDebugBackend::readRegion/writeRegion (logical byte order).
class MemoryView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit MemoryView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b);
    void refresh();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    enum class Area { None, Hex, Text };

    struct Metrics {
        int cw = 0, rh = 0, gap = 0, cellW = 0;
        int hexL = 0, hexR = 0, textL = 0, textR = 0, top = 0;
    };
    Metrics metrics() const;
    int  cellX(const Metrics& m, int col) const;
    int  textX(const Metrics& m, int col) const;

    void populateRegions();
    void applyRegion(int idx);
    void onRegionChanged(int idx);
    void onScroll(int value);
    void clampSelection();
    void refreshVisible();
    void updateScrollRange();
    int  visibleRows() const;
    uint32_t totalRows() const;
    uint8_t byteAt(uint32_t off, bool& ok) const;

    int64_t selStart() const;
    int64_t selEnd() const;
    void moveCursor(int64_t off, bool extend);
    void ensureVisible(int64_t off);
    bool hitTest(QPoint pos, int64_t& off, Area& area, bool clampInside) const;

    void writeBytes(uint32_t off, const uint8_t* data, uint32_t n);
    void typeHexDigit(int v);
    void typeChar(char c);
    void copyAuto();
    void copyNumbers();
    void copyChars();
    void copyAddress();
    void pasteAuto();
    void pasteNumbers();
    void pasteChars();
    void gotoDialog();
    void dumpToFile();

    QString symbolAt(int64_t off) const;
    QString displayAddr(int64_t off) const;
    void updateStatus();

    IDebugBackend* backend_ = nullptr;
    std::vector<MemRegion> regions_;
    MemRegion cur_{};
    bool hasRegion_ = false;

    std::vector<uint8_t> vis_;      // cache of visible bytes only
    uint32_t visFirst_ = 0;         // region offset of first visible row

    bool    hasSel_ = false;
    int64_t selAnchor_ = 0;         // selection anchor (fixed end)
    int64_t selLast_ = 0;           // moving end / cursor
    bool    mouseHeld_ = false;
    Area    editArea_ = Area::Hex;  // typing / auto copy-paste target
    int64_t hoverOff_ = -1;

    bool    pendingSecond_ = false; // half-typed hex byte
    uint8_t pendingNibble_ = 0;

    QWidget*   bar_ = nullptr;
    QComboBox* regionCombo_ = nullptr;
    QCheckBox* textChk_ = nullptr;
    QCheckBox* linesChk_ = nullptr;
    QLabel*    status_ = nullptr;
};
