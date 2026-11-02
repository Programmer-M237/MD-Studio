#include <algorithm>
#include <iterator>
#include "Exporter.h"
#include "MathRenderer.h"
#include "OutlineParser.h"
#include "ZipWriter.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeDatabase>
#include <QPageSize>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <functional>
#include <memory>

namespace Exporter {
namespace {

// =============================================================================
//  1. Neutral document model, built by walking Qt's parsed Markdown document.
//     HTML and DOCX writers both consume it, so behaviour stays consistent.
// =============================================================================
struct Run {
    QString text;
    bool bold = false, italic = false, strike = false, code = false;
    QString href;       // hyperlink target
    QString image;      // image source (text is empty for image runs)
    // typeset LaTeX formula (image runs created by MathRenderer::applyMath)
    QImage  mathImage;
    qreal   mathW = 0, mathH = 0, mathDescent = 0;   // logical px; descent = baseline -> bottom edge
    QString mathTex;
};
using Runs = QList<Run>;

struct Blk {
    enum Type { Para, Heading, Item, Code, Rule, Table } type = Para;
    int  level   = 0;        // heading level, or list nesting depth (0-based)
    bool ordered = false;
    int  number  = 1;        // item number for ordered lists
    int  task    = -1;       // -1 none, 0 unchecked, 1 checked
    int  quote   = 0;        // block-quote nesting depth
    bool center  = false;    // centred paragraph (display math)
    Runs runs;
    QStringList code;        // code block lines
    QString lang;
    QList<QList<Runs>> table;   // rows -> cells -> runs (row 0 is the header)
};

// Parses Markdown into a QTextDocument.  `font` is the body font of the target format; LaTeX formulas
// are typeset in black relative to it (so they are readable on white paper / in Word).
std::unique_ptr<QTextDocument> makeDoc(const QString &md, const QString &baseDir, const QString &css, const QFont &font)
{
    auto doc = std::make_unique<QTextDocument>();
    doc->setBaseUrl(QUrl::fromLocalFile(baseDir + "/"));
    doc->setDefaultFont(font);
    if (!css.isEmpty()) doc->setDefaultStyleSheet(css);

    const AppSettings &S = AppSettings::instance();
    QVector<MathItem> items;
    QString prepared = stripFrontMatter(md);
    if (S.renderMath) prepared = MathRenderer::extractMath(prepared, items);
    doc->setMarkdown(normalizeEmphasis(prepared));
    if (!items.isEmpty()) {
        MathOptions o;
        o.color = Qt::black;
        o.scale = S.mathScale / 100.0;
        o.dpr   = 4.0;
        MathRenderer::applyMath(doc.get(), items, o);
    }
    return doc;
}

QFont bodyFont(int pixelSize)
{
    QFont f;
    f.setFamilies({"Segoe UI", "Helvetica Neue", "DejaVu Sans", "Sans Serif"});
    f.setPixelSize(pixelSize);
    return f;
}

// For print output the size MUST be in points: pixel sizes are not scaled to the 1200 dpi PDF device,
// point sizes are.  (Math is sized from QFontInfo, which converts points to screen pixels.)
QFont bodyFontPt(qreal points)
{
    QFont f;
    f.setFamilies({"Segoe UI", "Helvetica Neue", "DejaVu Sans", "Sans Serif"});
    f.setPointSizeF(points);
    return f;
}

Runs runsOf(const QTextBlock &b)
{
    Runs out;
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid()) continue;
        const QTextCharFormat cf = f.charFormat();
        Run r;
        if (cf.isImageFormat() && cf.hasProperty(MathRenderer::DescentProperty)) {
            const QTextImageFormat imf = cf.toImageFormat();
            r.mathImage   = b.document()->resource(QTextDocument::ImageResource, QUrl(imf.name())).value<QImage>();
            r.mathW       = imf.width();
            r.mathH       = imf.height();
            r.mathDescent = cf.doubleProperty(MathRenderer::DescentProperty);
            r.mathTex     = cf.stringProperty(MathRenderer::TexProperty);
        } else if (cf.isImageFormat()) {
            r.image = cf.toImageFormat().name();
        } else {
            r.text = f.text();
            r.text.replace(QChar::LineSeparator, '\n');
            r.text.replace(QChar::Nbsp, ' ');
        }
        r.bold   = cf.fontWeight() >= QFont::Bold;
        r.italic = cf.fontItalic();
        r.strike = cf.fontStrikeOut();
        r.code   = cf.fontFixedPitch();
        if (cf.isAnchor()) r.href = cf.anchorHref();
        out.append(r);
    }
    return out;
}

