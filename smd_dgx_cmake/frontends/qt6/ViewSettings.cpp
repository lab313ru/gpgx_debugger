#include "ViewSettings.h"

#include <QDir>
#include <QStandardPaths>

namespace viewsettings {
namespace {

QString dataDirImpl()
{
    // GenericConfigLocation, not AppDataLocation: the latter is derived from
    // the running application's organisation and name, so inside IDA it would
    // resolve under IDA's own directory and the two hosts would keep separate
    // settings. This one is the same place either way.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (dir.isEmpty()) dir = QDir::homePath();
    dir += QStringLiteral("/smd_dgx");
    QDir().mkpath(dir);
    return dir;
}

} // namespace

QString dataDir() { return dataDirImpl(); }

QSettings& store()
{
    static QSettings s(dataDirImpl() + QStringLiteral("/views.ini"), QSettings::IniFormat);
    return s;
}

void putInt(const QString& group, const QString& key, int value)
{
    store().setValue(group + QLatin1Char('/') + key, value);
}

int getInt(const QString& group, const QString& key, int fallback)
{
    return store().value(group + QLatin1Char('/') + key, fallback).toInt();
}

void putBool(const QString& group, const QString& key, bool value)
{
    store().setValue(group + QLatin1Char('/') + key, value);
}

bool getBool(const QString& group, const QString& key, bool fallback)
{
    return store().value(group + QLatin1Char('/') + key, fallback).toBool();
}

void putString(const QString& group, const QString& key, const QString& value)
{
    store().setValue(group + QLatin1Char('/') + key, value);
}

QString getString(const QString& group, const QString& key, const QString& fallback)
{
    return store().value(group + QLatin1Char('/') + key, fallback).toString();
}

void flush() { store().sync(); }

} // namespace viewsettings
