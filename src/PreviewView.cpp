#include "PreviewView.h"
#include "MathRenderer.h"
#include "OutlineParser.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QFileInfo>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>
#include <functional>

namespace {
// Relative size of h1..h6 compared with the body text.
const double kHeadingFactor[6] = {2.0, 1.6, 1.35, 1.15, 1.0, 0.9};
}

PreviewView::PreviewView(QWidget *parent) : QTextBrowser(parent)
{
    setFrameShape(QFrame::NoFrame);
    setOpenLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, &PreviewView::handleLink);
    m_theme = Theme::dark_();
}

// -----------------------------------------------------------------------------
//  Style
// -----------------------------------------------------------------------------
void PreviewView::applyStyle(const AppSettings &s, const Theme &appTheme)
{
    m_s = s;
    m_theme = s.previewTheme == "Light" ? Theme::light_()
            : s.previewTheme == "Dark"  ? Theme::dark_()
            : appTheme;

    QFont f = font();
    if (s.previewFontFamily.isEmpty())
        f.setFamilies({"Segoe UI", "Helvetica Neue", "Noto Sans", "DejaVu Sans", "Sans Serif"});
    else
        f.setFamily(s.previewFontFamily);
    f.setPointSize(s.previewFontSize);
    setFont(f);
    document()->setDefaultFont(f);
    document()->setDefaultStyleSheet(m_theme.previewCss());
    document()->setDocumentMargin(s.previewMargin);

    QPalette p = palette();
    p.setColor(QPalette::Base, m_theme.editorBg);
    p.setColor(QPalette::Text, m_theme.editorFg);
    setPalette(p);
    updateViewportMargins();
}

// Constrain the page to a maximum width by centring it with viewport margins.
void PreviewView::updateViewportMargins()
{
    int margin = 0;
    if (m_s.previewMaxWidth > 0) {
        const int sb = verticalScrollBar()->isVisible() ? verticalScrollBar()->width() : 0;
        const int avail = width() - sb - 2 * frameWidth();
        margin = qMax(0, (avail - m_s.previewMaxWidth) / 2);
    }
    setViewportMargins(margin, 0, margin, 0);
}

void PreviewView::resizeEvent(QResizeEvent *e)
{
    QTextBrowser::resizeEvent(e);
    updateViewportMargins();
}

// -----------------------------------------------------------------------------
//  Rendering
// -----------------------------------------------------------------------------
void PreviewView::setMarkdownText(const QString &markdown, const QString &baseDir)
{
    m_baseDir = baseDir;
    const int scroll = verticalScrollBar()->value();

    // 1. strip front matter, 2. pull LaTeX out (placeholders), 3. fix _emphasis_
    QVector<MathItem> mathItems;
    QString prepared = stripFrontMatter(markdown);
    if (m_s.renderMath) prepared = MathRenderer::extractMath(prepared, mathItems);
    prepared = normalizeEmphasis(prepared);

    setSearchPaths({baseDir});
    document()->setBaseUrl(QUrl::fromLocalFile(baseDir + "/"));
    document()->setMarkdown(prepared);        // GitHub dialect: tables, task lists, strikethrough
    postProcess();                            // fonts, spacing, tables, code backgrounds

    // 4. typeset the formulas (needs the final fonts, hence after postProcess)
    if (!mathItems.isEmpty()) {
        MathOptions opt;
        opt.color = m_theme.editorFg;
        opt.scale = m_s.mathScale / 100.0;
        opt.dpr   = qMax(1.0, devicePixelRatioF());     // 1:1 with the screen: Qt does not smooth scaled images
        MathRenderer::applyMath(document(), mathItems, opt);
    }

    verticalScrollBar()->setValue(scroll);    // don't jump to the top on every keystroke
    updateViewportMargins();
}

