#include "VdpRamView.h"
#include "ViewSettings.h"
#include <QPainter>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QFile>
#include <QTimer>
#include <QFont>
#include <algorithm>
#include <cstring>

static const QString kBinFilter = QStringLiteral("Binary files (*.bin);;All files (*)");

// Gens DrawFocusRect equivalent, visible on any background.
static void drawFocusMarker(QPainter& p, const QRect& r)
{
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 1, Qt::SolidLine));
    p.drawRect(r);
    p.setPen(QPen(Qt::white, 1, Qt::DotLine));
    p.drawRect(r);
}

static void setTextIfChanged(QLabel* l, const QString& s)
{
    if (l->text() != s) l->setText(s);
}

// ---------------------------------------------------------------- canvases

VdpPalCanvas::VdpPalCanvas(VdpRamView* v) : QWidget(v), v_(v)
{
    setFixedSize(VdpRamView::kTilesInRow * VdpRamView::kPalCell,
                 4 * VdpRamView::kPalCell);                       // 256 x 64
}

void VdpPalCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    v_->paintPalette(p);
}

void VdpPalCanvas::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) v_->paletteClicked(e->position().toPoint());
}

VdpTileCanvas::VdpTileCanvas(VdpRamView* v) : QWidget(v), v_(v)
{
    setFixedWidth(VdpRamView::kTilesInRow * v->tilePx());
    setMinimumHeight(128);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void VdpTileCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    v_->paintTiles(p);
}

void VdpTileCanvas::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) v_->tilesClicked(e->position().toPoint());
}

void VdpTileCanvas::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    v_->updateScrollRange();
}

VdpTilePreview::VdpTilePreview(VdpRamView* v) : QWidget(v), v_(v)
{
    setFixedSize(128, 128);
}

void VdpTilePreview::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    v_->paintPreview(p);
}

static const QString kVdpRamGroup = QStringLiteral("VdpRam");

// -------------------------------------------------------------------- view

