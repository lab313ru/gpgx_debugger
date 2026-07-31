#pragma once
#include <QSettings>
#include <QString>

// Where the debug views keep what they remember between sessions.
//
// A view is re-hosted: the same widget runs in the standalone app and inside
// IDA. Neither host may write into the other's configuration — an IDA plugin
// scribbling into IDA's own QSettings organisation would be rude and would also
// mean the two hosts silently disagree about zoom levels. So both point at one
// explicit INI file of our own.
//
// The unit of state is small and boring on purpose: zoom levels, the selected
// plane, which region a hex view was on, the watch list. Losing those is the
// difference between resuming an investigation and setting it up again.
namespace viewsettings {

// The settings file, created on first write. Lives beside the user's other
// application data, not next to the binary — the IDA plugin directory is often
// not writable.
QSettings& store();

// Convenience: read/write one value inside a named group, so callers do not
// each invent a key prefix and collide.
void  putInt(const QString& group, const QString& key, int value);
int   getInt(const QString& group, const QString& key, int fallback);
void  putBool(const QString& group, const QString& key, bool value);
bool  getBool(const QString& group, const QString& key, bool fallback);
void  putString(const QString& group, const QString& key, const QString& value);
QString getString(const QString& group, const QString& key, const QString& fallback = QString());

// Anything a view saves should survive a crash, not just a clean exit: a
// debugger host is precisely the kind of program that gets killed.
void flush();

// The directory holding the settings file, for state too big or too structured
// for an INI — a watch list, for instance, which already has a file format.
QString dataDir();

} // namespace viewsettings
