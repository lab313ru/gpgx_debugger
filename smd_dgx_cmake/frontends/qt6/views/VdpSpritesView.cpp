#include "VdpSpritesView.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollBar>
#include <QPainter>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QSignalBlocker>
#include <QFont>
#include <algorithm>

static void drawChecker(QPainter& p, const QRect& r)
{
    p.fillRect(r, QColor(0x26, 0x26, 0x26));
    const int cs = 6;
    for (int y = 0; y * cs < r.height(); ++y)
        for (int x = 0; x * cs < r.width(); ++x)
            if ((x ^ y) & 1)
                p.fillRect(QRect(r.x() + x*cs, r.y() + y*cs, cs, cs).intersected(r),
                           QColor(0x33, 0x33, 0x33));
}

// Zoomed preview of the selected sprite (integer zoom to fit, nearest-neighbor).
class VdpSpritePreview : public QWidget {
public:
    explicit VdpSpritePreview(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(160, 160); }
    void setSprite(const QImage& img) { img_ = img; update(); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x1E, 0x1E, 0x1E));
        if (img_.isNull()) return;
        const int zoom = std::max(1, std::min(width() / img_.width(), height() / img_.height()));
        const QRect dst((width()  - img_.width()  * zoom) / 2,
                        (height() - img_.height() * zoom) / 2,
                        img_.width() * zoom, img_.height() * zoom);
        drawChecker(p, dst);
        p.drawImage(dst, img_);
    }
private:
    QImage img_;
};

// The same sprite rendered with each of the 4 palette lines.
class VdpSpritePalStrip : public QWidget {
public:
    explicit VdpSpritePalStrip(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(84); }
    void setSprites(const QImage imgs[4], int activePal)
    {
        for (int i = 0; i < 4; ++i) imgs_[i] = imgs[i];
        active_ = activePal;
        update();
    }
    void clear()
    {
        for (auto& i : imgs_) i = QImage();
        active_ = -1;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x1E, 0x1E, 0x1E));
        if (imgs_[0].isNull()) return;
        p.setFont(QFont(QStringLiteral("Courier New"), 8));
        const int cw = width() / 4;
        for (int i = 0; i < 4; ++i) {
            const QRect cell(i * cw, 0, cw, height());
            const QImage& img = imgs_[i];
            const int zoom = std::max(1, std::min((cell.width() - 6) / img.width(),
                                                  (cell.height() - 16) / img.height()));
            const QRect dst(cell.x() + (cell.width() - img.width() * zoom) / 2,
                            13 + (cell.height() - 13 - img.height() * zoom) / 2,
                            img.width() * zoom, img.height() * zoom);
            drawChecker(p, dst);
            p.drawImage(dst, img);
            p.setPen(i == active_ ? QColor(0x66, 0xB2, 0xFF) : QColor(0x80, 0x80, 0x80));
            p.drawText(cell.adjusted(3, 1, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                       QStringLiteral("Pal %1").arg(i));
            if (i == active_) p.drawRect(cell.adjusted(0, 0, -1, -1));
        }
    }
private:
    QImage imgs_[4];
    int    active_ = -1;
};