VdpRamView::VdpRamView(QWidget* parent) : QWidget(parent)
{
    const QFont mono(QStringLiteral("Courier New"), 9);

    palCanvas_  = new VdpPalCanvas(this);
    tileCanvas_ = new VdpTileCanvas(this);
    preview_    = new VdpTilePreview(this);

    scroll_ = new QScrollBar(Qt::Vertical, this);
    scroll_->setRange(0, kTotalRows - 1);
    connect(scroll_, &QScrollBar::valueChanged, this, &VdpRamView::onScroll);

    zoomSpin_ = new QSpinBox(this);
    zoomSpin_->setRange(1, 8);
    zoomSpin_->setValue(zoom_);
    zoomSpin_->setPrefix(QStringLiteral("x"));
    connect(zoomSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &VdpRamView::onZoom);

    vramRadio_ = new QRadioButton(QStringLiteral("VRAM"), this);
    ramRadio_  = new QRadioButton(QStringLiteral("RAM"), this);
    vramRadio_->setChecked(true);
    connect(vramRadio_, &QRadioButton::toggled, this, &VdpRamView::onModeChanged);

    dumpPalBtn_  = new QPushButton(QStringLiteral("Dump Pal"), this);
    loadPalBtn_  = new QPushButton(QStringLiteral("Load Pal"), this);
    yyPalBtn_    = new QPushButton(QStringLiteral("YY-CHR Pal"), this);
    rnbBtn_      = new QPushButton(QStringLiteral("Gray && Rnbw"), this);
    dumpVramBtn_ = new QPushButton(QStringLiteral("Dump VRAM"), this);
    loadVramBtn_ = new QPushButton(QStringLiteral("Load VRAM"), this);
    connect(dumpPalBtn_,  &QPushButton::clicked, this, &VdpRamView::onDumpPal);
    connect(loadPalBtn_,  &QPushButton::clicked, this, &VdpRamView::onLoadPal);
    connect(yyPalBtn_,    &QPushButton::clicked, this, &VdpRamView::onYyChrPal);
    connect(rnbBtn_,      &QPushButton::clicked, this, &VdpRamView::onGrayRnbw);
    connect(dumpVramBtn_, &QPushButton::clicked, this, &VdpRamView::onDumpVram);
    connect(loadVramBtn_, &QPushButton::clicked, this, &VdpRamView::onLoadVram);

    tileInfo_ = new QLabel(QStringLiteral("--"), this);
    tileInfo_->setFont(mono);
    tileInfo_->setAlignment(Qt::AlignHCenter);
    colorInfo_ = new QLabel(QStringLiteral("--"), this);
    colorInfo_->setFont(mono);
    colorInfo_->setAlignment(Qt::AlignHCenter);
    statusLabel_ = new QLabel(this);

    auto* modeGroup = new QGroupBox(QStringLiteral("Mode"), this);
    {
        auto* l = new QHBoxLayout(modeGroup);
        l->addWidget(vramRadio_);
        l->addWidget(ramRadio_);
    }
    auto* palGroup = new QGroupBox(QStringLiteral("Palette"), this);
    {
        auto* g = new QGridLayout(palGroup);
        g->addWidget(dumpPalBtn_, 0, 0);
        g->addWidget(loadPalBtn_, 0, 1);
        g->addWidget(yyPalBtn_,   1, 0);
        g->addWidget(rnbBtn_,     1, 1);
    }
    auto* vramGroup = new QGroupBox(QStringLiteral("VRAM"), this);
    {
        auto* l = new QHBoxLayout(vramGroup);
        l->addWidget(dumpVramBtn_);
        l->addWidget(loadVramBtn_);
    }

    auto* zoomRow = new QHBoxLayout;
    zoomRow->addWidget(new QLabel(QStringLiteral("Zoom:"), this));
    zoomRow->addWidget(zoomSpin_);
    zoomRow->addStretch();

    auto* rightW = new QWidget(this);
    rightW->setFixedWidth(210);
    auto* right = new QVBoxLayout(rightW);
    right->setContentsMargins(0, 0, 0, 0);
    right->addWidget(modeGroup);
    right->addWidget(palGroup);
    right->addWidget(vramGroup);
    right->addLayout(zoomRow);
    right->addWidget(preview_, 0, Qt::AlignHCenter);
    right->addWidget(tileInfo_);
    right->addWidget(colorInfo_);
    right->addWidget(statusLabel_);
    right->addStretch();

    auto* tilesRow = new QHBoxLayout;
    tilesRow->setSpacing(1);
    tilesRow->addWidget(tileCanvas_);
    tilesRow->addWidget(scroll_);
    tilesRow->addStretch();

    auto* left = new QVBoxLayout;
    left->addWidget(palCanvas_, 0, Qt::AlignLeft);
    left->addLayout(tilesRow, 1);

    auto* main = new QHBoxLayout(this);
    main->setContentsMargins(4, 4, 4, 4);
    main->addLayout(left, 1);
    main->addWidget(rightW);

    zoomSpin_->setValue(qBound(1, viewsettings::getInt(kVdpRamGroup, QStringLiteral("zoom"), zoom_), 8));
    if (!viewsettings::getBool(kVdpRamGroup, QStringLiteral("vram"), true))
        ramRadio_->setChecked(true);

    setControlsEnabled(false);
}

VdpRamView::~VdpRamView()
{
    viewsettings::putInt(kVdpRamGroup,  QStringLiteral("zoom"), zoom_);
    viewsettings::putBool(kVdpRamGroup, QStringLiteral("vram"), isVram_);
}

void VdpRamView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    ramRegionId_ = vramRegionId_ = cramRegionId_ = -1;
    if (backend_) resolveRegions();
    setControlsEnabled(backend_ != nullptr);
    refresh();
}

void VdpRamView::resolveRegions()
{
    for (const MemRegion& r : backend_->getMemRegions()) {
        if      (r.name == "RAM 68K") ramRegionId_  = r.id;
        else if (r.name == "VRAM")    vramRegionId_ = r.id;
        else if (r.name == "CRAM")    cramRegionId_ = r.id;
    }
}

void VdpRamView::refresh()
{
    if (backend_ && (ramRegionId_ < 0 || vramRegionId_ < 0 || cramRegionId_ < 0))
        resolveRegions();
    pullData();
    updateScrollRange();
    updateInfo();
    repaintCanvases();
}

void VdpRamView::pullData()
{
    tiles_.clear();
    std::memset(cram_, 0, sizeof cram_);
    if (!backend_) return;

    VdpState v = backend_->getVdpState();
    if (v.cram)
        for (int i = 0; i < 64; ++i)
            cram_[i] = static_cast<uint16_t>(v.cram[i * 2] | (v.cram[i * 2 + 1] << 8));

    if (isVram_) {
        if (v.vram) {
            tiles_.resize(0x10000);
            for (uint32_t a = 0; a < 0x10000; ++a)      // un-swap to logical order
                tiles_[a] = v.vram[a ^ 1];
        }
    } else if (ramRegionId_ >= 0) {
        tiles_ = backend_->readRegion(ramRegionId_, 0, 0x10000);
    }
}

