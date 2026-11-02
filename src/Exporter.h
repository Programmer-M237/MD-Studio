#pragma once
// =============================================================================
//  Exporter.h - Markdown -> HTML / PDF / DOCX / ODT / TXT (built in) and any
//  other format through an installed Pandoc.
// =============================================================================
#include <QList>
#include <QString>
#include "Settings.h"

namespace Exporter {

struct PandocFormat { QString label; QString pandocName; QString extension; };

QList<PandocFormat> pandocFormats();
/// Full path to pandoc (settings override, PATH, common install dirs) or empty.
QString findPandoc(const AppSettings &s);

// Every function returns true on success; on failure `*err` explains why.
bool toHtml (const QString &md, const QString &title, const QString &baseDir, const QString &out, QString *err);
bool toPdf  (const QString &md, const QString &baseDir, const AppSettings &s,   const QString &out, QString *err);
bool toDocx (const QString &md, const QString &baseDir, const AppSettings &s,   const QString &out, QString *err);
bool toOdt  (const QString &md, const QString &baseDir, const QString &out, QString *err);
bool toText (const QString &md, const QString &baseDir, const QString &out, QString *err);
bool viaPandoc(const QString &md, const QString &title, const QString &baseDir, const AppSettings &s,
               const PandocFormat &fmt, const QString &out, QString *err);

} // namespace Exporter