// Qt's importer gives sensible structure but plain styling; apply our settings here.
void PreviewView::postProcess()
{
    QTextDocument *doc = document();
    const QFont body = doc->defaultFont();
    const qreal bodyPt = body.pointSizeF() > 0 ? body.pointSizeF() : m_s.previewFontSize;
    const QColor border = m_theme.dark ? m_theme.border.lighter(160) : m_theme.border.darker(110);

    // ---- tables ------------------------------------------------------------------------
    bool hasTables = false;
    std::function<void(QTextFrame *)> walk = [&](QTextFrame *f) {
        for (auto it = f->begin(); !it.atEnd(); ++it) {
            if (QTextFrame *cf = it.currentFrame()) {
                if (auto *table = qobject_cast<QTextTable *>(cf)) {
                    hasTables = true;
                    QTextTableFormat tf = table->format();
                    tf.setBorder(1);
                    tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                    tf.setBorderBrush(border);
                    tf.setCellPadding(6);
                    tf.setCellSpacing(0);
                    tf.setHeaderRowCount(1);
                    table->setFormat(tf);
                } else {
                    walk(cf);
                }
            }
        }
    };
    walk(doc->rootFrame());

    // ---- collect what to restyle (editing while iterating would invalidate iterators) ----
    struct Range { int from, to; };
    QVector<Range> codeRanges;
    struct HeadFrag { int from, to; QTextCharFormat fmt; };
    struct Heading { int level; QVector<HeadFrag> frags; };
    QVector<Heading> headings;
    struct BlockStyle { int pos; enum Kind { Code, Quote, Heading, Item, Para } kind; int level; };
    QVector<BlockStyle> blocks;

    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        const bool isCode = bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage);
        if (hasTables && QTextCursor(b).currentTable()) continue;          // leave table cells alone

        BlockStyle bs{b.position(), BlockStyle::Para, 0};
        if (isCode)                          bs.kind = BlockStyle::Code;
        else if (bf.headingLevel() > 0)    { bs.kind = BlockStyle::Heading; bs.level = bf.headingLevel(); }
        else if (b.textList())               bs.kind = BlockStyle::Item;
        else if (bf.hasProperty(QTextFormat::BlockQuoteLevel)) { bs.kind = BlockStyle::Quote; bs.level = bf.intProperty(QTextFormat::BlockQuoteLevel); }
        blocks.append(bs);

        if (bf.headingLevel() > 0) {
            Heading h; h.level = bf.headingLevel();
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment fr = it.fragment();
                if (fr.isValid()) h.frags.append({fr.position(), fr.position() + fr.length(), fr.charFormat()});
            }
            headings.append(h);
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            if (fr.isValid() && fr.charFormat().fontFixedPitch())
                codeRanges.append({fr.position(), fr.position() + fr.length()});
        }
    }

    QTextCursor edit(doc);
    edit.beginEditBlock();

    // ---- code font (family + size relative to body) -----------------------------------------
    QStringList codeFamilies;
    if (m_s.previewCodeFamily.isEmpty()) codeFamilies = {"Cascadia Code", "Consolas", "Menlo", "DejaVu Sans Mono", "Monospace"};
    else codeFamilies = {m_s.previewCodeFamily};
    QTextCharFormat codeFmt;
    codeFmt.setFontFamilies(codeFamilies);
    codeFmt.setFontFixedPitch(true);
    codeFmt.setFontPointSize(bodyPt * m_s.previewCodePercent / 100.0);
    for (const Range &r : codeRanges) {
        QTextCursor c(doc);
        c.setPosition(r.from);
        c.setPosition(r.to, QTextCursor::KeepAnchor);
        c.mergeCharFormat(codeFmt);
    }

    // ---- heading sizes ---------------------------------------------------------------------
    // Qt's importer sizes headings with the FontSizeAdjustment property, which overrides any explicit
    // point size.  So we rebuild each fragment's format without it instead of merging on top.
    for (const Heading &h : headings) {
        const qreal pt = bodyPt * kHeadingFactor[qBound(1, h.level, 6) - 1] * m_s.previewHeadingScale / 100.0;
        for (const HeadFrag &hf : h.frags) {
            QTextCharFormat f = hf.fmt;
            f.clearProperty(QTextFormat::FontSizeAdjustment);
            f.setFontPointSize(pt);
            f.setFontWeight(QFont::Bold);
            QTextCursor c(doc);
            c.setPosition(hf.from);
            c.setPosition(hf.to, QTextCursor::KeepAnchor);
            c.setCharFormat(f);
        }
    }

    // ---- block spacing, line height, backgrounds ---------------------------------------------
    const qreal gap = m_s.previewParagraphSpacing;
    for (const BlockStyle &bs : blocks) {
        QTextBlockFormat nf;
        nf.setLineHeight(bs.kind == BlockStyle::Code ? 110 : m_s.previewLineHeight, QTextBlockFormat::ProportionalHeight);
        switch (bs.kind) {
        case BlockStyle::Code:    nf.setBackground(m_theme.codeBg); nf.setTopMargin(0); nf.setBottomMargin(0); break;
        case BlockStyle::Quote:   nf.setLeftMargin(16 * qMax(1, bs.level)); nf.setBottomMargin(gap); break;
        case BlockStyle::Heading: nf.setTopMargin(gap * 1.4); nf.setBottomMargin(gap * 0.6); break;
        case BlockStyle::Item:    nf.setTopMargin(0); nf.setBottomMargin(gap * 0.3); break;
        case BlockStyle::Para:    nf.setTopMargin(0); nf.setBottomMargin(gap); break;
        }
        QTextCursor c(doc);
        c.setPosition(bs.pos);
        c.mergeBlockFormat(nf);
    }
    edit.endEditBlock();
}

