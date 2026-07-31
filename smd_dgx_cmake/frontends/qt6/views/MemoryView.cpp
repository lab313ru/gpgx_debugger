#include "MemoryView.h"
#include "debugger/IDebugBackend.h"
#include <QScrollBar>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QPainter>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QGuiApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QFileDialog>
#include <QFile>
#include <QSignalBlocker>
#include <algorithm>

namespace {
int hexVal(QChar c)
{
    if (c >= QLatin1Char('0') && c <= QLatin1Char('9')) return c.unicode() - '0';
    if (c >= QLatin1Char('a') && c <= QLatin1Char('f')) return c.unicode() - 'a' + 10;
    if (c >= QLatin1Char('A') && c <= QLatin1Char('F')) return c.unicode() - 'A' + 10;
    return -1;
}
} // namespace

MemoryView::MemoryView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFont(QFont(QStringLiteral("Courier New"), 9));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);

    bar_ = new QWidget(this);
    auto* hl = new QHBoxLayout(bar_);
    hl->setContentsMargins(4, 2, 4, 2);
    hl->addWidget(new QLabel(QStringLiteral("Region:"), bar_));
    regionCombo_ = new QComboBox(bar_);
    regionCombo_->setMinimumWidth(90);
    regionCombo_->setEnabled(false);
    hl->addWidget(regionCombo_);
    textChk_ = new QCheckBox(QStringLiteral("Text"), bar_);
    textChk_->setChecked(true);
    hl->addWidget(textChk_);
    linesChk_ = new QCheckBox(QStringLiteral("Lines"), bar_);
    linesChk_->setChecked(true);
    hl->addWidget(linesChk_);
    hl->addStretch();
    status_ = new QLabel(bar_);
    hl->addWidget(status_);

    setViewportMargins(0, bar_->sizeHint().height(), 0, 0);

    connect(regionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MemoryView::onRegionChanged);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MemoryView::onScroll);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { viewport()->update(); });
    connect(textChk_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on && editArea_ == Area::Text) editArea_ = Area::Hex;
        updateScrollRange();
        viewport()->update();
        updateStatus();
    });
    connect(linesChk_, &QCheckBox::toggled, this, [this] { viewport()->update(); });

    updateStatus();
}

void MemoryView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    populateRegions();
}

void MemoryView::refresh()
{
    if (!backend_) return;
    if (regions_.empty()) { populateRegions(); return; }

    // Region sizes can change (ROM load) — re-query the cheap descriptor table.
    auto fresh = backend_->getMemRegions();
    int idx = regionCombo_->currentIndex();
    if (fresh.size() == regions_.size() && idx >= 0 && idx < static_cast<int>(fresh.size())) {
        regions_ = std::move(fresh);
        if (hasRegion_ && regions_[idx].size != cur_.size) {
            cur_ = regions_[idx];
            clampSelection();
            updateScrollRange();
            updateStatus();
        } else if (hasRegion_) {
            cur_ = regions_[idx];
        }
    }
    refreshVisible();
}

void MemoryView::populateRegions()
{
    QSignalBlocker block(regionCombo_);
    regionCombo_->clear();
    regions_.clear();
    hasRegion_ = false;
    hasSel_ = false;
    hoverOff_ = -1;
    vis_.clear();

    if (!backend_) {
        regionCombo_->setEnabled(false);
        viewport()->update();
        updateStatus();
        return;
    }

    regions_ = backend_->getMemRegions();
    int def = 0;
    for (size_t i = 0; i < regions_.size(); ++i) {
        regionCombo_->addItem(QString::fromStdString(regions_[i].name));
        if (regions_[i].name == "RAM 68K") def = static_cast<int>(i);
    }
    regionCombo_->setEnabled(!regions_.empty());
    if (!regions_.empty()) {
        regionCombo_->setCurrentIndex(def);
        applyRegion(def);
    } else {
        viewport()->update();
        updateStatus();
    }
}

void MemoryView::onRegionChanged(int idx) { applyRegion(idx); }

