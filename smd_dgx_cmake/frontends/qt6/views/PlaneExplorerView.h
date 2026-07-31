#pragma once
#include <QWidget>
#include <QImage>
#include <vector>
#include <cstdint>
#include "debugger/DebugState.h"
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QRadioButton;
class QCheckBox;
class QSpinBox;
class QLabel;
class QScrollArea;
class QMouseEvent;
class QWheelEvent;
QT_END_NAMESPACE

// Pixel canvas for PlaneExplorerView: 1024x1024 indexed plane image inside a
// QScrollArea. Tracks the cursor, draws a dashed hover highlight plus a solid
// one for the locked tile, and scales by an integer zoom (nearest, so pixels
// stay pixels).
//
// Navigation: middle-drag pans (left-click is taken by locking), Ctrl+wheel
// zooms, plain wheel is left to the scroll area.
class PlaneExplorerCanvas : public QWidget {
    Q_OBJECT
public:
    explicit PlaneExplorerCanvas(QWidget* parent = nullptr);
    void setImage(const QImage& img);
    void clearImage();
    void setZoom(int z);
    int  zoom() const { return zoom_; }
    void setHighlight(const QRect& r);
    void setLockHighlight(const QRect& r);
signals:
    void hoverMoved(int x, int y);          // image-space pixel; (-1,-1) on leave
    void clicked(int x, int y);             // image-space pixel
    void panRequested(int dx, int dy);      // pixels to scroll by
    void zoomRequested(int steps);          // +1 in, -1 out
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    bool  toImage(const QPoint& p, int* x, int* y) const;
    void  emitHover(const QPoint& p);
    QImage image_;
    QRect  highlight_;
    QRect  lockHighlight_;
    int    zoom_ = 1;
    bool   panning_ = false;
    QPoint panOrigin_;
};

// Port of Gens KMod "Plane Explorer" (plane_explorer_kmod.cpp).
// Read-only viewer for planes A/B/Window and the sprite layer.
class PlaneExplorerView : public QWidget {
    Q_OBJECT
public:
    explicit PlaneExplorerView(QWidget* parent = nullptr);
    ~PlaneExplorerView() override;          // persists plane / zoom / transparency
    void setBackend(IDebugBackend* b);
    void refresh();
private slots:
    void onPlane(int idx);
    void onTransparency(bool on);
    void onZoom(int z);
    void onZoomSteps(int steps);
    void onPan(int dx, int dy);
    void onHover(int x, int y);
    void onClicked(int x, int y);
private:
    // Snapshot access: VRAM logical byte at A lives at [A^1]; CRAM entries 16-bit LE.
    uint8_t  vb(uint32_t a) const { return vram_[(a & 0xFFFFu) ^ 1u]; }
    uint16_t vw(uint32_t a) const {
        return uint16_t((vram_[(a & 0xFFFFu) ^ 1u] << 8) | vram_[((a + 1) & 0xFFFFu) ^ 1u]);
    }
    uint16_t cw(int idx) const { return uint16_t(cram_[idx * 2] | (cram_[idx * 2 + 1] << 8)); }

    struct Sprite {
        int  no, xpos, ypos, wCells, hCells, link, pal, block;
        bool prio, hf, vf;
    };

    void decodeMode();
    void buildColorTable();
    void rebuild();
    void drawTile(uint16_t entry, int tx, int ty, int transColor);
    void drawSprite(const Sprite& s);
    Sprite readSprite(int no) const;
    std::vector<int> spriteLinkOrder() const;
    void clearHover();
    void updateControls();
    // Fills the info panel and returns the rect to highlight; empty rect = miss.
    QRect describeAt(int x, int y);

    IDebugBackend* backend_ = nullptr;
    bool           hasData_ = false;

    uint8_t vram_[0x10000] {};
    uint8_t cram_[0x80] {};
    uint8_t regs_[0x20] {};

    std::vector<uint8_t> buf_;              // 1024x1024 8bpp, stride 1024
    QList<QRgb>          colorTable_;       // 0-63 CRAM, 253/254/255 checker grays

    // decoded on each rebuild
    int      tileH_  = 8;
    bool     h40_    = false, im2_ = false;
    uint32_t base_   = 0;
    int      planeW_ = 32, planeH_ = 32;    // cells

    // tool state (statics in the original; persist for widget lifetime)
    int  plane_     = 0;                    // 0=A 1=B 2=Window 3=Sprites
    bool showTrans_ = false;
    int  zoom_      = 1;

    QRect spriteRect_;                      // stale sprite highlight persists on miss

    // Clicking pins the readout to one tile so it can be studied (and the
    // emulator stepped) without the mouse having to stay put — the same
    // behaviour VDP Ram has. Clicking the pinned tile again releases it.
    bool  locked_    = false;
    QRect lockedRect_;
    int   lockedX_ = 0, lockedY_ = 0;

    QRadioButton*        radios_[4] {};
    QCheckBox*           transCheck_ = nullptr;
    QSpinBox*            zoomSpin_   = nullptr;
    QLabel*              infoLabel_  = nullptr;
    QScrollArea*         scrollArea_ = nullptr;
    PlaneExplorerCanvas* canvas_     = nullptr;
};