// -----------------------------------------------------------------------------
//  Headings / navigation
// -----------------------------------------------------------------------------
QVector<QTextBlock> PreviewView::headingBlocks() const
{
    QVector<QTextBlock> v;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (b.blockFormat().headingLevel() > 0) v.append(b);
    return v;
}

int PreviewView::headingCount() const { return headingBlocks().size(); }

void PreviewView::scrollToHeading(int index)
{
    const auto v = headingBlocks();
    if (index < 0 || index >= v.size()) return;
    const QRectF r = document()->documentLayout()->blockBoundingRect(v[index]);
    verticalScrollBar()->setValue(qMax(0, int(r.top()) - 8));
}

void PreviewView::scrollToFraction(double f)
{
    QScrollBar *sb = verticalScrollBar();
    sb->setValue(int(f * sb->maximum()));
}

void PreviewView::mouseDoubleClickEvent(QMouseEvent *e)
{
    QTextBrowser::mouseDoubleClickEvent(e);
    const QTextBlock clicked = cursorForPosition(e->pos()).block();
    if (clicked.blockFormat().headingLevel() <= 0) return;
    const auto v = headingBlocks();
    for (int i = 0; i < v.size(); ++i)
        if (v[i] == clicked) { emit headingActivated(i); return; }
}

void PreviewView::handleLink(const QUrl &url)
{
    if (url.scheme() == "http" || url.scheme() == "https" || url.scheme() == "mailto") {
        QDesktopServices::openUrl(url);
        return;
    }
    // "#some-heading" => scroll to matching heading
    if (url.path().isEmpty() && !url.fragment().isEmpty()) {
        const auto v = headingBlocks();
        for (int i = 0; i < v.size(); ++i)
            if (slugify(v[i].text()) == url.fragment().toLower()) { scrollToHeading(i); return; }
        return;
    }
    // relative link to another file in the project
    QString path = url.isLocalFile() ? url.toLocalFile() : url.path();
    if (QFileInfo(path).isRelative()) path = m_baseDir + "/" + path;
    if (QFileInfo::exists(path)) emit openFileRequested(QFileInfo(path).absoluteFilePath());
}
