#pragma once
#include <QWidget>
#include "debugger/DebugState.h"

class IDebugBackend;
QT_BEGIN_NAMESPACE
class QLabel;
class QCheckBox;
class QRadioButton;
class QButtonGroup;
class QTableWidget;
class QProgressBar;
QT_END_NAMESPACE

// ADSR envelope sketch for one operator (port of the Gens owner-draw canvas,
// resolution-independent: same algorithm mapped onto the widget rect).
class AdsrCanvas : public QWidget {
    Q_OBJECT
public:
    explicit AdsrCanvas(QWidget* parent = nullptr);
    void setParams(int tl, int ar, int dr, int sl, int sr, int rr);
    void setValid(bool v);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    int  tl_ = 0x7F, ar_ = 0, dr_ = 0, sl_ = 0, sr_ = 0, rr_ = 0;
    bool valid_ = false;
};

// Read-only port of the Gens "YM2612 & PSG View". All values are decoded from
// the raw register shadows in SoundState (backend->getSoundState()).
class SoundDebugView : public QWidget {
    Q_OBJECT
public:
    explicit SoundDebugView(QWidget* parent = nullptr);
    void setBackend(IDebugBackend* b);
    void refresh();
private:
    void buildUi();
    void clearAll();
    void updateFm(const SoundState& s);
    void updatePsg(const SoundState& s);

    IDebugBackend* backend_    = nullptr;
    int            selChannel_ = 0;          // 0..5, persists like the original

    QButtonGroup* chGroup_ = nullptr;

    // Channel registers (selected channel)
    QLabel*    fnum_  = nullptr;
    QLabel*    block_ = nullptr;
    QLabel*    alg_   = nullptr;
    QLabel*    fb_    = nullptr;
    QLabel*    ams_   = nullptr;
    QLabel*    pms_   = nullptr;
    QCheckBox* outL_  = nullptr;
    QCheckBox* outR_  = nullptr;

    // Operator registers + envelopes
    QTableWidget* opTable_ = nullptr;
    AdsrCanvas*   adsr_[4]{};

    // LFO / DAC
    QCheckBox* lfoEn_   = nullptr;
    QLabel*    lfoFreq_ = nullptr;
    QLabel*    lfoHz_   = nullptr;
    QCheckBox* dacEn_   = nullptr;
    QLabel*    dacData_ = nullptr;

    // Timers (overflow flags live in YM2612.Status, not exposed -> N/A)
    QCheckBox* tLoad_[2]{};
    QCheckBox* tEn_[2]{};
    QLabel*    tPeriod_[2]{};

    // Channel 3 special mode
    QLabel* ch3Mode_ = nullptr;
    QLabel* ch3Fnum_[4]{};
    QLabel* ch3Block_[4]{};

    // Key on/off, decoded from the last register 0x28 write in the shadow
    QLabel*    keyCh_ = nullptr;
    QCheckBox* keyOp_[4]{};

    // PSG
    QLabel*       psgHz_[3]{};
    QLabel*       psgVal_[3]{};
    QLabel*       noiseType_  = nullptr;
    QLabel*       noiseClock_ = nullptr;
    QProgressBar* psgBar_[4]{};
};
