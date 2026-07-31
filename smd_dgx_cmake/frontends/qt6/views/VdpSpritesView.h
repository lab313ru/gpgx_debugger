#pragma once
#include <QWidget>
#include <QTableWidget>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

class VdpSpritePreview;
class VdpSpritePalStrip;

// Sprite list viewer – port of Gens KMod "VDP Sprites" dialog (vdp_sprites.cpp).
class VdpSpritesView : public QWidget {
    Q_OBJECT
public:
    explicit VdpSpritesView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b);
    void refresh();
    QSize sizeHint() const override { return {780, 440}; }
private slots:
    void onSelectionChanged();
    void onChainToggled(bool);
    void onDump();
private:
    struct Sprite {
        int      num  = 0;
        uint16_t rawY = 0, rawX = 0;
        int      wPx  = 8, hPx = 8;          // 8/16/24/32
        int      link = 0, pal = 0, tile = 0;
        bool     prio = false, vflip = false, hflip = false;
    };
    void     rebuildList();
    void     updatePreviews();
    void     setStatus(const QString& msg);
    Sprite   decodeSprite(int idx) const;
    uint16_t satWord(int spriteIdx, int word) const;
    QRgb     cramColor(int pal, int idx) const;
    QImage   renderSprite(const Sprite& s, int pal) const;
    QString  formatRow(const Sprite& s) const;

    IDebugBackend* backend_ = nullptr;
    std::vector<uint8_t> vram_, cram_, sat_;
    uint8_t  reg5_    = 0;
    bool     h40_     = false;
    uint32_t satBase_ = 0;
    std::vector<Sprite> sprites_;            // display order

    QTableWidget*      table_      = nullptr;
    QCheckBox*         chainCheck_ = nullptr;
    QPushButton*       dumpBtn_    = nullptr;
    QLabel*            infoLabel_  = nullptr;
    QLabel*            statusLabel_= nullptr;
    VdpSpritePreview*  preview_    = nullptr;
    VdpSpritePalStrip* palStrip_   = nullptr;
};
