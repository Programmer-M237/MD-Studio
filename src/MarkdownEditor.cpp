#include <algorithm>
#include "MarkdownEditor.h"
#include "MarkdownHighlighter.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QDate>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTextBlock>

namespace {

enum { KeyRole = Qt::UserRole + 1, InsertRole = Qt::UserRole + 2 };

// ----------------------------------------------------------------------------
//  Built-in snippets.  "$0" marks where the caret lands after insertion.
// ----------------------------------------------------------------------------
struct SnippetDef { const char *key, *desc, *body; };
const SnippetDef kSnippets[] = {
    {"h1",        "Heading 1",              "# $0"},
    {"h2",        "Heading 2",              "## $0"},
    {"h3",        "Heading 3",              "### $0"},
    {"h4",        "Heading 4",              "#### $0"},
    {"bold",      "Bold text",              "**$0**"},
    {"italic",    "Italic text",            "_$0_"},
    {"strike",    "Strikethrough",          "~~$0~~"},
    {"code",      "Inline code",            "`$0`"},
    {"codeblock", "Fenced code block",      "```\n$0\n```"},
    {"fence",     "Fenced code block",      "```\n$0\n```"},
    {"python",    "Python code block",      "```python\n$0\n```"},
    {"cpp",       "C++ code block",         "```cpp\n$0\n```"},
    {"javascript","JavaScript code block",  "```javascript\n$0\n```"},
    {"json",      "JSON code block",        "```json\n$0\n```"},
    {"bash",      "Shell code block",       "```bash\n$0\n```"},
    {"link",      "Hyperlink",              "[$0](https://)"},
    {"image",     "Image",                  "![$0](path/to/image.png)"},
    {"table",     "Table (3 columns)",      "| Column 1 | Column 2 | Column 3 |\n| --- | --- | --- |\n| $0 |  |  |\n"},
    {"task",      "Task list item",         "- [ ] $0"},
    {"todo",      "Task list item",         "- [ ] $0"},
    {"list",      "Bulleted list",          "- $0\n- \n- "},
    {"numbered",  "Numbered list",          "1. $0\n2. \n3. "},
    {"quote",     "Block quote",            "> $0"},
    {"rule",      "Horizontal rule",        "---\n$0"},
    {"footnote",  "Footnote definition",    "[^1]: $0"},
    {"details",   "Collapsible section",    "<details>\n<summary>$0</summary>\n\n</details>"},
    {"comment",   "HTML comment",           "<!-- $0 -->"},
    {"frontmatter","YAML front matter",     "---\ntitle: $0\ndate: @DATE@\ntags: []\n---\n"},
    {"date",      "Today's date",           "@DATE@$0"},
    {"math",      "LaTeX display equation", "$$\n$0\n$$"},
    {"imath",     "LaTeX inline formula",   "$$0$"},
    {"equation",  "LaTeX display equation", "$$\n$0\n$$"},
    {"matrix",    "LaTeX matrix",           "$$\n\\begin{pmatrix}\n$0 &  \\\\\n &  \n\\end{pmatrix}\n$$"},
    {"cases",     "LaTeX piecewise function","$$\nf(x) = \\begin{cases}\n$0 & \\text{if } \\\\\n & \\text{otherwise}\n\\end{cases}\n$$"},
    {"align",     "LaTeX aligned equations","$$\n\\begin{aligned}\n$0 &=  \\\\\n &= \n\\end{aligned}\n$$"},
};

// ----------------------------------------------------------------------------
//  LaTeX command completion (offered when the word before the caret starts with "\").
//  Structural commands carry a template; "$0" marks the caret position.
// ----------------------------------------------------------------------------
struct LatexCmd { const char *name, *insert, *detail; };
const LatexCmd kLatexStructural[] = {
    {"frac",      "\\frac{$0}{}",            "fraction"},
    {"dfrac",     "\\dfrac{$0}{}",           "fraction (display size)"},
    {"tfrac",     "\\tfrac{$0}{}",           "fraction (text size)"},
    {"binom",     "\\binom{$0}{}",           "binomial coefficient"},
    {"sqrt",      "\\sqrt{$0}",              "square root"},
    {"sqrt[n]",   "\\sqrt[$0]{}",            "n-th root"},
    {"sum",       "\\sum_{$0}^{}",           "summation"},
    {"prod",      "\\prod_{$0}^{}",          "product"},
    {"int",       "\\int_{$0}^{}",           "integral"},
    {"iint",      "\\iint_{$0}",             "double integral"},
    {"oint",      "\\oint_{$0}",             "contour integral"},
    {"bigcup",    "\\bigcup_{$0}^{}",        "big union"},
    {"bigcap",    "\\bigcap_{$0}^{}",        "big intersection"},
    {"lim",       "\\lim_{$0}",              "limit"},
    {"max",       "\\max_{$0}",              "maximum"},
    {"min",       "\\min_{$0}",              "minimum"},
    {"sup",       "\\sup_{$0}",              "supremum"},
    {"inf",       "\\inf_{$0}",              "infimum"},
    {"hat",       "\\hat{$0}",               "hat accent"},
    {"widehat",   "\\widehat{$0}",           "wide hat"},
    {"bar",       "\\bar{$0}",               "bar accent"},
    {"vec",       "\\vec{$0}",               "vector arrow"},
    {"dot",       "\\dot{$0}",               "dot accent"},
    {"ddot",      "\\ddot{$0}",              "double dot"},
    {"tilde",     "\\tilde{$0}",             "tilde accent"},
    {"widetilde", "\\widetilde{$0}",         "wide tilde"},
    {"overline",  "\\overline{$0}",          "line above"},
    {"underline", "\\underline{$0}",         "line below"},
    {"overbrace", "\\overbrace{$0}^{}",      "brace above"},
    {"underbrace","\\underbrace{$0}_{}",     "brace below"},
    {"text",      "\\text{$0}",              "text in a formula"},
    {"mathrm",    "\\mathrm{$0}",            "upright"},
    {"mathbf",    "\\mathbf{$0}",            "bold"},
    {"mathit",    "\\mathit{$0}",            "italic"},
    {"mathbb",    "\\mathbb{$0}",            "blackboard bold (R, N, Z...)"},
    {"mathcal",   "\\mathcal{$0}",           "calligraphic"},
    {"mathfrak",  "\\mathfrak{$0}",          "fraktur"},
    {"boldsymbol","\\boldsymbol{$0}",        "bold symbol"},
    {"operatorname","\\operatorname{$0}",    "custom operator name"},
    {"boxed",     "\\boxed{$0}",             "framed formula"},
    {"xrightarrow","\\xrightarrow{$0}",      "arrow with label"},
    {"overset",   "\\overset{$0}{}",         "symbol above"},
    {"underset",  "\\underset{$0}{}",        "symbol below"},
    {"substack",  "\\substack{$0 \\\\ }",    "stacked limits"},
    {"left(",     "\\left( $0 \\right)",     "auto-sized parentheses"},
    {"left[",     "\\left[ $0 \\right]",     "auto-sized brackets"},
    {"left\\{",   "\\left\\{ $0 \\right\\}", "auto-sized braces"},
    {"left|",     "\\left| $0 \\right|",     "auto-sized bars"},
    {"left\\langle","\\left\\langle $0 \\right\\rangle", "auto-sized angle brackets"},
    {"big",       "\\big",                   "larger delimiter"},
    {"Big",       "\\Big",                   "even larger delimiter"},
    {"quad",      "\\quad",                  "wide space"},
    {"qquad",     "\\qquad",                 "double wide space"},
    {"begin{pmatrix}", "\\begin{pmatrix}\n$0 &  \\\\\n &  \n\\end{pmatrix}", "matrix with ( )"},
    {"begin{bmatrix}", "\\begin{bmatrix}\n$0 &  \\\\\n &  \n\\end{bmatrix}", "matrix with [ ]"},
    {"begin{vmatrix}", "\\begin{vmatrix}\n$0 &  \\\\\n &  \n\\end{vmatrix}", "determinant"},
    {"begin{matrix}",  "\\begin{matrix}\n$0 &  \\\\\n &  \n\\end{matrix}",   "matrix without delimiters"},
    {"begin{cases}",   "\\begin{cases}\n$0 & \\text{if } \\\\\n & \\text{otherwise}\n\\end{cases}", "piecewise function"},
    {"begin{aligned}", "\\begin{aligned}\n$0 &=  \\\\\n &= \n\\end{aligned}", "aligned equations"},
    {"begin{array}",   "\\begin{array}{cc}\n$0 &  \\\\\n &  \n\\end{array}", "array"},
};
// Plain symbols: inserted as "\name"; the code point is shown as a preview.
struct LatexSym { const char *name; uint cp; };
const LatexSym kLatexSymbols[] = {
    {"alpha",0x3B1},{"beta",0x3B2},{"gamma",0x3B3},{"delta",0x3B4},{"epsilon",0x3F5},{"varepsilon",0x3B5},{"zeta",0x3B6},{"eta",0x3B7},
    {"theta",0x3B8},{"vartheta",0x3D1},{"iota",0x3B9},{"kappa",0x3BA},{"lambda",0x3BB},{"mu",0x3BC},{"nu",0x3BD},{"xi",0x3BE},
    {"pi",0x3C0},{"varpi",0x3D6},{"rho",0x3C1},{"varrho",0x3F1},{"sigma",0x3C3},{"varsigma",0x3C2},{"tau",0x3C4},{"upsilon",0x3C5},
    {"phi",0x3D5},{"varphi",0x3C6},{"chi",0x3C7},{"psi",0x3C8},{"omega",0x3C9},
    {"Gamma",0x393},{"Delta",0x394},{"Theta",0x398},{"Lambda",0x39B},{"Xi",0x39E},{"Pi",0x3A0},{"Sigma",0x3A3},{"Upsilon",0x3A5},
    {"Phi",0x3A6},{"Psi",0x3A8},{"Omega",0x3A9},
    {"pm",0xB1},{"mp",0x2213},{"times",0xD7},{"div",0xF7},{"cdot",0x22C5},{"ast",0x2217},{"star",0x22C6},{"circ",0x2218},{"oplus",0x2295},
    {"otimes",0x2297},{"cap",0x2229},{"cup",0x222A},{"vee",0x2228},{"wedge",0x2227},{"setminus",0x2216},
    {"leq",0x2264},{"geq",0x2265},{"neq",0x2260},{"approx",0x2248},{"equiv",0x2261},{"sim",0x223C},{"simeq",0x2243},{"cong",0x2245},
    {"propto",0x221D},{"in",0x2208},{"notin",0x2209},{"ni",0x220B},{"subset",0x2282},{"supset",0x2283},{"subseteq",0x2286},
    {"supseteq",0x2287},{"ll",0x226A},{"gg",0x226B},{"perp",0x22A5},{"parallel",0x2225},{"mid",0x2223},
    {"to",0x2192},{"rightarrow",0x2192},{"leftarrow",0x2190},{"leftrightarrow",0x2194},{"Rightarrow",0x21D2},{"Leftarrow",0x21D0},
    {"Leftrightarrow",0x21D4},{"mapsto",0x21A6},{"implies",0x27F9},{"iff",0x27FA},{"uparrow",0x2191},{"downarrow",0x2193},
    {"infty",0x221E},{"partial",0x2202},{"nabla",0x2207},{"forall",0x2200},{"exists",0x2203},{"neg",0xAC},{"emptyset",0x2205},
    {"angle",0x2220},{"hbar",0x210F},{"ell",0x2113},{"Re",0x211C},{"Im",0x2111},{"aleph",0x2135},{"ldots",0x2026},{"cdots",0x22EF},
    {"vdots",0x22EE},{"ddots",0x22F1},{"prime",0x2032},{"langle",0x27E8},{"rangle",0x27E9},{"lfloor",0x230A},{"rfloor",0x230B},
    {"lceil",0x2308},{"rceil",0x2309},
    {"sin",0},{"cos",0},{"tan",0},{"cot",0},{"sec",0},{"csc",0},{"arcsin",0},{"arccos",0},{"arctan",0},{"sinh",0},{"cosh",0},
    {"tanh",0},{"log",0},{"ln",0},{"exp",0},{"det",0},{"dim",0},{"ker",0},{"gcd",0},{"arg",0},
};

QString leadingWhitespace(const QString &s)
{
    int i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.left(i);
}

} // namespace

