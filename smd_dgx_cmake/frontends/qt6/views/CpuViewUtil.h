#pragma once
#include <QColor>
#include <QFont>
#include <QString>
#include <cstdint>

// Small helpers the Debugger dock's parts share: hex text, the monospace font
// and colours derived from the palette.
namespace cpuview {

inline QString hex(uint32_t v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

// A colour part-way from a to b, so marks stay readable under any palette.
inline QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t);
}

inline QFont monoFont() { return QFont(QStringLiteral("Courier New"), 9); }

// Hex as typed into an address field: an optional "$" or "0x", then digits.
inline bool parseHexField(QString t, uint32_t& v)
{
    t = t.trimmed();
    if (t.startsWith(QLatin1Char('$'))) t.remove(0, 1);
    else if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) t.remove(0, 2);
    bool ok = false;
    v = t.toUInt(&ok, 16);
    return ok && !t.isEmpty();
}

} // namespace cpuview
