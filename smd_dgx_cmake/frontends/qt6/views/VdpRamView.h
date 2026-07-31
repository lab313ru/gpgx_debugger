#pragma once
#include <QWidget>
#include <QColor>
#include <QScrollBar>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE
class VdpRamView;

// Internal paint surfaces (plain QWidget children, no signals).
class VdpPalCanvas : public QWidget {
public:
    explicit VdpPalCanvas(VdpRamView* v);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    VdpRamView* v_;
};

class VdpTileCanvas : public QWidget {
public:
    explicit VdpTileCanvas(VdpRamView* v);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    VdpRamView* v_;
};

class VdpTilePreview : public QWidget {
public:
    explicit VdpTilePreview(VdpRamView* v);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    VdpRamView* v_;
};

// VDP RAM viewer – port of the Gens "VDP Ram" window (palette grid, tile
// browser, zoomed tile preview, dump/load, VRAM-vs-68K-RAM tile source).
// The register tabs of the original live in VdpRegView.
class VdpRamView : public QWidget {
    Q_OBJECT
public:
    explicit VdpRamView(QWidget* parent = nullptr);
    ~VdpRamView() override;                 // persists zoom / VRAM-vs-RAM
    void setBackend(IDebugBackend* b);
    void refresh();
    QSize sizeHint() const override { return {520, 480}; }

private slots:
    void onScroll(int);
    void onZoom(int z);
    void onModeChanged();
    void onDumpPal();
    void onLoadPal();
    void onYyChrPal();
    void onGrayRnbw();
    void onDumpVram();
    void onLoadVram();

private:
    friend class VdpPalCanvas;
    friend class VdpTileCanvas;
    friend class VdpTilePreview;

    static constexpr int kTilesInRow = 16;
    static constexpr int kTileBytes  = 0x20;
    static constexpr int kTotalTiles = 0x10000 / kTileBytes;      // 0x800
    static constexpr int kTotalRows  = kTotalTiles / kTilesInRow; // 128
    static constexpr int kPalCell    = 16;

    void resolveRegions();
    void pullData();
    void updateScrollRange();
    void updateInfo();
    void setControlsEnabled(bool on);
    void repaintCanvases();
    void setStatus(const QString& msg);

    QRgb colorAt(int idx) const;                 // 0..63, default Genesis decode
    int  tilePx() const { return 8 * zoom_; }

    void paintPalette(QPainter& p);
    void paintTiles(QPainter& p);
    void paintPreview(QPainter& p);
    void paletteClicked(const QPoint& pos);
    void tilesClicked(const QPoint& pos);

    bool                 saveFile(const QString& caption, const QString& defName,
                                  const QString& filter, const uint8_t* bytes, int size);
    std::vector<uint8_t> openFile(const QString& caption, const QString& defName,
                                  const QString& filter, int size);

    IDebugBackend* backend_ = nullptr;
    int ramRegionId_ = -1, vramRegionId_ = -1, cramRegionId_ = -1;

    std::vector<uint8_t> tiles_;      // active tile source, logical byte order
    uint16_t             cram_[64] = {};

    int  palRow_ = 0, palCol_ = 0;    // selected palette line / color
    int  tile_   = 0;                 // selected tile index, absolute
    bool isVram_ = true;
    int  zoom_   = 2;

    VdpPalCanvas*   palCanvas_   = nullptr;
    VdpTileCanvas*  tileCanvas_  = nullptr;
    VdpTilePreview* preview_     = nullptr;
    QScrollBar*     scroll_      = nullptr;
    QSpinBox*       zoomSpin_    = nullptr;
    QLabel*         colorInfo_   = nullptr;
    QLabel*         tileInfo_    = nullptr;
    QLabel*         statusLabel_ = nullptr;
    QRadioButton*   vramRadio_   = nullptr;
    QRadioButton*   ramRadio_    = nullptr;
    QPushButton*    dumpPalBtn_  = nullptr;
    QPushButton*    loadPalBtn_  = nullptr;
    QPushButton*    yyPalBtn_    = nullptr;
    QPushButton*    rnbBtn_      = nullptr;
    QPushButton*    dumpVramBtn_ = nullptr;
    QPushButton*    loadVramBtn_ = nullptr;
};