void MemoryView::applyRegion(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(regions_.size())) {
        hasRegion_ = false;
        hasSel_ = false;
        vis_.clear();
        viewport()->update();
        updateStatus();
        return;
    }
    cur_ = regions_[idx];
    hasRegion_ = true;
    visFirst_ = 0;
    selAnchor_ = selLast_ = 0;
    hasSel_ = cur_.size > 0;
    pendingSecond_ = false;
    hoverOff_ = -1;
    editArea_ = Area::Hex;
    {
        QSignalBlocker b(verticalScrollBar());
        verticalScrollBar()->setValue(0);
    }
    updateScrollRange();
    refreshVisible();
    updateStatus();
}

void MemoryView::onScroll(int value)
{
    visFirst_ = static_cast<uint32_t>(value) * 16u;
    refreshVisible();
}

void MemoryView::clampSelection()
{
    if (!hasRegion_ || cur_.size == 0) {
        hasSel_ = false;
        visFirst_ = 0;
        selAnchor_ = selLast_ = 0;
        pendingSecond_ = false;
        return;
    }
    const int64_t last = static_cast<int64_t>(cur_.size) - 1;
    selAnchor_ = std::clamp<int64_t>(selAnchor_, 0, last);
    selLast_   = std::clamp<int64_t>(selLast_, 0, last);
    if (!hasSel_) { hasSel_ = true; selAnchor_ = selLast_ = 0; }
}

void MemoryView::refreshVisible()
{
    vis_.clear();
    if (backend_ && hasRegion_ && cur_.size > 0 && visFirst_ < cur_.size) {
        const uint32_t want = static_cast<uint32_t>(visibleRows() + 1) * 16u;
        const uint32_t n = std::min(want, cur_.size - visFirst_);
        vis_ = backend_->readRegion(cur_.id, visFirst_, n);
    }
    viewport()->update();
}

void MemoryView::updateScrollRange()
{
    const Metrics m = metrics();
    const int total = static_cast<int>(totalRows());
    const int visR  = visibleRows();
    auto* vs = verticalScrollBar();
    vs->setRange(0, std::max(0, total - visR));
    vs->setPageStep(visR);
    vs->setSingleStep(1);

    const int clientW = textChk_->isChecked() ? m.textR : m.hexR;
    auto* hs = horizontalScrollBar();
    hs->setRange(0, std::max(0, clientW - viewport()->width()));
    hs->setPageStep(viewport()->width());
    hs->setSingleStep(m.cw);

    visFirst_ = static_cast<uint32_t>(vs->value()) * 16u;
}

int MemoryView::visibleRows() const
{
    const Metrics m = metrics();
    return std::max(1, (viewport()->height() - m.top) / m.rh);
}

uint32_t MemoryView::totalRows() const
{
    return hasRegion_ ? (cur_.size + 15u) / 16u : 0;
}

uint8_t MemoryView::byteAt(uint32_t off, bool& ok) const
{
    ok = false;
    if (off < visFirst_) return 0;
    const uint32_t i = off - visFirst_;
    if (i >= vis_.size()) return 0;
    ok = true;
    return vis_[i];
}

// ---------------------------------------------------------------------------
// Geometry (Gens formulas with FontWidth=cw, Gap=cw)
// ---------------------------------------------------------------------------
MemoryView::Metrics MemoryView::metrics() const
{
    QFontMetrics fm(font());
    Metrics m;
    m.cw    = fm.horizontalAdvance(QLatin1Char('0'));
    m.rh    = fm.height() + 2;
    m.gap   = m.cw;
    m.cellW = m.cw * 3;
    m.hexL  = m.cw * 9;
    m.hexR  = m.hexL + m.gap + 16 * m.cellW;
    m.textL = m.hexR;
    m.textR = m.textL + m.gap + 16 * m.cw;
    m.top   = m.rh;
    return m;
}

int MemoryView::cellX(const Metrics& m, int col) const
{
    return m.hexL + col * m.cellW + (col >= 8 ? m.gap : 0);
}