VdpSpritesView::VdpSpritesView(QWidget* parent) : QWidget(parent)
{
    table_ = new QTableWidget(0, 8, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Num"),  QStringLiteral("Ypos"),
                                       QStringLiteral("Xpos"), QStringLiteral("Size"),
                                       QStringLiteral("Link"), QStringLiteral("Pal"),
                                       QStringLiteral("Tile"), QStringLiteral("Flags*")});
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(18);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setFont(QFont(QStringLiteral("Courier New"), 9));
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(0, 42);  table_->setColumnWidth(1, 92);
    table_->setColumnWidth(2, 92);  table_->setColumnWidth(3, 100);
    table_->setColumnWidth(4, 44);  table_->setColumnWidth(5, 34);
    table_->setColumnWidth(6, 46);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &VdpSpritesView::onSelectionChanged);

    infoLabel_  = new QLabel(QStringLiteral("--"), this);
    infoLabel_->setFont(QFont(QStringLiteral("Courier New"), 9));
    chainCheck_ = new QCheckBox(QStringLiteral("Follow link chain"), this);
    chainCheck_->setChecked(true);
    connect(chainCheck_, &QCheckBox::toggled, this, &VdpSpritesView::onChainToggled);
    dumpBtn_ = new QPushButton(QStringLiteral("Dump"), this);
    dumpBtn_->setEnabled(false);
    connect(dumpBtn_, &QPushButton::clicked, this, &VdpSpritesView::onDump);

    preview_  = new VdpSpritePreview(this);
    palStrip_ = new VdpSpritePalStrip(this);
    auto* legend = new QLabel(QStringLiteral("* Priority / VFlip / HFlip"), this);
    statusLabel_ = new QLabel(this);

    auto* rightW = new QWidget(this);
    rightW->setFixedWidth(212);
    auto* right = new QVBoxLayout(rightW);
    right->setContentsMargins(0, 0, 0, 0);
    right->addWidget(preview_, 1);
    right->addWidget(palStrip_);
    right->addWidget(legend);
    right->addWidget(statusLabel_);

    auto* top = new QHBoxLayout;
    top->addWidget(infoLabel_);
    top->addStretch();
    top->addWidget(chainCheck_);
    top->addWidget(dumpBtn_);

    auto* mid = new QHBoxLayout;
    mid->addWidget(table_, 1);
    mid->addWidget(rightW);

    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(4, 4, 4, 4);
    main->addLayout(top);
    main->addLayout(mid, 1);
}

void VdpSpritesView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    refresh();
}

void VdpSpritesView::refresh()
{
    vram_.clear(); cram_.clear(); sat_.clear();
    reg5_ = 0; h40_ = false; satBase_ = 0;
    if (backend_) {
        VdpState v = backend_->getVdpState();
        if (v.vram) vram_.assign(v.vram, v.vram + 0x10000);
        if (v.cram) cram_.assign(v.cram, v.cram + 0x80);
        if (v.sat)  sat_.assign(v.sat,  v.sat  + 0x400);
        reg5_ = v.reg[5];
        h40_  = v.reg[12] & 0x01;
        satBase_ = static_cast<uint32_t>(reg5_ & (h40_ ? 0x7E : 0x7F)) << 9;
    }
    const bool have = !sat_.empty() || !vram_.empty();
    table_->setEnabled(have);
    chainCheck_->setEnabled(have);
    rebuildList();
    dumpBtn_->setEnabled(!sprites_.empty());
    updatePreviews();
}

uint16_t VdpSpritesView::satWord(int spriteIdx, int word) const
{
    // Logical Genesis byte A lives at ptr[A^1]; a big-endian VDP word at even
    // offset therefore reads as little-endian u16: lo=ptr[off], hi=ptr[off+1].
    const uint32_t off = static_cast<uint32_t>(spriteIdx) * 8 + word * 2;
    if (off + 1 < sat_.size())
        return static_cast<uint16_t>(sat_[off] | (sat_[off + 1] << 8));
    if (!vram_.empty()) {
        const uint32_t a = (satBase_ + off) & 0xFFFF;
        return static_cast<uint16_t>(vram_[a] | (vram_[(a + 1) & 0xFFFF] << 8));
    }
    return 0;
}

VdpSpritesView::Sprite VdpSpritesView::decodeSprite(int idx) const
{
    const uint16_t w0 = satWord(idx, 0), w1 = satWord(idx, 1);
    const uint16_t w2 = satWord(idx, 2), w3 = satWord(idx, 3);
    Sprite s;
    s.num   = idx;
    s.rawY  = w0 & 0x03FF;
    s.wPx   = (((w1 >> 10) & 3) + 1) * 8;
    s.hPx   = (((w1 >>  8) & 3) + 1) * 8;
    s.link  = w1 & 0x007F;
    s.prio  = (w2 & 0x8000) != 0;
    s.pal   = (w2 >> 13) & 3;
    s.vflip = (w2 & 0x1000) != 0;
    s.hflip = (w2 & 0x0800) != 0;
    s.tile  = w2 & 0x07FF;
    s.rawX  = w3 & 0x01FF;      // hardware X field is 9 bits (Gens showed 10)
    return s;
}

