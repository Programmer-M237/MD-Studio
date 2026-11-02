#pragma once
// =============================================================================
//  MathRenderer.h - LaTeX math support for the Markdown preview and exporters.
//
//  Qt has no LaTeX engine, so this module contains a small TeX-style math
//  typesetter:  LaTeX source -> parse tree -> boxes (TeXbook layout rules using
//  the OpenType MATH constants of the embedded Latin Modern Math font) -> QImage.
//
//  Markdown integration works in three steps (see PreviewView / Exporter):
//    1. extractMath()  replaces $..$, $$..$$, \(..\), \[..\] and ```math blocks
//                      with placeholder words BEFORE Markdown parsing
//    2. QTextDocument::setMarkdown() runs on the placeholder text
//    3. applyMath()    swaps each placeholder for a rendered inline image
// =============================================================================
#include <QColor>
#include <QImage>
#include <QString>
#include <QTextFormat>
#include <QVector>

class QTextDocument;

struct MathItem {
    QString tex;
    bool    display = false;    // block / display-style formula
};

struct MathOptions {
    QColor color = Qt::black;
    qreal  emPx  = 16.0;        // 1 em in logical pixels (applyMath() overrides it with the local font size)
    qreal  scale = 1.0;         // user "math size" multiplier
    qreal  dpr   = 3.0;         // render resolution multiplier (sharpness)
};

struct MathImage {
    QImage  image;              // devicePixelRatio set, logical size = width x height
    qreal   width   = 0;
    qreal   height  = 0;
    qreal   ascent  = 0;        // distance from the top edge to the baseline
    qreal   descent = 0;        // distance from the baseline to the bottom edge
    bool    ok      = false;
    QString error;
};

namespace MathRenderer {

// Custom QTextFormat properties stored on inline math images.
constexpr int DescentProperty = QTextFormat::UserProperty + 1;
constexpr int TexProperty     = QTextFormat::UserProperty + 2;
constexpr int DisplayProperty = QTextFormat::UserProperty + 3;

MathImage render(const QString &tex, bool display, const MathOptions &opt);

QString extractMath(const QString &markdown, QVector<MathItem> &items);
void    applyMath(QTextDocument *doc, const QVector<MathItem> &items, const MathOptions &opt);

} // namespace MathRenderer