void VdpRamView::updateScrollRange()
{
    if (!scroll_ || !tileCanvas_) return;
    const int visRows = std::max(1, tileCanvas_->height() / tilePx());
    scroll_->setRange(0, std::max(0, kTotalRows - visRows));
    scroll_->setPageStep(visRows);
    scroll_->setSingleStep(1);
}

void VdpRamView::updateInfo()
{
    if (!backend_) {
        setTextIfChanged(tileInfo_,  QStringLiteral("--"));
        setTextIfChanged(colorInfo_, QStringLiteral("--"));
        return;
    }
    // Gens format: "Offset: %04X  Id: %03X"; RAM mode ORs the 68k base in.
    const unsigned off = static_cast<unsigned>(tile_ * kTileBytes) | (isVram_ ? 0 : 0xFF0000);
    setTextIfChanged(tileInfo_,
        QString::asprintf("Offset: %04X\nId: %03X\nSize: 0x20 bytes", off, tile_));

    const uint16_t raw = cram_[(palRow_ * 16 + palCol_) & 63];
    const QRgb c = colorAt(palRow_ * 16 + palCol_);
    setTextIfChanged(colorInfo_,
        QString::asprintf("Pal %d  Col %d\nCRAM: %04X  RGB: %02X%02X%02X",
                          palRow_, palCol_, raw, qRed(c), qGreen(c), qBlue(c)));
}

void VdpRamView::setControlsEnabled(bool on)
{
    for (QWidget* w : std::initializer_list<QWidget*>{
             vramRadio_, ramRadio_, dumpPalBtn_, loadPalBtn_, yyPalBtn_,
             rnbBtn_, dumpVramBtn_, loadVramBtn_ })
        w->setEnabled(on);
}

void VdpRamView::repaintCanvases()
{
    palCanvas_->update();
    tileCanvas_->update();
    preview_->update();
}

void VdpRamView::setStatus(const QString& msg)
{
    statusLabel_->setText(msg);
    QTimer::singleShot(2000, this, [this] { statusLabel_->clear(); });
}

QRgb VdpRamView::colorAt(int idx) const
{
    int r, g, b;
    cram_to_rgb(cram_[idx & 63], r, g, b);
    return qRgb(r, g, b);
}

// -------------------------------------------------------------- rendering

void VdpRamView::paintPalette(QPainter& p)
{
    p.fillRect(palCanvas_->rect(), QColor(0x1E, 0x1E, 0x1E));
    if (!backend_) return;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 16; ++x)
            p.fillRect(x * kPalCell, y * kPalCell, kPalCell, kPalCell,
                       QColor(colorAt(y * 16 + x)));
    // selected color cell, then Gens-style focus rect around the whole row
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 1, Qt::SolidLine));
    p.drawRect(palCol_ * kPalCell + 1, palRow_ * kPalCell + 1, kPalCell - 3, kPalCell - 3);
    p.setPen(QPen(Qt::white, 1, Qt::SolidLine));
    p.drawRect(palCol_ * kPalCell + 2, palRow_ * kPalCell + 2, kPalCell - 5, kPalCell - 5);
    drawFocusMarker(p, QRect(0, palRow_ * kPalCell,
                             kTilesInRow * kPalCell - 1, kPalCell - 1));
}

void VdpRamView::paintTiles(QPainter& p)
{
    p.fillRect(tileCanvas_->rect(), QColor(0x1E, 0x1E, 0x1E));
    if (tiles_.empty()) return;

    const int startRow = scroll_->value();
    const int rows = std::min((tileCanvas_->height() + tilePx() - 1) / tilePx(),
                              kTotalRows - startRow);
    if (rows <= 0) return;

    QRgb pal[16];
    for (int i = 0; i < 16; ++i) pal[i] = colorAt(palRow_ * 16 + i);

    QImage img(kTilesInRow * 8, rows * 8, QImage::Format_RGB32);
    img.fill(QColor(0x1E, 0x1E, 0x1E));
    const int avail = static_cast<int>(tiles_.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < kTilesInRow; ++c) {
            const int base = ((startRow + r) * kTilesInRow + c) * kTileBytes;
            if (base + kTileBytes > avail) continue;
            for (int y = 0; y < 8; ++y) {
                QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(r * 8 + y)) + c * 8;
                for (int x = 0; x < 4; ++x) {
                    const uint8_t b = tiles_[base + y * 4 + x];
                    line[x * 2]     = pal[b >> 4];   // high nibble = left pixel
                    line[x * 2 + 1] = pal[b & 0xF];  // index 0 opaque in this viewer
                }
            }
        }
    }
    p.drawImage(QRect(0, 0, img.width() * zoom_, img.height() * zoom_), img);

    const int selRow = tile_ / kTilesInRow - startRow;
    if (selRow >= 0 && selRow < rows)
        drawFocusMarker(p, QRect((tile_ % kTilesInRow) * tilePx(), selRow * tilePx(),
                                 tilePx() - 1, tilePx() - 1));
}