QRgb VdpSpritesView::cramColor(int pal, int idx) const
{
    const int off = (pal * 16 + idx) * 2;
    if (off + 1 >= static_cast<int>(cram_.size())) return qRgb(0, 0, 0);
    const uint16_t c = cram_[off] | (cram_[off + 1] << 8);
    int r, g, b;
    cram_to_rgb(c, r, g, b);
    return qRgb(r, g, b);
}

QImage VdpSpritesView::renderSprite(const Sprite& s, int pal) const
{
    QImage img(s.wPx, s.hPx, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (vram_.empty()) return img;
    int tile = s.tile;
    for (int tx = 0; tx < s.wPx; tx += 8) {                 // column-major tile order
        for (int ty = 0; ty < s.hPx; ty += 8, ++tile) {
            const uint32_t base = static_cast<uint32_t>(tile & 0x7FF) * 32;
            for (int y = 0; y < 8; ++y) {
                for (int bx = 0; bx < 4; ++bx) {
                    const uint8_t b = vram_[((base + y * 4 + bx) ^ 1) & 0xFFFF];
                    const int nib[2] = { b >> 4, b & 0xF }; // high nibble = left pixel
                    for (int k = 0; k < 2; ++k) {
                        if (!nib[k]) continue;              // index 0 transparent
                        int px = tx + bx * 2 + k, py = ty + y;
                        if (s.hflip) px = s.wPx - 1 - px;
                        if (s.vflip) py = s.hPx - 1 - py;
                        img.setPixel(px, py, cramColor(pal, nib[k]));
                    }
                }
            }
        }
    }
    return img;
}

void VdpSpritesView::rebuildList()
{
    sprites_.clear();
    QString note;
    const bool have = !sat_.empty() || !vram_.empty();
    const int  maxSpr = h40_ ? 80 : 64;
    if (have) {
        if (chainCheck_->isChecked()) {
            bool seen[128] = {};
            int idx = 0;
            while (static_cast<int>(sprites_.size()) < maxSpr) {
                if (idx >= maxSpr) { note = QStringLiteral("  [link %1 out of range]").arg(idx); break; }
                if (seen[idx])     { note = QStringLiteral("  [LOOP back to #%1]").arg(idx, 2, 10, QLatin1Char('0')); break; }
                seen[idx] = true;
                const Sprite s = decodeSprite(idx);
                sprites_.push_back(s);
                if (s.link == 0) break;
                idx = s.link;
            }
        } else {
            for (int i = 0; i < maxSpr; ++i) sprites_.push_back(decodeSprite(i));
        }
        QString info = QString::asprintf("SAT @ $%04X  %s / %d sprites  listed %d",
                                         satBase_, h40_ ? "H40" : "H32", maxSpr,
                                         static_cast<int>(sprites_.size()));
        infoLabel_->setText(info + note);
    } else {
        infoLabel_->setText(QStringLiteral("--"));
    }

    int selNum = -1;
    const int curRow = table_->currentRow();
    if (curRow >= 0 && curRow < table_->rowCount())
        if (auto* it = table_->item(curRow, 0)) selNum = it->data(Qt::UserRole).toInt();
    const int scrollPos = table_->verticalScrollBar()->value();

    QSignalBlocker block(table_);
    table_->setUpdatesEnabled(false);
    table_->setRowCount(static_cast<int>(sprites_.size()));
    auto put = [&](int r, int c, const QString& txt) -> QTableWidgetItem* {
        auto* it = table_->item(r, c);
        if (!it) { it = new QTableWidgetItem; table_->setItem(r, c, it); }
        it->setText(txt);
        return it;
    };
    int newRow = -1;
    for (int r = 0; r < static_cast<int>(sprites_.size()); ++r) {
        const Sprite& s = sprites_[r];
        put(r, 0, QString::asprintf("%02d", s.num))->setData(Qt::UserRole, s.num);
        put(r, 1, QString::asprintf("%4d (%4d)", s.rawY, s.rawY - 128));
        auto* xIt = put(r, 2, QString::asprintf("%4d (%4d)", s.rawX, s.rawX - 128));
        xIt->setToolTip(s.rawX == 0
            ? QStringLiteral("X=0: line-mask sprite (hides lower-priority sprites on its scanlines)")
            : QString());
        put(r, 3, QString::asprintf("%02dx%02d (%dx%d)", s.wPx, s.hPx, s.wPx / 8, s.hPx / 8));
        put(r, 4, QString::asprintf("%02d", s.link));
        put(r, 5, QString::asprintf("%d", s.pal));
        put(r, 6, QString::asprintf("%03X", s.tile));
        put(r, 7, QString::asprintf("%c%c%c", s.prio ? 'P' : 'p',
                                              s.vflip ? 'V' : 'v',
                                              s.hflip ? 'H' : 'h'));
        if (s.num == selNum) newRow = r;
    }
    if (newRow >= 0) table_->selectRow(newRow);
    else if (selNum >= 0) table_->clearSelection();
    table_->verticalScrollBar()->setValue(scrollPos);
    table_->setUpdatesEnabled(true);
}

void VdpSpritesView::updatePreviews()
{
    int row = -1;
    if (auto* sel = table_->selectionModel(); sel && sel->hasSelection())
        row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(sprites_.size()) || vram_.empty()) {
        preview_->setSprite(QImage());
        palStrip_->clear();
        return;
    }
    const Sprite& s = sprites_[row];
    preview_->setSprite(renderSprite(s, s.pal));
    QImage imgs[4];
    for (int p = 0; p < 4; ++p) imgs[p] = renderSprite(s, p);
    palStrip_->setSprites(imgs, s.pal);
}

