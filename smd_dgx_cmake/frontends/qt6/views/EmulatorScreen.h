#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <cstdint>
#include "debugger/IDebugBackend.h"

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QContextMenuEvent;
QT_END_NAMESPACE

// Emulator video output. Also owns controller input: the screen is where the
// user looks, so it is where key presses belong. Keys are translated to a
// PadButton mask and pushed to the backend; the core samples it once a frame.
//
// Default layout (pad 1):
//   arrows = D-pad,  Z/X/C = A/B/C,  A/S/D = X/Y/Z,
//   Enter = Start,   Backspace = Mode
class EmulatorScreen : public QWidget {
    Q_OBJECT
public:
    explicit EmulatorScreen(QWidget* parent = nullptr);
    ~EmulatorScreen() override;             // persists the presentation options
    void setBackend(IDebugBackend* b) { backend_ = b; }
    void refresh() {}                       // frames arrive via pushFrame()

    void pushFrame(const uint8_t* data, int srcW, int srcH, int pitch,
                   int vpX, int vpY, int vpW, int vpH);

    // Presentation. Defaults suit a debugger: unfiltered pixels at an integer
    // scale, so what you see maps 1:1 onto VRAM. Right-click to change.
    void setKeepAspect(bool v)   { keepAspect_ = v;   update(); }
    void setSmooth(bool v)       { smooth_ = v;       update(); }
    void setIntegerScale(bool v) { integerScale_ = v; update(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    QSize sizeHint() const override { return {640, 480}; }

private:
    void applyKey(QKeyEvent* e, bool pressed);
    QRectF targetRect() const;

    IDebugBackend* backend_ = nullptr;
    uint16_t pad_ = 0;

    QMutex  mutex_;
    QImage  back_, front_;
    bool    newFrame_     = false;
    bool    keepAspect_   = true;
    bool    smooth_       = false;   // nearest-neighbour by default
    bool    integerScale_ = true;    // pixel perfect by default
    int     vpX_=0, vpY_=0, vpW_=320, vpH_=224;
};