int MemoryView::textX(const Metrics& m, int col) const
{
    return m.textL + m.gap / 2 + col * m.cw;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
int64_t MemoryView::selStart() const { return std::min(selAnchor_, selLast_); }
int64_t MemoryView::selEnd()   const { return std::max(selAnchor_, selLast_); }

void MemoryView::moveCursor(int64_t off, bool extend)
{
    if (!hasRegion_ || cur_.size == 0) return;
    off = std::clamp<int64_t>(off, 0, static_cast<int64_t>(cur_.size) - 1);
    pendingSecond_ = false;
    hasSel_ = true;
    selLast_ = off;
    if (!extend) selAnchor_ = off;
    ensureVisible(off);
    viewport()->update();
    updateStatus();
}

void MemoryView::ensureVisible(int64_t off)
{
    const int row = static_cast<int>(off / 16);
    auto* vs = verticalScrollBar();
    const int first = vs->value();
    const int visR = visibleRows();
    if (row < first)                vs->setValue(row);
    else if (row >= first + visR)   vs->setValue(row - visR + 1);
}

bool MemoryView::hitTest(QPoint pos, int64_t& off, Area& area, bool clampInside) const
{
    off = -1;
    area = Area::None;
    if (!hasRegion_ || cur_.size == 0) return false;

    const Metrics m = metrics();
    const bool showText = textChk_->isChecked();
    int x = pos.x() + horizontalScrollBar()->value();
    const int y = pos.y();

    const int right = (showText ? m.textR : m.hexR) - 1;
    if (clampInside) x = std::clamp(x, m.hexL, right);
    else if (x < m.hexL || x > right) return false;

    // Gens gap compensation: half-char cell padding on the left group,
    // remove the mid gap on the right group.
    if (x > m.hexL + 8 * m.cellW) x -= m.gap / 2;
    else                          x += m.gap / 2;

    const int dy = y - m.top;
    const int line = dy >= 0 ? dy / m.rh : -((-dy + m.rh - 1) / m.rh);

    int col;
    if (x < m.hexR) {
        area = Area::Hex;
        col = std::clamp((x - m.hexL) / m.cellW, 0, 15);
    } else if (showText) {
        area = Area::Text;
        col = std::clamp((x - m.textL) / m.cw, 0, 15);
    } else {
        return false;
    }
    off = static_cast<int64_t>(visFirst_) + static_cast<int64_t>(line) * 16 + col;
    return true;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void MemoryView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), Qt::white);
    p.setFont(font());

    const Metrics m = metrics();
    const QFontMetrics fm(font());
    const int asc = fm.ascent() + 1;
    const bool showText = textChk_->isChecked();
    p.translate(-horizontalScrollBar()->value(), 0);

    // column header
    p.setPen(QColor(0x60, 0x60, 0xAA));
    for (int col = 0; col < 16; ++col)
        p.drawText(cellX(m, col), asc, QStringLiteral("%1").arg(col, 2, 16).toUpper());

    if (!backend_ || !hasRegion_ || cur_.size == 0) {
        p.setPen(QColor(0xA0, 0xA0, 0xA0));
        p.drawText(m.hexL, m.top + asc, QStringLiteral("(no data)"));
        return;
    }

    const int rows = visibleRows() + 1;
    const int64_t s0 = hasSel_ ? selStart() : -1;
    const int64_t s1 = hasSel_ ? selEnd() : -1;

    for (int line = 0; line < rows; ++line) {
        const int64_t rowAddr = static_cast<int64_t>(visFirst_) + static_cast<int64_t>(line) * 16;
        if (rowAddr >= static_cast<int64_t>(cur_.size)) break;
        const int y = m.top + line * m.rh;

        p.setPen(QColor(0x60, 0x60, 0xAA));
        p.drawText(m.cw / 2, y + asc, displayAddr(rowAddr) + QLatin1Char(':'));

        for (int col = 0; col < 16; ++col) {
            const int64_t addr = rowAddr + col;
            if (addr >= static_cast<int64_t>(cur_.size)) break;
            bool ok = false;
            const uint8_t b = byteAt(static_cast<uint32_t>(addr), ok);
            const bool sel = hasSel_ && addr >= s0 && addr <= s1;

            const int cx = cellX(m, col);
            if (sel) p.fillRect(QRect(cx - m.cw / 2, y, m.cw * 5 / 2, m.rh), Qt::black);
            p.setPen(sel ? Qt::white : Qt::black);
            QString t;
            if (pendingSecond_ && addr == s0)
                t = QStringLiteral("%1.").arg(pendingNibble_, 1, 16).toUpper();
            else if (ok)
                t = QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
            else
                t = QStringLiteral("??");
            p.drawText(cx, y + asc, t);

            if (showText) {
                const int tx = textX(m, col);
                if (sel) p.fillRect(QRect(tx, y, m.cw, m.rh), Qt::black);
                p.setPen(sel ? Qt::white : Qt::black);
                const char c = (ok && b >= 0x20 && b <= 0x7E) ? static_cast<char>(b) : '.';
                p.drawText(tx, y + asc, QString(QLatin1Char(c)));
            }
        }
    }

    if (linesChk_->isChecked()) {
        p.setPen(QColor(0xB0, 0xB0, 0xB0));
        const int w = showText ? m.textR : m.hexR;
        const int h = viewport()->height();
        p.drawLine(0, m.top - 1, w, m.top - 1);
        p.drawLine(m.hexL - m.cw, 0, m.hexL - m.cw, h);
        p.drawLine(m.hexL + 8 * m.cellW + m.gap / 2, 0, m.hexL + 8 * m.cellW + m.gap / 2, h);
        if (showText) p.drawLine(m.textL, 0, m.textL, h);
    }
}