bool isCodeBlock(const QTextBlockFormat &bf)
{
    return bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage);
}

void collectBlock(const QTextBlock &b, QList<Blk> &out)
{
    const QTextBlockFormat bf = b.blockFormat();
    Blk blk;
    blk.quote = bf.intProperty(QTextFormat::BlockQuoteLevel);

    if (isCodeBlock(bf)) {
        QString t = b.text();
        t.replace(QChar::LineSeparator, '\n');
        // Consecutive code blocks that share a fence are one block.
        if (!out.isEmpty() && out.last().type == Blk::Code && out.last().quote == blk.quote
            && bf.property(QTextFormat::BlockCodeLanguage).toString() == out.last().lang) {
            out.last().code << t.split('\n');
            return;
        }
        blk.type = Blk::Code;
        blk.lang = bf.property(QTextFormat::BlockCodeLanguage).toString();
        blk.code = t.split('\n');
        out.append(blk);
        return;
    }
    if (bf.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
        blk.type = Blk::Rule;
        out.append(blk);
        return;
    }
    if (bf.headingLevel() > 0) {
        blk.type  = Blk::Heading;
        blk.level = bf.headingLevel();
        blk.runs  = runsOf(b);
        out.append(blk);
        return;
    }
    if (QTextList *list = b.textList()) {
        blk.type  = Blk::Item;
        blk.level = qMax(0, list->format().indent() - 1);
        switch (list->format().style()) {
        case QTextListFormat::ListDecimal: case QTextListFormat::ListLowerAlpha:
        case QTextListFormat::ListUpperAlpha: case QTextListFormat::ListLowerRoman:
        case QTextListFormat::ListUpperRoman:
            blk.ordered = true; break;
        default: break;
        }
        blk.number = list->itemNumber(b) + 1;
        if (bf.marker() == QTextBlockFormat::MarkerType::Checked)        blk.task = 1;
        else if (bf.marker() == QTextBlockFormat::MarkerType::Unchecked) blk.task = 0;
        blk.runs = runsOf(b);
        out.append(blk);
        return;
    }
    blk.type = Blk::Para;
    blk.center = bool(bf.alignment() & Qt::AlignHCenter);
    blk.runs = runsOf(b);
    if (blk.runs.isEmpty()) return;              // skip empty spacer blocks
    out.append(blk);
}

void walkFrame(QTextFrame *f, QList<Blk> &out)
{
    for (auto it = f->begin(); !it.atEnd(); ++it) {
        if (QTextFrame *cf = it.currentFrame()) {
            if (auto *t = qobject_cast<QTextTable *>(cf)) {
                Blk blk; blk.type = Blk::Table;
                for (int r = 0; r < t->rows(); ++r) {
                    QList<Runs> row;
                    for (int c = 0; c < t->columns(); ++c) {
                        Runs cell;
                        const QTextTableCell tc = t->cellAt(r, c);
                        for (auto ci = tc.begin(); !ci.atEnd(); ++ci) {
                            if (!ci.currentBlock().isValid()) continue;
                            if (!cell.isEmpty()) cell.append(Run{" "});
                            cell += runsOf(ci.currentBlock());
                        }
                        row.append(cell);
                    }
                    blk.table.append(row);
                }
                out.append(blk);
            } else {
                walkFrame(cf, out);
            }
        } else if (it.currentBlock().isValid()) {
            collectBlock(it.currentBlock(), out);
        }
    }
}

QList<Blk> buildModel(QTextDocument *doc)
{
    QList<Blk> out;
    walkFrame(doc->rootFrame(), out);
    return out;
}

// =============================================================================
//  2. HTML writer
// =============================================================================
QString dataUriFor(const QString &src, const QString &baseDir)
{
    if (src.startsWith("data:") || src.startsWith("http://") || src.startsWith("https://")) return src;
    QString path = src.startsWith("file:") ? QUrl(src).toLocalFile() : src;
    if (QFileInfo(path).isRelative()) path = QDir(baseDir).filePath(path);
    QFile f(path);
    if (!f.exists() || f.size() > 12 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) return src;
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
    return "data:" + mime + ";base64," + QString::fromLatin1(f.readAll().toBase64());
}