void VdpRamView::paintPreview(QPainter& p)
{
    p.fillRect(preview_->rect(), QColor(0x1E, 0x1E, 0x1E));
    const int base = tile_ * kTileBytes;
    if (tiles_.empty() || base + kTileBytes > static_cast<int>(tiles_.size())) return;

    QRgb pal[16];
    for (int i = 0; i < 16; ++i) pal[i] = colorAt(palRow_ * 16 + i);

    QImage img(8, 8, QImage::Format_RGB32);
    for (int y = 0; y < 8; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < 4; ++x) {
            const uint8_t b = tiles_[base + y * 4 + x];
            line[x * 2]     = pal[b >> 4];
            line[x * 2 + 1] = pal[b & 0xF];
        }
    }
    const int zoom = std::max(1, std::min(preview_->width() / 8, preview_->height() / 8));
    const QRect dst((preview_->width()  - 8 * zoom) / 2,
                    (preview_->height() - 8 * zoom) / 2, 8 * zoom, 8 * zoom);
    p.drawImage(dst, img);
    p.setPen(QColor(0x50, 0x50, 0x50));
    p.setBrush(Qt::NoBrush);
    p.drawRect(dst.adjusted(-1, -1, 0, 0));
}

// ------------------------------------------------------------ interaction

void VdpRamView::paletteClicked(const QPoint& pos)
{
    if (!backend_) return;
    palRow_ = std::clamp(pos.y() / kPalCell, 0, 3);
    palCol_ = std::clamp(pos.x() / kPalCell, 0, 15);
    updateInfo();
    repaintCanvases();
}

void VdpRamView::tilesClicked(const QPoint& pos)
{
    if (tiles_.empty()) return;
    const int col = pos.x() / tilePx();
    if (col >= kTilesInRow) return;
    const int row = pos.y() / tilePx() + scroll_->value();
    const int t = row * kTilesInRow + col;
    if (t < 0 || t >= kTotalTiles) return;
    tile_ = t;
    updateInfo();
    tileCanvas_->update();
    preview_->update();
}

void VdpRamView::onScroll(int)   { tileCanvas_->update(); }

void VdpRamView::onZoom(int z)
{
    zoom_ = z;
    tileCanvas_->setFixedWidth(kTilesInRow * tilePx());
    updateScrollRange();
    tileCanvas_->update();
}

void VdpRamView::onModeChanged()
{
    const bool v = vramRadio_->isChecked();
    if (v == isVram_) return;
    isVram_ = v;                    // selected tile/palette kept, as in Gens
    refresh();
}

// -------------------------------------------------------------- dump/load

bool VdpRamView::saveFile(const QString& caption, const QString& defName,
                          const QString& filter, const uint8_t* bytes, int size)
{
    const QString fn = QFileDialog::getSaveFileName(this, caption, defName, filter);
    if (fn.isEmpty()) return false;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly)) {
        setStatus(QStringLiteral("Save failed"));
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes), size);
    return true;
}

std::vector<uint8_t> VdpRamView::openFile(const QString& caption, const QString& defName,
                                          const QString& filter, int size)
{
    const QString fn = QFileDialog::getOpenFileName(this, caption, defName, filter);
    if (fn.isEmpty()) return {};
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("Open failed"));
        return {};
    }
    const QByteArray ba = f.read(size);
    if (ba.size() < size) {
        setStatus(QStringLiteral("File too small"));
        return {};
    }
    const auto* d = reinterpret_cast<const uint8_t*>(ba.constData());
    return std::vector<uint8_t>(d, d + size);
}

void VdpRamView::onDumpPal()
{
    // Logical big-endian word order, like the Gens dump.
    // Same byte order the CRAM region uses, so a dump reloads unchanged.
    uint8_t buf[0x80];
    for (int i = 0; i < 64; ++i) {
        buf[i * 2]     = static_cast<uint8_t>(cram_[i] & 0xFF);
        buf[i * 2 + 1] = static_cast<uint8_t>(cram_[i] >> 8);
    }
    if (saveFile(QStringLiteral("Dump Palette"), QStringLiteral("pal.bin"),
                 kBinFilter, buf, sizeof buf))
        setStatus(QStringLiteral("Palette dumped"));
}