// ----------------------------------------------------------------------------
//  Gutter widget: forwards painting to the editor.
// ----------------------------------------------------------------------------
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(MarkdownEditor *e) : QWidget(e), m_editor(e) {}
    QSize sizeHint() const override { return {m_editor->lineNumberAreaWidth(), 0}; }
protected:
    void paintEvent(QPaintEvent *ev) override { m_editor->lineNumberAreaPaintEvent(ev); }
private:
    MarkdownEditor *m_editor;
};

// ----------------------------------------------------------------------------
MarkdownEditor::MarkdownEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    m_lineArea    = new LineNumberArea(this);
    m_highlighter = new MarkdownHighlighter(document());
    m_theme       = Theme::dark_();

    setFrameShape(QFrame::NoFrame);
    setCursorWidth(2);

    // --- suggestion popup -------------------------------------------------
    m_model     = new QStandardItemModel(this);
    m_completer = new QCompleter(m_model, this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionRole(KeyRole);
    m_completer->setMaxVisibleItems(10);
    connect(m_completer, QOverload<const QModelIndex &>::of(&QCompleter::activated), this,
            [this](const QModelIndex &idx) {
                applyInsert(idx.data(InsertRole).toString(), m_prefixLen);
            });

    // Word index for "words already in this document" suggestions.
    m_wordTimer.setSingleShot(true);
    m_wordTimer.setInterval(400);
    connect(&m_wordTimer, &QTimer::timeout, this, &MarkdownEditor::rebuildWordIndex);
    connect(this, &QPlainTextEdit::textChanged, &m_wordTimer, qOverload<>(&QTimer::start));

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateLineNumberAreaWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this, &MarkdownEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &MarkdownEditor::highlightCurrentLine);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!m_ghost.isEmpty()) { m_ghost.clear(); viewport()->update(); }
    });

    applySettings(m_settings, m_theme);
}