QString runsHtml(const Runs &rs, const QString &baseDir, bool ignoreBold = false)
{
    QString h;
    for (const Run &r : rs) {
        QString t;
        if (!r.mathImage.isNull()) {
            QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly); r.mathImage.save(&buf, "PNG");
            t = QString("<img class=\"math\" alt=\"%1\" style=\"height:%2px;vertical-align:-%3px\" src=\"data:image/png;base64,%4\">")
                    .arg(r.mathTex.toHtmlEscaped()).arg(r.mathH, 0, 'f', 2).arg(r.mathDescent, 0, 'f', 2)
                    .arg(QString::fromLatin1(png.toBase64()));
        } else if (!r.image.isEmpty()) {
            t = QString("<img src=\"%1\" alt=\"\">").arg(dataUriFor(r.image, baseDir).toHtmlEscaped());
        } else {
            t = r.text.toHtmlEscaped().replace("\n", "<br>\n");
            if (r.code)   t = "<code>" + t + "</code>";
            if (r.bold && !ignoreBold) t = "<strong>" + t + "</strong>";
            if (r.italic) t = "<em>" + t + "</em>";
            if (r.strike) t = "<del>" + t + "</del>";
        }
        if (!r.href.isEmpty()) t = QString("<a href=\"%1\">%2</a>").arg(r.href.toHtmlEscaped(), t);
        h += t;
    }
    return h;
}

QString modelToHtml(const QList<Blk> &blocks, const QString &baseDir)
{
    QString out;
    QStringList stack;                // open list tags
    int quoteDepth = 0;

    auto closeLists = [&](int toSize) {
        while (stack.size() > toSize) out += "</li></" + stack.takeLast() + ">\n";
    };

    for (const Blk &b : blocks) {
        // blockquote nesting
        if (b.type != Blk::Item || b.quote != quoteDepth) {
            if (b.quote != quoteDepth) closeLists(0);
        }
        while (quoteDepth < b.quote) { out += "<blockquote>\n"; ++quoteDepth; }
        while (quoteDepth > b.quote) { out += "</blockquote>\n"; --quoteDepth; }

        if (b.type != Blk::Item) closeLists(0);

        switch (b.type) {
        case Blk::Heading: {
            const QString plain = [&] { QString s; for (const Run &r : b.runs) s += r.text; return s; }();
            out += QString("<h%1 id=\"%2\">%3</h%1>\n").arg(b.level).arg(slugify(plain), runsHtml(b.runs, baseDir, true));
            break;
        }
        case Blk::Para:
            out += QString(b.center ? "<p style=\"text-align:center\">" : "<p>") + runsHtml(b.runs, baseDir) + "</p>\n";
            break;
        case Blk::Rule:
            out += "<hr>\n";
            break;
        case Blk::Code: {
            const QString cls = b.lang.isEmpty() ? QString() : QString(" class=\"language-%1\"").arg(b.lang.toHtmlEscaped());
            out += "<pre><code" + cls + ">" + b.code.join("\n").toHtmlEscaped() + "</code></pre>\n";
            break;
        }
        case Blk::Item: {
            const int depth = b.level + 1;
            const QString tag = b.ordered ? "ol" : "ul";
            while (stack.size() > depth) out += "</li></" + stack.takeLast() + ">\n";
            if (stack.size() == depth && stack.last() != tag) out += "</li></" + stack.takeLast() + ">\n";
            if (stack.size() == depth) out += "</li>\n";
            while (stack.size() < depth) {
                stack << tag;
                out += "<" + tag + ">\n";
                if (stack.size() < depth) out += "<li>";
            }
            out += "<li>";
            if (b.task >= 0) out += QString("<input type=\"checkbox\" disabled%1> ").arg(b.task ? " checked" : "");
            out += runsHtml(b.runs, baseDir);
            break;
        }
        case Blk::Table: {
            out += "<table>\n";
            for (int r = 0; r < b.table.size(); ++r) {
                out += "<tr>";
                for (const Runs &cell : b.table[r])
                    out += (r == 0 ? "<th>" : "<td>") + runsHtml(cell, baseDir, r == 0) + (r == 0 ? "</th>" : "</td>");
                out += "</tr>\n";
            }
            out += "</table>\n";
            break;
        }
        }
    }
    closeLists(0);
    while (quoteDepth-- > 0) out += "</blockquote>\n";
    return out;
}

// =============================================================================
//  3. DOCX writer (WordprocessingML)
// =============================================================================
class DocxBuilder {
public:
    DocxBuilder(const QString &baseDir, const AppSettings &s) : m_base(baseDir), m_s(s) {}