void MemoryView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    const int barH = bar_->sizeHint().height();
    const int sbw = verticalScrollBar()->isVisible() ? verticalScrollBar()->width() : 0;
    bar_->setGeometry(0, 0, width() - sbw, barH);
    updateScrollRange();
    refreshVisible();
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void MemoryView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    setFocus();
    const QPoint pt = e->position().toPoint();
    int64_t off; Area a;
    const Metrics m = metrics();
    if (pt.y() >= m.top && hitTest(pt, off, a, false)) {
        editArea_ = a;
        moveCursor(off, e->modifiers() & Qt::ShiftModifier);
        mouseHeld_ = true;
    }
}

void MemoryView::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pt = e->position().toPoint();
    if (mouseHeld_) {
        int64_t off; Area a;
        if (hitTest(pt, off, a, true))
            moveCursor(off, true);
        return;
    }
    int64_t off; Area a;
    if (hitTest(pt, off, a, false) &&
        off >= 0 && off < static_cast<int64_t>(cur_.size))
        hoverOff_ = off;
    else
        hoverOff_ = -1;
    const QString sym = hoverOff_ >= 0 ? symbolAt(hoverOff_) : QString();
    viewport()->setToolTip(sym);
    updateStatus();
}

void MemoryView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) mouseHeld_ = false;
    else QAbstractScrollArea::mouseReleaseEvent(e);
}