void VdpRamView::onLoadPal()
{
    const auto bytes = openFile(QStringLiteral("Load Palette"), QStringLiteral("pal.bin"),
                                kBinFilter, 0x80);
    if (bytes.empty()) return;
    if (!backend_ || cramRegionId_ < 0) {
        setStatus(QStringLiteral("CRAM region unavailable"));
        return;
    }
    if (backend_->writeRegion(cramRegionId_, 0, bytes.data(), 0x80)) {
        setStatus(QStringLiteral("Palette loaded"));
        refresh();
    } else {
        setStatus(QStringLiteral("CRAM write failed"));
    }
}

void VdpRamView::onYyChrPal()
{
    // 256 x 3-byte RGB entries; only the first 64 are real, the rest zeroed.
    uint8_t buf[768] = {};
    for (int i = 0; i < 64; ++i) {
        const QRgb c = colorAt(i);
        buf[i * 3]     = static_cast<uint8_t>(qRed(c));
        buf[i * 3 + 1] = static_cast<uint8_t>(qGreen(c));
        buf[i * 3 + 2] = static_cast<uint8_t>(qBlue(c));
    }
    if (saveFile(QStringLiteral("Dump YY-CHR Palette"), QStringLiteral("pal.pal"),
                 QStringLiteral("YY-CHR palette (*.pal);;All files (*)"), buf, sizeof buf))
        setStatus(QStringLiteral("YY-CHR palette dumped"));
}

void VdpRamView::onGrayRnbw()
{
    if (!backend_ || cramRegionId_ < 0) {
        setStatus(QStringLiteral("CRAM region unavailable"));
        return;
    }
    // Lines 0-2 only (line 3 untouched): ascending gray, descending gray, rainbow.
    uint8_t buf[96];
    auto putColor = [&buf](int color, uint16_t packed) {   // CRAM region order
        buf[color * 2]     = static_cast<uint8_t>(packed & 0xFF);
        buf[color * 2 + 1] = static_cast<uint8_t>(packed >> 8);
    };
    for (int i = 0; i < 16; ++i) {
        const int v = (i & 7) << 5;                        // ascending gray
        putColor(i, rgb_to_cram(v, v, v));
    }
    for (int i = 0; i < 16; ++i) {
        const int v = ((15 - i) & 7) << 5;                 // descending gray
        putColor(16 + i, rgb_to_cram(v, v, v));
    }
    for (int i = 0; i < 16; ++i) {
        const QColor c = QColor::fromHsvF(float(i) / 16.0f, 0.85f, 1.0f);
        putColor(32 + i, rgb_to_cram(c.red(), c.green(), c.blue()));
    }
    if (backend_->writeRegion(cramRegionId_, 0, buf, sizeof buf)) {
        setStatus(QStringLiteral("Gray/rainbow palette set"));
        refresh();
    } else {
        setStatus(QStringLiteral("CRAM write failed"));
    }
}

void VdpRamView::onDumpVram()
{
    std::vector<uint8_t> bytes;
    if (backend_ && vramRegionId_ >= 0)
        bytes = backend_->readRegion(vramRegionId_, 0, 0x10000);
    if (bytes.size() < 0x10000 && backend_) {
        const VdpState v = backend_->getVdpState();
        if (v.vram) {
            bytes.resize(0x10000);
            for (uint32_t a = 0; a < 0x10000; ++a) bytes[a] = v.vram[a ^ 1];
        }
    }
    if (bytes.size() < 0x10000) {
        setStatus(QStringLiteral("No VRAM data"));
        return;
    }
    if (saveFile(QStringLiteral("Dump VRAM"), QStringLiteral("vram.bin"),
                 kBinFilter, bytes.data(), 0x10000))
        setStatus(QStringLiteral("VRAM dumped"));
}

void VdpRamView::onLoadVram()
{
    const auto bytes = openFile(QStringLiteral("Load VRAM"), QStringLiteral("vram.bin"),
                                kBinFilter, 0x10000);
    if (bytes.empty()) return;
    if (!backend_ || vramRegionId_ < 0) {
        setStatus(QStringLiteral("VRAM region unavailable"));
        return;
    }
    if (backend_->writeRegion(vramRegionId_, 0, bytes.data(), 0x10000)) {
        setStatus(QStringLiteral("VRAM loaded"));
        refresh();
    } else {
        setStatus(QStringLiteral("VRAM write failed"));
    }
}