// ----------------------------------------------------------------------------
//  Settings / theme
// ----------------------------------------------------------------------------
void MarkdownEditor::applySettings(const AppSettings &s, const Theme &t)
{
    m_settings = s;
    m_theme    = t;

    QFont f;
    if (s.fontFamily.isEmpty())
        f.setFamilies({"Cascadia Code", "Consolas", "Menlo", "DejaVu Sans Mono", "Monospace"});
    else
        f.setFamily(s.fontFamily);
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    f.setPointSize(s.fontSize);
    setFont(f);

    setTabStopDistance(s.tabSize * fontMetrics().horizontalAdvance(' '));
    setLineWrapMode(s.wordWrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);

    QPalette p = palette();
    p.setColor(QPalette::Base, t.editorBg);
    p.setColor(QPalette::Text, t.editorFg);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.dark ? Qt::white : Qt::black);
    setPalette(p);

    m_highlighter->setTheme(t);
    m_lineArea->setVisible(s.lineNumbers);
    updateLineNumberAreaWidth();
    highlightCurrentLine();
    if (!s.suggestions && !s.inlineSuggestions) hideSuggestions();
    viewport()->update();
}

// ----------------------------------------------------------------------------
//  Gutter
// ----------------------------------------------------------------------------
int MarkdownEditor::lineNumberAreaWidth() const
{
    if (!m_settings.lineNumbers) return 0;
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10) ++digits;
    return 20 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * qMax(digits, 2);
}

void MarkdownEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void MarkdownEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy) m_lineArea->scroll(0, dy);
    else    m_lineArea->update(0, rect.y(), m_lineArea->width(), rect.height());
    if (rect.contains(viewport()->rect())) updateLineNumberAreaWidth();
}

void MarkdownEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    const QRect cr = contentsRect();
    m_lineArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void MarkdownEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_lineArea);
    painter.fillRect(event->rect(), m_theme.gutterBg);
    painter.setFont(font());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top    = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(blockNumber == current ? m_theme.gutterActive : m_theme.gutterFg);
            painter.drawText(0, top, m_lineArea->width() - 10, fontMetrics().height(),
                             Qt::AlignRight, QString::number(blockNumber + 1));
        }
        block  = block.next();
        top    = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void MarkdownEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> sels;
    if (m_settings.highlightCurrentLine && !isReadOnly()) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(m_theme.currentLine);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        sels.append(sel);
    }
    setExtraSelections(sels);
    m_lineArea->update();
}

// ----------------------------------------------------------------------------
//  Ghost text painting
// ----------------------------------------------------------------------------
void MarkdownEditor::paintEvent(QPaintEvent *e)
{
    QPlainTextEdit::paintEvent(e);
    if (m_ghost.isEmpty() || !hasFocus()) return;

    QPainter p(viewport());
    p.setPen(m_theme.ghost);
    p.setFont(font());
    const QRect cr = cursorRect();
    p.drawText(QPoint(cr.right() + 1, cr.top() + fontMetrics().ascent()), m_ghost);
}

void MarkdownEditor::focusOutEvent(QFocusEvent *e)
{
    QPlainTextEdit::focusOutEvent(e);
    // Keep the popup alive when it is the thing that took focus.
    if (!m_completer->popup()->isVisible()) { m_ghost.clear(); viewport()->update(); }
}