void MemoryView::wheelEvent(QWheelEvent* e)
{
    // Gens: one full page per wheel notch, sign only.
    const int dy = e->angleDelta().y();
    if (dy == 0) { QAbstractScrollArea::wheelEvent(e); return; }
    auto* vs = verticalScrollBar();
    vs->setValue(vs->value() + (dy < 0 ? vs->pageStep() : -vs->pageStep()));
    e->accept();
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
void MemoryView::keyPressEvent(QKeyEvent* e)
{
    if (!backend_ || !hasRegion_ || cur_.size == 0) {
        QAbstractScrollArea::keyPressEvent(e);
        return;
    }
    if (e->matches(QKeySequence::Copy))  { copyAuto();  return; }
    if (e->matches(QKeySequence::Paste)) { pasteAuto(); return; }
    const bool ctrl  = e->modifiers() & Qt::ControlModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    if (ctrl && e->key() == Qt::Key_G) { gotoDialog(); return; }

    const int64_t cur = hasSel_ ? selLast_ : 0;
    const int64_t page = static_cast<int64_t>(visibleRows()) * 16;
    switch (e->key()) {
    case Qt::Key_Left:     moveCursor(cur - 1, shift);    return;
    case Qt::Key_Right:    moveCursor(cur + 1, shift);    return;
    case Qt::Key_Up:       moveCursor(cur - 16, shift);   return;
    case Qt::Key_Down:     moveCursor(cur + 16, shift);   return;
    case Qt::Key_PageUp:   moveCursor(cur - page, shift); return;
    case Qt::Key_PageDown: moveCursor(cur + page, shift); return;
    case Qt::Key_Home:
        moveCursor(ctrl ? 0 : cur - (cur % 16), shift);
        return;
    case Qt::Key_End:
        moveCursor(ctrl ? static_cast<int64_t>(cur_.size) - 1 : cur - (cur % 16) + 15, shift);
        return;
    default:
        break;
    }

    if (!ctrl) {
        const QString t = e->text();
        if (t.size() == 1) {
            const QChar qc = t.at(0);
            if (editArea_ == Area::Text) {
                const char c = qc.toLatin1();
                if (c != 0 && qc.unicode() >= 0x20) { typeChar(c); return; }
            } else {
                const int v = hexVal(qc);
                if (v >= 0) { typeHexDigit(v); return; }
            }
        }
    }
    QAbstractScrollArea::keyPressEvent(e);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------
void MemoryView::writeBytes(uint32_t off, const uint8_t* data, uint32_t n)
{
    if (!backend_ || !hasRegion_ || !cur_.writable) return;
    if (off >= cur_.size) return;
    n = std::min(n, cur_.size - off);
    backend_->writeRegion(cur_.id, off, data, n);
    refreshVisible();
}

void MemoryView::typeHexDigit(int v)
{
    if (!cur_.writable || !hasSel_) return;
    const int64_t start = selStart();
    if (!pendingSecond_) {
        selAnchor_ = selLast_ = start;      // collapse to selection start
        pendingSecond_ = true;
        pendingNibble_ = static_cast<uint8_t>(v);
        ensureVisible(start);
        viewport()->update();
        updateStatus();
    } else {
        const uint8_t b = static_cast<uint8_t>((pendingNibble_ << 4) | v);
        pendingSecond_ = false;
        writeBytes(static_cast<uint32_t>(start), &b, 1);
        moveCursor(start + 1, false);
    }
}

void MemoryView::typeChar(char c)
{
    if (!cur_.writable || !hasSel_) return;
    const int64_t start = selStart();
    const uint8_t b = static_cast<uint8_t>(c);
    writeBytes(static_cast<uint32_t>(start), &b, 1);
    moveCursor(std::min<int64_t>(start + 1, static_cast<int64_t>(cur_.size) - 1), false);
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------
void MemoryView::copyAuto()
{
    if (editArea_ == Area::Text) copyChars();
    else copyNumbers();
}

void MemoryView::copyNumbers()
{
    if (!backend_ || !hasRegion_ || !hasSel_) return;
    const int64_t s = selStart();
    const uint32_t n = static_cast<uint32_t>(selEnd() - s + 1);
    const auto d = backend_->readRegion(cur_.id, static_cast<uint32_t>(s), n);
    QString out;
    out.reserve(static_cast<int>(d.size()) * 2);
    for (uint8_t b : d)
        out += QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
    QGuiApplication::clipboard()->setText(out);
}

void MemoryView::copyChars()
{
    if (!backend_ || !hasRegion_ || !hasSel_) return;
    const int64_t s = selStart();
    const uint32_t n = static_cast<uint32_t>(selEnd() - s + 1);
    const auto d = backend_->readRegion(cur_.id, static_cast<uint32_t>(s), n);
    QGuiApplication::clipboard()->setText(
        QString::fromLatin1(reinterpret_cast<const char*>(d.data()),
                            static_cast<int>(d.size())));
}

void MemoryView::copyAddress()
{
    if (!hasRegion_ || !hasSel_) return;
    QGuiApplication::clipboard()->setText(displayAddr(selStart()));
}

void MemoryView::pasteAuto()
{
    if (editArea_ == Area::Text) pasteChars();
    else pasteNumbers();
}

void MemoryView::pasteNumbers()
{
    if (!backend_ || !hasRegion_ || !hasSel_ || !cur_.writable) return;
    pendingSecond_ = false;
    const QString t = QGuiApplication::clipboard()->text();
    std::vector<uint8_t> bytes;
    int hi = -1;
    for (QChar qc : t) {
        const int v = hexVal(qc);
        if (v < 0) continue;                // skip separators / garbage
        if (hi < 0) hi = v;
        else { bytes.push_back(static_cast<uint8_t>((hi << 4) | v)); hi = -1; }
    }
    const int64_t start = selStart();
    if (!bytes.empty()) {
        writeBytes(static_cast<uint32_t>(start), bytes.data(),
                   static_cast<uint32_t>(bytes.size()));
        moveCursor(std::min<int64_t>(start + static_cast<int64_t>(bytes.size()),
                                     static_cast<int64_t>(cur_.size) - 1), false);
    }
    if (hi >= 0) {                          // odd trailing digit stays pending
        pendingSecond_ = true;
        pendingNibble_ = static_cast<uint8_t>(hi);
        viewport()->update();
    }
    updateStatus();
}

void MemoryView::pasteChars()
{
    if (!backend_ || !hasRegion_ || !hasSel_ || !cur_.writable) return;
    pendingSecond_ = false;
    const QByteArray ba = QGuiApplication::clipboard()->text().toLatin1();
    if (ba.isEmpty()) return;
    const int64_t start = selStart();
    writeBytes(static_cast<uint32_t>(start),
               reinterpret_cast<const uint8_t*>(ba.constData()),
               static_cast<uint32_t>(ba.size()));
    moveCursor(std::min<int64_t>(start + ba.size(),
                                 static_cast<int64_t>(cur_.size) - 1), false);
}

// ---------------------------------------------------------------------------
// GoTo / dump
// ---------------------------------------------------------------------------
void MemoryView::gotoDialog()
{
    if (!hasRegion_ || cur_.size == 0) return;
    bool ok = false;
    QString s = QInputDialog::getText(
        this, QStringLiteral("Go To"),
        QStringLiteral("Type address to go to.\nFormat: FF**** (display address or region offset)"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    s = s.trimmed();
    if (s.startsWith(QLatin1Char('$'))) s.remove(0, 1);
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) s.remove(0, 2);
    bool okHex = false;
    const qulonglong v = s.toULongLong(&okHex, 16);
    if (!okHex) return;
    int64_t off = -1;
    if (v >= cur_.base && v < static_cast<qulonglong>(cur_.base) + cur_.size)
        off = static_cast<int64_t>(v - cur_.base);
    else if (v < cur_.size)
        off = static_cast<int64_t>(v);
    if (off < 0) return;
    moveCursor(off, false);
}

void MemoryView::dumpToFile()
{
    if (!backend_ || !hasRegion_ || cur_.size == 0) return;
    const QString def =
        QString::fromStdString(cur_.name).replace(QLatin1Char(' '), QLatin1Char('_'))
        + QStringLiteral("_dump.bin");
    const QString fn = QFileDialog::getSaveFileName(
        this, QStringLiteral("Dump region to file"), def,
        QStringLiteral("Binary files (*.bin);;All files (*)"));
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly)) return;
    constexpr uint32_t kChunk = 0x10000;
    for (uint32_t off = 0; off < cur_.size; off += kChunk) {
        const uint32_t n = std::min(kChunk, cur_.size - off);
        const auto d = backend_->readRegion(cur_.id, off, n);   // logical order
        f.write(reinterpret_cast<const char*>(d.data()),
                static_cast<qint64>(d.size()));
        if (d.size() < n) break;
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------
void MemoryView::contextMenuEvent(QContextMenuEvent* e)
{
    if (!hasRegion_) return;

    int64_t off; Area a;
    const Metrics m = metrics();
    if (e->pos().y() >= m.top && hitTest(e->pos(), off, a, false) &&
        off >= 0 && off < static_cast<int64_t>(cur_.size)) {
        editArea_ = a;
        if (!hasSel_ || off < selStart() || off > selEnd())
            moveCursor(off, false);
    }

    QMenu menu(this);
    auto* copyA  = menu.addAction(QStringLiteral("Copy"));
    copyA->setShortcut(QKeySequence::Copy);
    auto* copyN  = menu.addAction(QStringLiteral("Copy Numbers"));
    auto* copyC  = menu.addAction(QStringLiteral("Copy Characters"));
    auto* copyAd = menu.addAction(QStringLiteral("Copy Address"));
    menu.addSeparator();
    auto* pasteA = menu.addAction(QStringLiteral("Paste"));
    pasteA->setShortcut(QKeySequence::Paste);
    auto* pasteN = menu.addAction(QStringLiteral("Paste to Numbers"));
    auto* pasteC = menu.addAction(QStringLiteral("Paste to Characters"));
    menu.addSeparator();
    auto* dumpA  = menu.addAction(QStringLiteral("Dump to File..."));
    auto* gotoA  = menu.addAction(QStringLiteral("Go To..."));
    gotoA->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));

    const bool canCopy = hasSel_ && cur_.size > 0;
    const bool canEdit = cur_.writable && hasSel_ && cur_.size > 0;
    copyA->setEnabled(canCopy);
    copyN->setEnabled(canCopy);
    copyC->setEnabled(canCopy);
    copyAd->setEnabled(canCopy);
    pasteA->setEnabled(canEdit);
    pasteN->setEnabled(canEdit);
    pasteC->setEnabled(canEdit);
    dumpA->setEnabled(backend_ && cur_.size > 0);

    QAction* r = menu.exec(e->globalPos());
    if      (r == copyA)  copyAuto();
    else if (r == copyN)  copyNumbers();
    else if (r == copyC)  copyChars();
    else if (r == copyAd) copyAddress();
    else if (r == pasteA) pasteAuto();
    else if (r == pasteN) pasteNumbers();
    else if (r == pasteC) pasteChars();
    else if (r == dumpA)  dumpToFile();
    else if (r == gotoA)  gotoDialog();
}

// ---------------------------------------------------------------------------
// Symbols / status
// ---------------------------------------------------------------------------
QString MemoryView::symbolAt(int64_t off) const
{
    if (!hasRegion_ || off < 0 || off >= static_cast<int64_t>(cur_.size)) return {};
    static const char* const kZ80[12] = {
        "AF", "BC", "DE", "HL", "AF2", "BC2", "DE2", "HL2", "IX", "IY", "SP", "PC"
    };
    static const char* const kVdp[24] = {
        "Set1", "Set2", "Pat_ScrA_Adr", "Pat_Win_Adr", "Pat_ScrB_Adr",
        "Spr_Att_Adr", "Reg6", "BG_Color", "Reg8", "Reg9", "H_Int", "Set3",
        "Set4", "H_Scr_Adr", "Reg14", "Auto_Inc", "Scr_Size", "Win_H_Pos",
        "Win_V_Pos", "DMA_Length_L", "DMA_Length_H", "DMA_Src_Adr_L",
        "DMA_Src_Adr_M", "DMA_Src_Adr_H"
    };
    switch (cur_.id) {
    case 6: {   // Regs M68K: D0-D7 then A0-A7, BE dwords
        const int r = static_cast<int>(off / 4);
        const int b = static_cast<int>(off % 4);
        const QString n = r < 8 ? QStringLiteral("D%1").arg(r)
                                : QStringLiteral("A%1").arg(r - 8);
        return QStringLiteral("%1[%2]").arg(n).arg(b);
    }
    case 7: {   // Regs Z80: 12 BE word pairs
        const int r = static_cast<int>(off / 2);
        if (r >= 12) return {};
        return QStringLiteral("%1[%2]").arg(QLatin1String(kZ80[r])).arg(off % 2);
    }
    case 8:     // Regs VDP: one byte per register
        if (off < 24) return QLatin1String(kVdp[static_cast<int>(off)]);
        return QStringLiteral("Reg%1").arg(off);
    default:
        return {};
    }
}

QString MemoryView::displayAddr(int64_t off) const
{
    return QStringLiteral("%1")
        .arg(static_cast<qulonglong>(cur_.base) + static_cast<qulonglong>(off),
             6, 16, QLatin1Char('0'))
        .toUpper();
}

void MemoryView::updateStatus()
{
    QString s;
    if (!backend_) {
        s = QStringLiteral("no backend");
    } else if (!hasRegion_ || cur_.size == 0) {
        s = QStringLiteral("no data");
    } else {
        const QString areaLbl = editArea_ == Area::Text
            ? QStringLiteral("Chars") : QString::fromStdString(cur_.name);
        if (!hasSel_) {
            s = areaLbl;
        } else {
            const int64_t s0 = selStart(), s1 = selEnd();
            if (s0 == s1) {
                s = QStringLiteral("%1: $%2").arg(areaLbl, displayAddr(s0));
                const QString sym = symbolAt(s0);
                if (!sym.isEmpty()) s += QStringLiteral(" : ") + sym;
            } else {
                s = QStringLiteral("%1: $%2 - $%3 (%4)")
                        .arg(areaLbl, displayAddr(s0), displayAddr(s1))
                        .arg(s1 - s0 + 1);
            }
        }
        if (!cur_.writable) s += QStringLiteral("  [read-only]");
        if (hoverOff_ >= 0 && hoverOff_ < static_cast<int64_t>(cur_.size)) {
            s += QStringLiteral("  |  $%1").arg(displayAddr(hoverOff_));
            const QString sym = symbolAt(hoverOff_);
            if (!sym.isEmpty()) s += QLatin1Char(' ') + sym;
        }
    }
    status_->setText(s);
}
