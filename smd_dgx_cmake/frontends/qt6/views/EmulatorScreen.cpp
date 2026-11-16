#include "EmulatorScreen.h"
#include "ViewSettings.h"
#include <QPainter>
#include <QMutexLocker>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <cmath>
#include <cstring>

EmulatorScreen::EmulatorScreen(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(320, 224);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPalette pal = palette(); pal.setColor(QPalette::Window, Qt::black);
    setAutoFillBackground(true); setPalette(pal);
    // Needed to receive key events at all; click-to-focus so the host's other
    // panes keep working normally.
    setFocusPolicy(Qt::StrongFocus);

    static const QString g = QStringLiteral("Screen");
    integerScale_ = viewsettings::getBool(g, QStringLiteral("integerScale"), integerScale_);
    smooth_       = viewsettings::getBool(g, QStringLiteral("smooth"),       smooth_);
    keepAspect_   = viewsettings::getBool(g, QStringLiteral("keepAspect"),   keepAspect_);
}

EmulatorScreen::~EmulatorScreen()
{
    static const QString g = QStringLiteral("Screen");
    viewsettings::putBool(g, QStringLiteral("integerScale"), integerScale_);
    viewsettings::putBool(g, QStringLiteral("smooth"),       smooth_);
    viewsettings::putBool(g, QStringLiteral("keepAspect"),   keepAspect_);
}

// --------------------------------------------------------------------------
// input
// --------------------------------------------------------------------------
// Letter keys are matched by physical position, not by the character they
// produce: Qt::Key_Z is whatever key types "z" in the current layout, so on a
// Russian (or French, or Dvorak) layout the face buttons would move or vanish.
// Scan codes are layout-independent but platform-specific, hence the table.
namespace {
#if defined(Q_OS_WIN)
enum : quint32 { SC_A = 0x1E, SC_S = 0x1F, SC_D = 0x20,
                 SC_Z = 0x2C, SC_X = 0x2D, SC_C = 0x2E };
#elif defined(Q_OS_MACOS)
enum : quint32 { SC_A = 0x00, SC_S = 0x01, SC_D = 0x02,
                 SC_Z = 0x06, SC_X = 0x07, SC_C = 0x08 };
#else   // X11/xkb: keycode == PS/2 scan code + 8
enum : quint32 { SC_A = 0x26, SC_S = 0x27, SC_D = 0x28,
                 SC_Z = 0x34, SC_X = 0x35, SC_C = 0x36 };
#endif
} // namespace

void EmulatorScreen::applyKey(QKeyEvent* e, bool pressed)
{
    if (e->isAutoRepeat()) { e->accept(); return; }

    uint16_t bit = 0;
    // Arrows, Enter and Backspace carry the same Qt key code in every layout,
    // so they are matched directly.
    switch (e->key()) {
    case Qt::Key_Up:        bit = PAD_UP;    break;
    case Qt::Key_Down:      bit = PAD_DOWN;  break;
    case Qt::Key_Left:      bit = PAD_LEFT;  break;
    case Qt::Key_Right:     bit = PAD_RIGHT; break;
    case Qt::Key_Return:
    case Qt::Key_Enter:     bit = PAD_START; break;
    case Qt::Key_Backspace: bit = PAD_MODE;  break;
    default:
        switch (e->nativeScanCode()) {
        case SC_Z: bit = PAD_A; break;
        case SC_X: bit = PAD_B; break;
        case SC_C: bit = PAD_C; break;
        case SC_A: bit = PAD_X; break;
        case SC_S: bit = PAD_Y; break;
        case SC_D: bit = PAD_Z; break;
        default:
            e->ignore();
            return;
        }
        break;
    }

    pad_ = pressed ? (pad_ | bit) : (uint16_t)(pad_ & ~bit);
    if (backend_) backend_->setPad(0, pad_);
    e->accept();
}

void EmulatorScreen::keyPressEvent(QKeyEvent* e)   { applyKey(e, true);  }
void EmulatorScreen::keyReleaseEvent(QKeyEvent* e) { applyKey(e, false); }

void EmulatorScreen::focusOutEvent(QFocusEvent*)
{
    // Losing focus mid-press would latch a button down forever.
    pad_ = 0;
    if (backend_) backend_->setPad(0, 0);
}

