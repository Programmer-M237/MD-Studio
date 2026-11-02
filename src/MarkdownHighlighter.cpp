#include "MarkdownHighlighter.h"

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc)
{
    setTheme(Theme::dark_());
}

void MarkdownHighlighter::setTheme(const Theme &t)
{
    m_rules.clear();
    auto fmt = [](const QColor &c, bool bold = false, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)   f.setFontWeight(QFont::Bold);
        if (italic) f.setFontItalic(true);
        return f;
    };
    using QRE = QRegularExpression;

    // The order matters: later rules paint over earlier ones.
    QTextCharFormat strike = fmt(t.strike);  strike.setFontStrikeOut(true);
    QTextCharFormat code   = fmt(t.code);    code.setBackground(t.codeBg);

    m_rules.append({QRE(R"(^\s*\|.*\|\s*$)"),                 fmt(t.editorFg),               0});
    m_rules.append({QRE(R"(\|)"),                             fmt(t.listMarker, true),       0});
    m_rules.append({QRE(R"(^\s*(?:[-*+]|\d+[.)])\s)"),        fmt(t.listMarker, true),       0});
    m_rules.append({QRE(R"(^\s*(?:[-*+]|\d+[.)])\s+(\[[ xX]\]))"), fmt(t.listMarker, true),  1});
    m_rules.append({QRE(R"(^ {0,3}([-*_])(?:\s*\1){2,}\s*$)"), fmt(t.rule, true),            0});
    m_rules.append({QRE(R"(^ {0,3}(?:=+|-+)\s*$)"),           fmt(t.heading, true),          0});
    m_rules.append({QRE(R"(^ {0,3}#{1,6}(?:\s.*)?$)"),        fmt(t.heading, true),          0});
    m_rules.append({QRE(R"((?<![*\w])\*[^*\s](?:[^*]*[^*\s])?\*(?!\*))"), fmt(t.italic, false, true), 0});
    m_rules.append({QRE(R"((?<![_\w])_[^_\s](?:[^_]*[^_\s])?_(?!\w))"),   fmt(t.italic, false, true), 0});
    m_rules.append({QRE(R"(\*\*[^*\n]+\*\*|__[^_\n]+__)"),    fmt(t.bold, true),             0});
    m_rules.append({QRE(R"(~~[^~\n]+~~)"),                    strike,                        0});
    m_rules.append({QRE(R"(!?\[[^\]\n]*\]\([^)\n]*\))"),      fmt(t.link),                   0});
    m_rules.append({QRE(R"(<https?://[^>\s]+>|\bhttps?://[^\s)>\]]+)"), fmt(t.link),         0});
    m_rules.append({QRE(R"((?<!\\)\$\$[^$\n]+\$\$|(?<![\\$])\$(?=[^\s$])[^$\n]*[^\s$\\]\$(?!\d))"), fmt(t.math), 0});   // LaTeX math
    m_rules.append({QRE(R"(`[^`\n]+`)"),                      code,                          0});
    m_rules.append({QRE(R"(<!--.*?-->)"),                     fmt(t.comment, false, true),   0});
    m_rules.append({QRE(R"(^\s*>.*$)"),                       fmt(t.quote, false, true),     0});

    m_fenceFmt = code;
    m_mathFmt  = fmt(t.math);
    rehighlight();
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    static const QRegularExpression fence(R"(^\s{0,3}(```|~~~))");
    // multi-line display math:  $$ ... $$   (block state 2)
    if (previousBlockState() == 2) {
        setFormat(0, text.length(), m_mathFmt);
        setCurrentBlockState(text.contains("$$") ? 0 : 2);
        return;
    }
    {
        const QString t = text.trimmed();
        if (previousBlockState() != 1 && t.startsWith("$$") && t.indexOf("$$", 2) < 0) {
            setFormat(0, text.length(), m_mathFmt);
            setCurrentBlockState(2);
            return;
        }
    }
    const bool inCode = previousBlockState() == 1;
    const bool isFence = fence.match(text).hasMatch();

    // Fenced code: colour whole lines, toggle state on fence lines.
    if (inCode) {
        setFormat(0, text.length(), m_fenceFmt);
        setCurrentBlockState(isFence ? 0 : 1);
        return;
    }
    if (isFence) {
        setFormat(0, text.length(), m_fenceFmt);
        setCurrentBlockState(1);
        return;
    }
    setCurrentBlockState(0);

    for (const Rule &r : std::as_const(m_rules)) {
        auto it = r.re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const int start = m.capturedStart(r.group);
            const int len   = m.capturedLength(r.group);
            if (start >= 0 && len > 0) setFormat(start, len, r.fmt);
        }
    }
}