    QByteArray build(const QList<Blk> &blocks)
    {
        for (const Blk &b : blocks) addBlock(b);

        const int mar = int(m_s.pdfMarginMm * 56.7);
        const bool letter = m_s.pdfPageSize == "Letter", legal = m_s.pdfPageSize == "Legal", a5 = m_s.pdfPageSize == "A5";
        const int w = letter ? 12240 : legal ? 12240 : a5 ? 8392  : 11906;
        const int h = letter ? 15840 : legal ? 20160 : a5 ? 11906 : 16838;

        const QString document = QString(R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><w:body>%1<w:sectPr><w:pgSz w:w="%2" w:h="%3"/><w:pgMar w:top="%4" w:right="%4" w:bottom="%4" w:left="%4" w:header="708" w:footer="708" w:gutter="0"/></w:sectPr></w:body></w:document>)XML")
            .arg(m_body, QString::number(w), QString::number(h), QString::number(mar));

        ZipWriter zip;
        zip.add("[Content_Types].xml", contentTypes().toUtf8());
        zip.add("_rels/.rels", QByteArray(R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)XML"));
        zip.add("word/document.xml", document.toUtf8());
        zip.add("word/styles.xml", stylesXml().toUtf8());
        zip.add("word/numbering.xml", numberingXml().toUtf8());
        zip.add("word/_rels/document.xml.rels", relsXml().toUtf8());
        for (const auto &m : std::as_const(m_media)) zip.add("word/media/" + m.first, m.second);
        return zip.finish();
    }

private:
    QString m_base; AppSettings m_s;
    QString m_body;
    QStringList m_rels;
    QList<QPair<QString, QByteArray>> m_media;
    QStringList m_nums;
    int m_rid = 10, m_img = 1, m_numId = 3;
    int m_cur[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    static QString esc(const QString &s) { return s.toHtmlEscaped(); }

    QString imageXml(const QString &src)
    {
        QString path = src.startsWith("file:") ? QUrl(src).toLocalFile() : src;
        if (QFileInfo(path).isRelative()) path = QDir(m_base).filePath(path);
        QImage img(path);
        if (img.isNull())   // remote or missing image: leave a readable placeholder
            return QString("<w:r><w:t xml:space=\"preserve\">[image: %1]</w:t></w:r>").arg(esc(src));

        QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly); img.save(&buf, "PNG");
        const QString name = QString("image%1.png").arg(m_img);
        const QString rid  = QString("rId%1").arg(m_rid++);
        m_media.append({name, png});
        m_rels << QString("<Relationship Id=\"%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/%2\"/>").arg(rid, name);

        qint64 cx = qint64(img.width()) * 9525, cy = qint64(img.height()) * 9525;
        const qint64 maxW = 5486400;                       // 6 inches
        if (cx > maxW) { cy = cy * maxW / cx; cx = maxW; }
        const int id = m_img++;
        return QString(R"XML(<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="%1" cy="%2"/><wp:docPr id="%3" name="Picture %3"/><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic><pic:nvPicPr><pic:cNvPr id="%3" name="%4"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed="%5"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="%1" cy="%2"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>)XML")
            .arg(cx).arg(cy).arg(id).arg(esc(name), rid);
    }

    // A typeset formula: embedded PNG, lowered so its baseline sits on the text baseline.
    QString mathImageXml(const Run &r)
    {
        QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly); r.mathImage.save(&buf, "PNG");
        const QString name = QString("image%1.png").arg(m_img);
        const QString rid  = QString("rId%1").arg(m_rid++);
        m_media.append({name, png});
        m_rels << QString("<Relationship Id=\"%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/%2\"/>").arg(rid, name);
        const qint64 cx = qint64(r.mathW * 9525), cy = qint64(r.mathH * 9525);     // 96 dpi pixels -> EMU
        const int id = m_img++;
        const int lower = int(r.mathDescent * 1.5);                                // px -> half-points
        return QString(R"XML(<w:r><w:rPr><w:position w:val="-%6"/></w:rPr><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="%1" cy="%2"/><wp:docPr id="%3" name="Formula %3" descr="%4"/><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic><pic:nvPicPr><pic:cNvPr id="%3" name="formula%3"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed="%5"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="%1" cy="%2"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>)XML")
            .arg(QString::number(cx), QString::number(cy), QString::number(id), esc(r.mathTex), rid, QString::number(lower));
    }

