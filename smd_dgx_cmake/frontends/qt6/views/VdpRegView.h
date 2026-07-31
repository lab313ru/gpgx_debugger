#pragma once
#include <QWidget>
#include <QTabWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <functional>
#include <vector>
#include <cstdint>
#include "debugger/IDebugBackend.h"

// Two-tab VDP register editor – port of the Gens KMod register tab pages
// (IDD_VDP_REGISTERS_MODEREGISTERS / IDD_VDP_REGISTERS_OTHERREGISTERS).
class VdpRegView : public QWidget {
    Q_OBJECT
public:
    explicit VdpRegView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b);
    void refresh();
    QSize sizeHint() const override { return {840, 560}; }
private:
    struct BitBox {
        QCheckBox* box;
        int reg, bit;
    };
    struct HexField {
        QLineEdit* edit;
        int width;                                    // min hex digits; 0 = decimal
        std::vector<int> deps;                        // regs feeding the display
        std::function<uint32_t(const uint8_t*)> read;
        std::function<void(uint32_t)> write;          // null = read-only
    };

    QWidget*   buildModeTab();
    QWidget*   buildOtherTab();
    QCheckBox* addCheck(int reg, int bit, const QString& label);
    QLineEdit* addField(int width, std::vector<int> deps,
                        std::function<uint32_t(const uint8_t*)> read,
                        std::function<void(uint32_t)> write = nullptr);
    void       commitField(QLineEdit* edit);
    void       setRegBits(int reg, int lo, int count, uint32_t v);
    void       writeDmaSrc(uint32_t byteAddr);
    void       syncControls();

    IDebugBackend* backend_ = nullptr;
    QTabWidget*    tabs_    = nullptr;
    QLabel*        modeHdr_[4]{};                     // raw-value headers: 0x00/0x01/0x0B/0x0C
    std::vector<BitBox>   bits_;
    std::vector<HexField> fields_;
    uint8_t regs_[0x18]{};
    uint8_t prev_[0x18]{};
    uint8_t diff_[0x18]{};                            // per-bit change mask since last refresh
    bool    first_ = true;
};