void VdpSpritesView::onSelectionChanged() { updatePreviews(); }

void VdpSpritesView::onChainToggled(bool)
{
    rebuildList();
    dumpBtn_->setEnabled(!sprites_.empty());
    updatePreviews();
}

QString VdpSpritesView::formatRow(const Sprite& s) const
{
    return QString::asprintf("%02d   %4d(%4d)   %4d(%4d)   %02dx%02d (%dx%d)   %02d    %d    %03X   %c%c%c",
                             s.num, s.rawY, s.rawY - 128, s.rawX, s.rawX - 128,
                             s.wPx, s.hPx, s.wPx / 8, s.hPx / 8,
                             s.link, s.pal, s.tile,
                             s.prio ? 'P' : 'p', s.vflip ? 'V' : 'v', s.hflip ? 'H' : 'h');
}

void VdpSpritesView::onDump()
{
    if (sprites_.empty()) return;
    const QString fn = QFileDialog::getSaveFileName(this, QStringLiteral("Dump Sprite List"),
                                                    QStringLiteral("sprites.txt"),
                                                    QStringLiteral("Text files (*.txt);;All files (*)"));
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setStatus(QStringLiteral("Dump failed"));
        return;
    }
    QTextStream ts(&f);
    ts << QString::asprintf("VDP sprite table  SAT @ $%04X  %s (%d sprites)  order: %s\n",
                            satBase_, h40_ ? "H40" : "H32", h40_ ? 80 : 64,
                            chainCheck_->isChecked() ? "link chain" : "table");
    ts << "Num  Ypos(scr)    Xpos(scr)    Size           Link  Pal  Tile  Flags (Priority/VFlip/HFlip, upper=set)\n";
    for (const Sprite& s : sprites_) ts << formatRow(s) << '\n';
    setStatus(QStringLiteral("Sprite list dumped"));
}

void VdpSpritesView::setStatus(const QString& msg)
{
    statusLabel_->setText(msg);
    QTimer::singleShot(1500, this, [this] { statusLabel_->clear(); });
}