// ----------------------------------------------------------------------------
//  Suggestions
// ----------------------------------------------------------------------------
void MarkdownEditor::rebuildWordIndex()
{
    static const QRegularExpression wordRe(R"([A-Za-z][A-Za-z0-9_]{3,})");
    const QString text = toPlainText();
    const int caret = textCursor().position();
    const bool skipCaretWord = m_typing;      // loading a file must not hide the word at position 0
    m_typing = false;

    QHash<QString, int> seen;
    auto it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        // Ignore the word currently being typed, otherwise "tabl" suggests "tabl".
        if (skipCaretWord && m.capturedStart() < caret && caret <= m.capturedEnd()) continue;
        seen[m.captured()]++;
    }
    m_words = seen.keys();
    std::sort(m_words.begin(), m_words.end(), [](const QString &a, const QString &b) {
        return a.size() != b.size() ? a.size() < b.size() : a.compare(b, Qt::CaseInsensitive) < 0;
    });
}

QList<MarkdownEditor::Suggestion> MarkdownEditor::candidatesFor(const QString &prefix, bool force) const
{
    QList<Suggestion> out;

    // "\fr" -> LaTeX commands only
    if (prefix.startsWith('\\')) {
        for (const LatexCmd &c : kLatexStructural) {
            const QString key = "\\" + QString::fromLatin1(c.name);
            if (key.startsWith(prefix, Qt::CaseSensitive))
                out.append({key, QString("%1   \u2014 %2").arg(key, QString::fromLatin1(c.detail)), QString::fromLatin1(c.insert), true});
        }
        for (const LatexSym &c : kLatexSymbols) {
            const QString key = "\\" + QString::fromLatin1(c.name);
            if (!key.startsWith(prefix, Qt::CaseSensitive)) continue;
            const char32_t c32 = c.cp;
            const QString shown = c.cp ? QString::fromUcs4(&c32, 1) : QString::fromLatin1(c.name);
            out.append({key, QString("%1   \u2014 %2").arg(key, shown), key, true});
        }
        std::stable_sort(out.begin(), out.end(), [](const Suggestion &a, const Suggestion &b) {
            return a.key.size() != b.key.size() ? a.key.size() < b.key.size() : a.key < b.key; });
        if (out.size() > 30) out.resize(30);
        return out;
    }
    for (const SnippetDef &s : kSnippets) {
        const QString key = QString::fromLatin1(s.key);
        if (!key.startsWith(prefix, Qt::CaseInsensitive)) continue;
        QString body = QString::fromLatin1(s.body);
        body.replace("@DATE@", QDate::currentDate().toString(Qt::ISODate));
        out.append({key, QString("%1   \u2014 %2").arg(key, QString::fromLatin1(s.desc)), body, true});
    }
    if (!prefix.isEmpty() || force) {
        for (const QString &w : m_words) {
            if (out.size() >= 14) break;
            if (w.size() > prefix.size() && w.startsWith(prefix, Qt::CaseInsensitive))
                out.append({w, w, w, false});
        }
    }
    return out;
}

void MarkdownEditor::hideSuggestions()
{
    m_completer->popup()->hide();
    if (!m_ghost.isEmpty()) { m_ghost.clear(); viewport()->update(); }
}