void EmulatorScreen::pushFrame(const uint8_t* data, int w, int h, int pitch, double aspect)
{
    if (w <= 0 || h <= 0) return;
    QMutexLocker lk(&mutex_);
    if (back_.width() != w || back_.height() != h)
        back_ = QImage(w, h, QImage::Format_RGB16);
    for (int y = 0; y < h; ++y)
        std::memcpy(back_.scanLine(y), data + y*pitch, (size_t)w*2);
    aspect_ = aspect; newFrame_ = true;
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void EmulatorScreen::paintEvent(QPaintEvent*)
{
    { QMutexLocker lk(&mutex_); if (newFrame_) { front_ = back_.copy(); frontAspect_ = aspect_; newFrame_ = false; } }
    if (front_.isNull()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth_);
    const QRectF dst = targetRect();
    if (dst != QRectF(rect()))
        p.fillRect(rect(), Qt::black);
    p.drawImage(dst, front_);
}

// Where the frame lands inside the widget. The aspect it keeps is the one the
// frame came with — square pixels or the TV's — or square pixels when it came
// with none.
//   integer scale : the largest picture, centred, in which every emulated line
//                   is N rows and every pixel M columns, both whole numbers, so
//                   nothing is resampled. M is the one that comes closest to
//                   the aspect; it is N for square pixels.
//                   Falls back to fitting when the widget is smaller than one
//                   full frame, otherwise there would be nothing to show.
//   fit           : fill the widget, optionally preserving aspect.
QRectF EmulatorScreen::targetRect() const
{
    const double sw = front_.width(), sh = front_.height();
    if (sw <= 0 || sh <= 0) return QRectF(rect());
    const double aspect = frontAspect_ > 0 ? frontAspect_ : sw / sh;

    auto centred = [this](double w, double h) {
        return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
    };

    if (integerScale_) {
        // A pixel's width over its height: 1 for square pixels; at the TV's
        // aspect about 0.9 for a plain H40 frame, 0.46 for the NTSC filter's
        // doubled lines, 1.8 for a double field.
        const double pixelAspect = aspect * sh / sw;
        for (int n = int(height() / sh); n >= 1; --n) {
            const int m = qMax(1, qRound(n * pixelAspect));
            // Square pixels, 9% off the TV shape in H40 and 13% in H32, have
            // to pass; a 1x NTSC-filtered frame, more than twice too wide,
            // must not.
            if (std::abs(m / (n * pixelAspect) - 1.0) > 0.15) continue;
            if (sw * m <= width()) return centred(sw * m, sh * n);
        }
        // No whole-number shape near the aspect fits: PAL H32 has 1.39-wide
        // pixels, so 1x is 28% off, and 2x needs two frames of height. Square
        // pixels still beat resampling unless they are far off — a 1x
        // NTSC-filtered frame is twice too wide.
        const int n = qMin(int(width() / sw), int(height() / sh));
        if (n >= 1 && std::abs(1.0 / pixelAspect - 1.0) <= 0.35)
            return centred(sw * n, sh * n);
        // too small for 1x — fall through to a fitted, aspect-correct image
    }

    if (keepAspect_ || integerScale_) {
        if (double(width()) / double(height()) > aspect) {
            const double h = height();
            return centred(h * aspect, h);
        }
        const double w = width();
        return centred(w, w / aspect);
    }
    return QRectF(rect());
}

void EmulatorScreen::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);

    auto* pixel = menu.addAction(QStringLiteral("Pixel perfect (integer scale)"));
    pixel->setCheckable(true);
    pixel->setChecked(integerScale_);
    connect(pixel, &QAction::toggled, this, &EmulatorScreen::setIntegerScale);

    auto* smooth = menu.addAction(QStringLiteral("Smooth filtering"));
    smooth->setCheckable(true);
    smooth->setChecked(smooth_);
    connect(smooth, &QAction::toggled, this, &EmulatorScreen::setSmooth);

    auto* aspect = menu.addAction(QStringLiteral("Keep aspect ratio"));
    aspect->setCheckable(true);
    aspect->setChecked(keepAspect_);
    aspect->setEnabled(!integerScale_);       // integer scaling implies it
    connect(aspect, &QAction::toggled, this, &EmulatorScreen::setKeepAspect);

    menu.exec(e->globalPos());
}