    // boldMode: 0 = as-is, 1 = force bold, -1 = ignore bold flags (headings)
    QString runXml(const Run &r, int boldMode)
    {
        if (!r.mathImage.isNull()) return mathImageXml(r);
        if (!r.image.isEmpty()) return imageXml(r.image);
        const bool linked = !r.href.isEmpty() && !r.href.startsWith('#');

        QString rpr;
        if (linked) rpr += "<w:rStyle w:val=\"Hyperlink\"/>";
        if (r.code) rpr += "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/>";
        if (boldMode == 1 || (boldMode == 0 && r.bold)) rpr += "<w:b/>";
        if (r.italic) rpr += "<w:i/>";
        if (r.strike) rpr += "<w:strike/>";
        if (r.code)   rpr += "<w:sz w:val=\"20\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"EEEEEE\"/>";
        const QString props = rpr.isEmpty() ? QString() : "<w:rPr>" + rpr + "</w:rPr>";

        QString xml;
        const QStringList pieces = r.text.split('\n');
        for (int i = 0; i < pieces.size(); ++i) {
            if (i > 0) xml += "<w:r><w:br/></w:r>";
            if (pieces[i].isEmpty()) continue;
            xml += QString("<w:r>%1<w:t xml:space=\"preserve\">%2</w:t></w:r>").arg(props, esc(pieces[i]));
        }
        if (linked) {
            const QString rid = QString("rId%1").arg(m_rid++);
            m_rels << QString("<Relationship Id=\"%1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"%2\" TargetMode=\"External\"/>").arg(rid, esc(r.href));
            xml = QString("<w:hyperlink r:id=\"%1\" w:history=\"1\">%2</w:hyperlink>").arg(rid, xml);
        }
        return xml;
    }

    QString runsXml(const Runs &rs, int boldMode = 0)
    {
        QString x; for (const Run &r : rs) x += runXml(r, boldMode); return x;
    }

    void addBlock(const Blk &b)
    {
        if (b.type != Blk::Item) std::fill(std::begin(m_cur), std::end(m_cur), 0);
        const QString quoteInd = b.quote ? QString("<w:pStyle w:val=\"Quote\"/>")
                                           + (b.quote > 1 ? QString("<w:ind w:left=\"%1\"/>").arg(360 * (b.quote - 1)) : QString()) : QString();

        switch (b.type) {
        case Blk::Heading:
            m_body += QString("<w:p><w:pPr><w:pStyle w:val=\"Heading%1\"/></w:pPr>%2</w:p>")
                          .arg(qBound(1, b.level, 6)).arg(runsXml(b.runs, -1));
            break;
        case Blk::Para:
        {
            const QString ppr = quoteInd + (b.center ? "<w:jc w:val=\"center\"/>" : QString());
            m_body += QString("<w:p>%1%2</w:p>").arg(ppr.isEmpty() ? QString() : "<w:pPr>" + ppr + "</w:pPr>", runsXml(b.runs));
        }
            break;
        case Blk::Rule:
            m_body += "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"999999\"/></w:pBdr></w:pPr></w:p>";
            break;
        case Blk::Code:
            for (const QString &line : b.code)
                m_body += QString("<w:p><w:pPr><w:pStyle w:val=\"SourceCode\"/></w:pPr><w:r><w:t xml:space=\"preserve\">%1</w:t></w:r></w:p>").arg(esc(line));
            m_body += "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr></w:p>";
            break;
        case Blk::Item: {
            const int lvl = qBound(0, b.level, 8);
            int numId = 1;
            if (b.ordered) {
                if (b.number <= 1 || m_cur[lvl] == 0) {
                    m_cur[lvl] = m_numId++;
                    QString ov;
                    for (int i = 0; i < 9; ++i)
                        ov += QString("<w:lvlOverride w:ilvl=\"%1\"><w:startOverride w:val=\"1\"/></w:lvlOverride>").arg(i);
                    m_nums << QString("<w:num w:numId=\"%1\"><w:abstractNumId w:val=\"1\"/>%2</w:num>").arg(m_cur[lvl]).arg(ov);
                }
                numId = m_cur[lvl];
            }
            QString prefix;
            if (b.task >= 0) prefix = QString("<w:r><w:t xml:space=\"preserve\">%1 </w:t></w:r>").arg(b.task ? QString::fromUtf8("\u2611") : QString::fromUtf8("\u2610"));
            m_body += QString("<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"%1\"/><w:numId w:val=\"%2\"/></w:numPr></w:pPr>%3%4</w:p>")
                          .arg(lvl).arg(numId).arg(prefix, runsXml(b.runs));
            break;
        }
        case Blk::Table: {
            const int cols = b.table.isEmpty() ? 1 : int(b.table[0].size());
            QString grid;
            for (int c = 0; c < cols; ++c) grid += QString("<w:gridCol w:w=\"%1\"/>").arg(9000 / cols);
            QString rows;
            for (int r = 0; r < b.table.size(); ++r) {
                QString cells;
                for (const Runs &cell : b.table[r]) {
                    const QString shade = r == 0 ? "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"D9E2F3\"/>" : QString();
                    cells += QString("<w:tc><w:tcPr><w:tcW w:w=\"%1\" w:type=\"dxa\"/>%2</w:tcPr><w:p>%3</w:p></w:tc>")
                                 .arg(9000 / cols).arg(shade, runsXml(cell, r == 0 ? 1 : 0));
                }
                rows += QString("<w:tr>%1%2</w:tr>").arg(r == 0 ? "<w:trPr><w:tblHeader/></w:trPr>" : "", cells);
            }
            m_body += QString("<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid>%1</w:tblGrid>%2</w:tbl><w:p/>")
                          .arg(grid, rows);
            break;
        }
        }
    }