void MarkdownEditor::updateSuggestions(bool force)
{
    m_ghost.clear();
    const bool wantPopup = m_settings.suggestions || force;
    const bool wantGhost = m_settings.inlineSuggestions && !force;

    QTextCursor c = textCursor();
    if ((!wantPopup && !wantGhost) || c.hasSelection()) { hideSuggestions(); return; }

    const QString line   = c.block().text();
    const int     col    = c.positionInBlock();
    const QString before = line.left(col);

    static const QRegularExpression prefixRe(R"(\\?[A-Za-z][A-Za-z0-9_]*$)");   // optional leading "\" => LaTeX command
    const auto pm = prefixRe.match(before);
    const QString prefix = pm.hasMatch() ? pm.captured() : QString();

    // Do not interrupt when the caret sits inside a word.
    if (!force && col < line.size() && line[col].isLetterOrNumber()) { hideSuggestions(); return; }
    if (!force && prefix.size() < m_settings.suggestMinChars)         { hideSuggestions(); return; }

    m_cands = candidatesFor(prefix, force);
    if (m_cands.isEmpty()) { hideSuggestions(); return; }
    m_prefixLen = prefix.size();

    // Ghost text = remainder of the best candidate.
    if (wantGhost && !prefix.isEmpty()) {
        const Suggestion &best = m_cands.first();
        m_ghost = best.key.mid(prefix.size());
        if (best.snippet) m_ghost += "   \u21E5 " + best.display.section(QString::fromUtf8("\u2014 "), 1);
        viewport()->update();
    }

    if (!wantPopup) { m_completer->popup()->hide(); return; }

    m_model->clear();
    for (const Suggestion &s : std::as_const(m_cands)) {
        auto *item = new QStandardItem(s.display);
        item->setData(s.key,    KeyRole);
        item->setData(s.insert, InsertRole);
        m_model->appendRow(item);
    }
    m_completer->setCompletionPrefix(prefix);
    if (m_completer->completionCount() == 0) { m_completer->popup()->hide(); return; }

    QRect cr = cursorRect();
    cr.translate(viewport()->pos());
    cr.setWidth(m_completer->popup()->sizeHintForColumn(0)
                + m_completer->popup()->verticalScrollBar()->sizeHint().width() + 24);
    m_completer->complete(cr);
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
}

void MarkdownEditor::applyInsert(QString text, int prefixLen)
{
    QTextCursor c = textCursor();
    c.beginEditBlock();
    if (c.hasSelection()) c.removeSelectedText();
    else if (prefixLen > 0) {
        c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, prefixLen);
        c.removeSelectedText();
    }
    // Keep multi-line snippets aligned with the current line's indentation.
    const QString indent = leadingWhitespace(c.block().text());
    if (!indent.isEmpty()) text.replace("\n", "\n" + indent);
    const int caret = text.indexOf("$0");
    if (caret >= 0) text.remove(caret, 2);

    const int start = c.position();
    c.insertText(text);
    if (caret >= 0) c.setPosition(start + caret);
    c.endEditBlock();
    setTextCursor(c);
    m_ghost.clear();
    viewport()->update();
}

void MarkdownEditor::insertSnippet(const QString &text) { applyInsert(text, 0); }

// ----------------------------------------------------------------------------
//  Key handling
// ----------------------------------------------------------------------------
void MarkdownEditor::keyPressEvent(QKeyEvent *e)
{
    const int key = e->key();
    const Qt::KeyboardModifiers mods = e->modifiers();

    // The completer's popup handles these itself; just don't act on them here.
    if (m_completer->popup()->isVisible()) {
        switch (key) {
        case Qt::Key_Enter: case Qt::Key_Return: case Qt::Key_Escape:
        case Qt::Key_Tab:   case Qt::Key_Backtab:
            e->ignore();
            return;
        default: break;
        }
    }

    if ((mods & Qt::ControlModifier) && key == Qt::Key_Space) { updateSuggestions(true); return; }

    // Tab accepts the inline ghost suggestion.
    if (key == Qt::Key_Tab && mods == Qt::NoModifier && !m_ghost.isEmpty() && !m_cands.isEmpty()) {
        applyInsert(m_cands.first().insert, m_prefixLen);
        return;
    }
    if (key == Qt::Key_Escape && !m_ghost.isEmpty()) { hideSuggestions(); return; }

    if ((key == Qt::Key_Return || key == Qt::Key_Enter)
        && !(mods & (Qt::ControlModifier | Qt::ShiftModifier)) && handleEnter())
        return;

    if (key == Qt::Key_Tab && !(mods & Qt::ControlModifier)) {
        if (textCursor().hasSelection()) { indentSelection(false); return; }
        // Insert spaces up to the next tab stop.
        QTextCursor c = textCursor();
        const int n = m_settings.tabSize - (c.positionInBlock() % m_settings.tabSize);
        c.insertText(QString(n, ' '));
        return;
    }
    if (key == Qt::Key_Backtab) { indentSelection(true); return; }

    if (m_settings.autoClosePairs && handlePair(e)) return;

    QPlainTextEdit::keyPressEvent(e);

    const bool typed = !e->text().isEmpty() && e->text().at(0).isPrint()
                       && !(mods & (Qt::ControlModifier | Qt::AltModifier));
    if (typed || key == Qt::Key_Backspace) {
        m_typing = true;
        updateSuggestions(false);
    } else {
        switch (key) {
        case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Home: case Qt::Key_End:
        case Qt::Key_PageUp: case Qt::Key_PageDown: case Qt::Key_Delete:
            hideSuggestions();
            break;
        default: break;
        }
    }
}