    QString contentTypes() const
    {
        return R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Default Extension="png" ContentType="image/png"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/><Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/></Types>)XML";
    }

    QString relsXml() const
    {
        return QString(R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>%1</Relationships>)XML")
            .arg(m_rels.join(""));
    }

    QString numberingXml() const
    {
        QString bullets, ordered;
        const char *bch[] = {"\u2022", "\u25E6", "\u25AA"};
        const char *ofmt[] = {"decimal", "lowerLetter", "lowerRoman"};
        for (int i = 0; i < 9; ++i) {
            const int left = 720 + 360 * i;
            bullets += QString("<w:lvl w:ilvl=\"%1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"%2\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%3\" w:hanging=\"360\"/></w:pPr></w:lvl>")
                           .arg(i).arg(QString::fromUtf8(bch[i % 3])).arg(left);
            // NB: multi-argument arg() fills every placeholder in ONE pass, so the literal
            // "%1" level text we insert is not mistaken for a placeholder itself.
            ordered += QString("<w:lvl w:ilvl=\"%1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%2\"/><w:lvlText w:val=\"%3\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%4\" w:hanging=\"360\"/></w:pPr></w:lvl>")
                           .arg(QString::number(i), QString::fromLatin1(ofmt[i % 3]),
                                "%" + QString::number(i + 1) + ".", QString::number(left));
        }
        return QString(R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:abstractNum w:abstractNumId="0"><w:multiLevelType w:val="hybridMultilevel"/>%1</w:abstractNum><w:abstractNum w:abstractNumId="1"><w:multiLevelType w:val="hybridMultilevel"/>%2</w:abstractNum><w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>%3</w:numbering>)XML")
            .arg(bullets, ordered, m_nums.join(""));
    }

    QString stylesXml() const
    {
        QString heads;
        const int sizes[] = {36, 30, 26, 24, 22, 22};
        for (int i = 1; i <= 6; ++i)
            heads += QString("<w:style w:type=\"paragraph\" w:styleId=\"Heading%1\"><w:name w:val=\"heading %1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before=\"%2\" w:after=\"120\"/><w:outlineLvl w:val=\"%3\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"1F3864\"/><w:sz w:val=\"%4\"/></w:rPr></w:style>")
                         .arg(i).arg(i <= 2 ? 320 : 220).arg(i - 1).arg(sizes[i - 1]);
        return QString(R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="Calibri" w:cs="Calibri"/><w:sz w:val="22"/><w:szCs w:val="22"/><w:lang w:val="en-US"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="120" w:line="264" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults><w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>%1<w:style w:type="paragraph" w:styleId="Quote"><w:name w:val="Quote"/><w:basedOn w:val="Normal"/><w:qFormat/><w:pPr><w:pBdr><w:left w:val="single" w:sz="18" w:space="8" w:color="BBBBBB"/></w:pBdr></w:pPr><w:rPr><w:i/><w:color w:val="595959"/></w:rPr></w:style><w:style w:type="paragraph" w:styleId="SourceCode"><w:name w:val="Source Code"/><w:basedOn w:val="Normal"/><w:qFormat/><w:pPr><w:shd w:val="clear" w:color="auto" w:fill="F3F3F3"/><w:spacing w:after="0" w:line="240" w:lineRule="auto"/></w:pPr><w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:cs="Consolas"/><w:sz w:val="20"/></w:rPr></w:style><w:style w:type="paragraph" w:styleId="ListParagraph"><w:name w:val="List Paragraph"/><w:basedOn w:val="Normal"/><w:qFormat/><w:pPr><w:spacing w:after="40"/></w:pPr></w:style><w:style w:type="character" w:styleId="Hyperlink"><w:name w:val="Hyperlink"/><w:rPr><w:color w:val="0563C1"/><w:u w:val="single"/></w:rPr></w:style><w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/><w:tblPr><w:tblBorders><w:top w:val="single" w:sz="4" w:space="0" w:color="999999"/><w:left w:val="single" w:sz="4" w:space="0" w:color="999999"/><w:bottom w:val="single" w:sz="4" w:space="0" w:color="999999"/><w:right w:val="single" w:sz="4" w:space="0" w:color="999999"/><w:insideH w:val="single" w:sz="4" w:space="0" w:color="999999"/><w:insideV w:val="single" w:sz="4" w:space="0" w:color="999999"/></w:tblBorders><w:tblCellMar><w:left w:w="100" w:type="dxa"/><w:right w:w="100" w:type="dxa"/></w:tblCellMar></w:tblPr></w:style></w:styles>)XML").arg(heads);
    }
};