// Enter inside a list/quote continues it; Enter on an empty item ends it.
bool MarkdownEditor::handleEnter()
{
    QTextCursor c = textCursor();
    if (c.hasSelection()) return false;

    static const QRegularExpression listRe(R"(^(\s*)([-*+]|\d+[.)])(\s+)(\[[ xX]\]\s+)?(.*)$)");
    static const QRegularExpression quoteRe(R"(^(\s*(?:>\s?)+)(.*)$)");
    const QString line = c.block().text();
    const int col = c.positionInBlock();

    // Don't continue lists inside fenced code.
    if (c.block().userState() == 1) {
        c.insertText("\n" + leadingWhitespace(line));
        setTextCursor(c);
        return true;
    }

    if (const auto m = listRe.match(line); m.hasMatch()) {
        const int markerEnd = m.capturedEnd(4) >= 0 ? m.capturedEnd(4) : m.capturedEnd(3);
        if (col < markerEnd) return false;                         // caret inside the marker
        if (m.captured(5).trimmed().isEmpty()) {                   // empty item => end list
            c.beginEditBlock();
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            c.endEditBlock();
            setTextCursor(c);
            return true;
        }
        QString marker = m.captured(2);
        if (marker[0].isDigit()) {                                 // 3. -> 4.
            const int n = marker.left(marker.size() - 1).toInt() + 1;
            marker = QString::number(n) + marker.right(1);
        }
        QString next = m.captured(1) + marker + m.captured(3);
        if (!m.captured(4).isEmpty()) next += "[ ] ";
        c.insertText("\n" + next);
        setTextCursor(c);
        ensureCursorVisible();
        return true;
    }
    if (const auto m = quoteRe.match(line); m.hasMatch() && col >= m.capturedEnd(1)) {
        if (m.captured(2).trimmed().isEmpty()) {
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        } else {
            c.insertText("\n" + m.captured(1));
        }
        setTextCursor(c);
        return true;
    }

    // Plain auto-indent.
    const QString indent = leadingWhitespace(line).left(col);
    if (!indent.isEmpty()) { c.insertText("\n" + indent); setTextCursor(c); ensureCursorVisible(); return true; }
    return false;
}

// Auto-closing brackets/quotes, over-typing of closers, wrap-selection.
bool MarkdownEditor::handlePair(QKeyEvent *e)
{
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) return false;
    const QString t = e->text();
    if (t.size() != 1) return false;
    const QChar ch = t[0];

    static const QString openers = "([{\"`";
    static const QString closers = ")]}\"`";
    static const QString wrapOnly = "*_~";

    QTextCursor c = textCursor();

    if (c.hasSelection() && (openers.contains(ch) || wrapOnly.contains(ch))) {
        const int idx = openers.indexOf(ch);
        const QString closeStr = idx >= 0 ? QString(closers[idx]) : QString(ch);
        wrapSelection(QString(ch), closeStr);
        return true;
    }

    const QString line = c.block().text();
    const int col = c.positionInBlock();
    const QChar next = col < line.size() ? line[col] : QChar();
    const QChar prev = col > 0 ? line[col - 1] : QChar();

    // Over-type a closer that we (probably) inserted earlier.
    if (closers.contains(ch) && next == ch) {
        c.movePosition(QTextCursor::Right);
        setTextCursor(c);
        return true;
    }
    if (openers.contains(ch)) {
        const bool quoteLike = (ch == '"' || ch == '`');
        if (quoteLike && (prev.isLetterOrNumber() || next.isLetterOrNumber())) return false; // apostrophe-ish
        if (ch == '`' && col >= 2 && line.mid(col - 2, 2) == "``") return false;             // typing ``` fence
        if (!next.isNull() && !next.isSpace() && !closers.contains(next)) return false;       // only before space/closer/EOL
        const QChar close = closers[openers.indexOf(ch)];
        c.insertText(QString(ch) + close);
        c.movePosition(QTextCursor::Left);
        setTextCursor(c);
        return true;
    }
    return false;
}