QPageSize pageSizeFor(const QString &name)
{
    if (name == "Letter") return QPageSize(QPageSize::Letter);
    if (name == "Legal")  return QPageSize(QPageSize::Legal);
    if (name == "A5")     return QPageSize(QPageSize::A5);
    return QPageSize(QPageSize::A4);
}

bool writeFile(const QString &path, const QByteArray &data, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (err) *err = f.errorString(); return false; }
    f.write(data);
    return true;
}

const char *kHtmlCss = R"CSS(
body{font-family:-apple-system,"Segoe UI",Helvetica,Arial,sans-serif;line-height:1.65;max-width:860px;margin:2rem auto;padding:0 1.2rem;color:#24292f;background:#fff}
h1,h2{border-bottom:1px solid #d0d7de;padding-bottom:.3em}
code{background:#f3f4f6;padding:.15em .35em;border-radius:4px;font-family:Consolas,"SFMono-Regular",Menlo,monospace;font-size:.9em}
pre{background:#f6f8fa;padding:1em;overflow:auto;border-radius:6px}pre code{background:none;padding:0}
blockquote{border-left:4px solid #d0d7de;margin:0;padding:0 1em;color:#57606a}
table{border-collapse:collapse}th,td{border:1px solid #d0d7de;padding:6px 13px}th{background:#f6f8fa}
img{max-width:100%}hr{border:0;border-top:1px solid #d0d7de}a{color:#0969da}
@media (prefers-color-scheme:dark){img.math{filter:invert(1) hue-rotate(180deg)}body{background:#0d1117;color:#e6edf3}h1,h2{border-color:#30363d}code{background:#262c36}pre{background:#161b22}
blockquote{border-color:#30363d;color:#8b949e}th,td{border-color:#30363d}th{background:#161b22}hr{border-color:#30363d}a{color:#58a6ff}}
)CSS";

} // namespace

// =============================================================================
//  Public API
// =============================================================================
bool toHtml(const QString &md, const QString &title, const QString &baseDir, const QString &out, QString *err)
{
    auto doc = makeDoc(md, baseDir, QString(), bodyFont(16));
    const QString html = QString("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                                 "<title>%1</title>\n<style>%2</style>\n</head>\n<body>\n%3</body>\n</html>\n")
                             .arg(title.toHtmlEscaped(), kHtmlCss, modelToHtml(buildModel(doc.get()), baseDir));
    return writeFile(out, html.toUtf8(), err);
}

bool toDocx(const QString &md, const QString &baseDir, const AppSettings &s, const QString &out, QString *err)
{
    auto doc = makeDoc(md, baseDir, QString(), bodyFont(15));       // 15 px ~ 11 pt, the DOCX body size
    DocxBuilder builder(baseDir, s);
    return writeFile(out, builder.build(buildModel(doc.get())), err);
}

bool toPdf(const QString &md, const QString &baseDir, const AppSettings &s, const QString &out, QString *err)
{
    const QString css = "a{color:#0969da;} h1,h2{color:#1f2328;} code{background-color:#f0f0f0;color:#a31515;}"
                        "pre{background-color:#f6f8fa;} blockquote{color:#57606a;} th{background-color:#f0f0f0;}";
    auto doc = makeDoc(md, baseDir, css, bodyFontPt(11));
    doc->setDocumentMargin(0);

    // QTextDocument::print() paints text in the APPLICATION palette colour - light grey in the dark
    // theme, i.e. invisible on white paper.  Give the whole document explicit print colours.
    {
        QTextCursor all(doc.get());
        all.select(QTextCursor::Document);
        QTextCharFormat ink; ink.setForeground(QColor("#1f2328"));
        all.mergeCharFormat(ink);

        struct R { int from, to; };
        QVector<R> links;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().isValid() && it.fragment().charFormat().isAnchor())
                    links.append({it.fragment().position(), it.fragment().position() + it.fragment().length()});
        QTextCharFormat linkFmt; linkFmt.setForeground(QColor("#0969da")); linkFmt.setFontUnderline(true);
        for (const R &r : links) {
            QTextCursor c(doc.get());
            c.setPosition(r.from); c.setPosition(r.to, QTextCursor::KeepAnchor);
            c.mergeCharFormat(linkFmt);
        }
    }

    // Give tables visible borders in print.
    std::function<void(QTextFrame *)> walk = [&](QTextFrame *fr) {
        for (auto it = fr->begin(); !it.atEnd(); ++it)
            if (QTextFrame *cf = it.currentFrame()) {
                if (auto *t = qobject_cast<QTextTable *>(cf)) {
                    QTextTableFormat tf = t->format();
                    tf.setBorder(0.6); tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                    tf.setBorderBrush(QColor("#999999")); tf.setCellPadding(4); tf.setCellSpacing(0);
                    t->setFormat(tf);
                } else walk(cf);
            }
    };
    walk(doc->rootFrame());

    QPdfWriter pdf(out);
    pdf.setPageSize(pageSizeFor(s.pdfPageSize));
    pdf.setPageMargins(QMarginsF(s.pdfMarginMm, s.pdfMarginMm, s.pdfMarginMm, s.pdfMarginMm), QPageLayout::Millimeter);
    pdf.setCreator("MD Studio");
    pdf.setTitle(QFileInfo(out).completeBaseName());
    doc->print(&pdf);
    if (!QFileInfo::exists(out)) { if (err) *err = "The PDF file could not be written."; return false; }
    return true;
}

bool toOdt(const QString &md, const QString &baseDir, const QString &out, QString *err)
{
    auto doc = makeDoc(md, baseDir, QString(), bodyFont(16));
    QTextDocumentWriter w(out, "ODF");
    if (!w.write(doc.get())) { if (err) *err = "The ODT file could not be written."; return false; }
    return true;
}

bool toText(const QString &md, const QString &baseDir, const QString &out, QString *err)
{
    auto doc = makeDoc(md, baseDir, QString(), bodyFont(16));
    return writeFile(out, doc->toPlainText().toUtf8(), err);
}

// ---- Pandoc -----------------------------------------------------------------
QList<PandocFormat> pandocFormats()
{
    return {
        {"Word document (.docx)",        "docx",      "docx"},
        {"OpenDocument text (.odt)",     "odt",       "odt"},
        {"PDF via LaTeX (.pdf)",         "pdf",       "pdf"},
        {"HTML5 standalone (.html)",     "html5",     "html"},
        {"EPUB e-book (.epub)",          "epub",      "epub"},
        {"LaTeX (.tex)",                 "latex",     "tex"},
        {"Rich Text Format (.rtf)",      "rtf",       "rtf"},
        {"PowerPoint slides (.pptx)",    "pptx",      "pptx"},
        {"reStructuredText (.rst)",      "rst",       "rst"},
        {"AsciiDoc (.adoc)",             "asciidoc",  "adoc"},
        {"MediaWiki (.wiki)",            "mediawiki", "wiki"},
        {"Org mode (.org)",              "org",       "org"},
        {"Textile (.textile)",           "textile",   "textile"},
        {"Plain text (.txt)",            "plain",     "txt"},
    };
}

QString findPandoc(const AppSettings &s)
{
    if (!s.pandocPath.isEmpty() && QFileInfo(s.pandocPath).isExecutable()) return s.pandocPath;
    QString p = QStandardPaths::findExecutable("pandoc");
    if (!p.isEmpty()) return p;
#ifdef Q_OS_WIN
    const QStringList guesses = {
        qEnvironmentVariable("LOCALAPPDATA") + "/Pandoc/pandoc.exe",
        "C:/Program Files/Pandoc/pandoc.exe",
        "C:/Program Files (x86)/Pandoc/pandoc.exe",
    };
    for (const QString &g : guesses) if (QFileInfo::exists(g)) return g;
#endif
    return {};
}

bool viaPandoc(const QString &md, const QString &title, const QString &baseDir, const AppSettings &s,
               const PandocFormat &fmt, const QString &out, QString *err)
{
    const QString exe = findPandoc(s);
    if (exe.isEmpty()) { if (err) *err = "Pandoc was not found. Install it from pandoc.org or set its path in Settings > Export."; return false; }

    QStringList args = {"-f", "markdown+task_lists+strikeout+pipe_tables", "-t", fmt.pandocName, "-o", out};
    if (fmt.pandocName == "html5") args << "--standalone" << "--metadata" << ("pagetitle=" + title);
    if (fmt.pandocName == "latex") args << "--standalone";

    QProcess p;
    p.setWorkingDirectory(baseDir);                 // so relative image paths resolve
    p.start(exe, args);
    if (!p.waitForStarted(5000)) { if (err) *err = "Could not start pandoc."; return false; }
    p.write(md.toUtf8());
    p.closeWriteChannel();
    if (!p.waitForFinished(180000)) { p.kill(); if (err) *err = "Pandoc timed out."; return false; }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (err) *err = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

} // namespace Exporter