void MarkdownEditor::indentSelection(bool outdent)
{
    QTextCursor c = textCursor();
    const int start = c.selectionStart(), end = c.selectionEnd();
    QTextBlock first = document()->findBlock(start);
    QTextBlock last  = document()->findBlock(qMax(start, end - (c.hasSelection() && end > start &&
                                              document()->findBlock(end).position() == end ? 1 : 0)));
    const QString pad(m_settings.tabSize, ' ');

    c.beginEditBlock();
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last.blockNumber(); b = b.next()) {
        QTextCursor lc(b);
        if (!outdent) { lc.insertText(pad); continue; }
        const QString t = b.text();
        int n = 0;
        if (t.startsWith('\t')) n = 1;
        else while (n < m_settings.tabSize && n < t.size() && t[n] == ' ') ++n;
        if (n) { lc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, n); lc.removeSelectedText(); }
    }
    c.endEditBlock();
}

// ----------------------------------------------------------------------------
//  Formatting helpers
// ----------------------------------------------------------------------------
void MarkdownEditor::wrapSelection(const QString &before, const QString &after, const QString &placeholder)
{
    QTextCursor c = textCursor();

    // Selection sits INSIDE existing markers (e.g. after a previous wrap) => remove them.
    if (c.hasSelection() && !before.isEmpty()) {
        const int s = c.selectionStart(), e = c.selectionEnd();
        const int docEnd = document()->characterCount() - 1;
        auto textAt = [&](int from, int len) {
            QTextCursor t(document());
            t.setPosition(from);
            t.setPosition(from + len, QTextCursor::KeepAnchor);
            return t.selectedText();
        };
        if (s - before.size() >= 0 && e + after.size() <= docEnd
            && textAt(s - before.size(), before.size()) == before && textAt(e, after.size()) == after) {
            c.beginEditBlock();
            QTextCursor t(document());
            t.setPosition(e); t.setPosition(e + after.size(), QTextCursor::KeepAnchor); t.removeSelectedText();
            t.setPosition(s - before.size()); t.setPosition(s, QTextCursor::KeepAnchor); t.removeSelectedText();
            c.endEditBlock();
            c.setPosition(s - before.size());
            c.setPosition(e - before.size(), QTextCursor::KeepAnchor);
            setTextCursor(c);
            return;
        }
    }

    c.beginEditBlock();
    if (c.hasSelection()) {
        QString sel = c.selectedText();
        sel.replace(QChar::ParagraphSeparator, '\n');
        const int start = c.selectionStart();
        const bool wrapped = sel.size() >= before.size() + after.size()
                             && sel.startsWith(before) && sel.endsWith(after);
        const QString inner = wrapped ? sel.mid(before.size(), sel.size() - before.size() - after.size()) : sel;
        c.insertText(wrapped ? inner : before + sel + after);
        const int innerStart = wrapped ? start : start + before.size();
        c.setPosition(innerStart);
        c.setPosition(innerStart + inner.size(), QTextCursor::KeepAnchor);
    } else {
        const int start = c.position();
        c.insertText(before + placeholder + after);
        c.setPosition(start + before.size());
        c.setPosition(start + before.size() + placeholder.size(), QTextCursor::KeepAnchor);
    }
    c.endEditBlock();
    setTextCursor(c);
}

void MarkdownEditor::toggleLinePrefix(const QString &prefix)
{
    QTextCursor c = textCursor();
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last  = document()->findBlock(c.selectionEnd());

    bool allHave = true;
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last.blockNumber(); b = b.next())
        if (!b.text().startsWith(prefix)) { allHave = false; break; }

    c.beginEditBlock();
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last.blockNumber(); b = b.next()) {
        QTextCursor lc(b);
        if (allHave) { lc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, prefix.size()); lc.removeSelectedText(); }
        else         { lc.insertText(prefix); }
    }
    c.endEditBlock();
}

void MarkdownEditor::setHeadingLevel(int level)
{
    static const QRegularExpression re(R"(^(#{1,6})\s+)");
    QTextCursor c = textCursor();
    const QString t = c.block().text();
    const auto m = re.match(t);
    const int cur = m.hasMatch() ? m.captured(1).size() : 0;
    const QString body = m.hasMatch() ? t.mid(m.capturedLength()) : t;

    c.beginEditBlock();
    c.movePosition(QTextCursor::StartOfBlock);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    c.insertText((level > 0 && level != cur ? QString(level, '#') + " " : QString()) + body);
    c.endEditBlock();
    setTextCursor(c);
}

void MarkdownEditor::gotoLine(int line)
{
    const QTextBlock b = document()->findBlockByNumber(qBound(0, line, blockCount() - 1));
    QTextCursor c(b);
    setTextCursor(c);
    centerCursor();
    setFocus();
}
