#pragma once
// =============================================================================
//  OutlineParser.h - extracts headings from Markdown text (ATX "# x" and
//  Setext "x\n===" styles), skipping fenced code blocks and YAML front matter.
//  The N-th outline entry corresponds to the N-th heading in the rendered
//  preview, which is how "jump to heading" works in both directions.
// =============================================================================
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

struct OutlineItem {
    int     level;   // 1..6
    QString title;   // cleaned text
    int     line;    // 0-based line in the editor
};

/// Number of leading lines occupied by a YAML front matter block (0 if none).
inline int frontMatterLines(const QStringList &lines)
{
    if (lines.isEmpty() || lines[0].trimmed() != "---") return 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QString t = lines[i].trimmed();
        if (t == "---" || t == "...") return i + 1;
    }
    return 0;
}

/// Markdown with its front matter removed (used before rendering the preview).
inline QString stripFrontMatter(const QString &text)
{
    if (!text.startsWith("---")) return text;
    const QStringList lines = text.split('\n');
    const int n = frontMatterLines(lines);
    return n ? lines.mid(n).join('\n') : text;
}

inline QString cleanHeadingText(QString t)
{
    t = t.trimmed();
    static const QRegularExpression trail(R"(\s+#+\s*$)");
    static const QRegularExpression link(R"(!?\[([^\]]*)\]\([^)]*\))");
    static const QRegularExpression marks(R"([*_`~])");
    t.remove(trail);
    t.replace(link, "\\1");
    t.remove(marks);
    return t.trimmed();
}

/// GitHub-style anchor slug: "Hello, World!" -> "hello-world".
inline QString slugify(const QString &title)
{
    QString s;
    for (const QChar &c : title.toLower()) {
        if (c.isLetterOrNumber() || c == '_') s += c;
        else if (c.isSpace() || c == '-')      s += '-';
    }
    return s;
}

inline QVector<OutlineItem> parseOutline(const QString &text)
{
    static const QRegularExpression atx(R"(^ {0,3}(#{1,6})(?:\s+(.*?))?\s*$)");
    static const QRegularExpression fenceRe(R"(^ {0,3}(`{3,}|~{3,}))");
    static const QRegularExpression setextH1(R"(^ {0,3}=+\s*$)");
    static const QRegularExpression setextH2(R"(^ {0,3}-+\s*$)");
    static const QRegularExpression notParagraph(R"(^\s*([-*+>|]|\d+[.)]\s|#))");

    const QStringList lines = text.split('\n');
    QVector<OutlineItem> out;
    bool inFence = false;
    QString fence;

    for (int i = frontMatterLines(lines); i < lines.size(); ++i) {
        const QString &l = lines[i];

        const auto fm = fenceRe.match(l);
        if (fm.hasMatch()) {
            const QString f = fm.captured(1);
            if (!inFence)                                        { inFence = true; fence = f; }
            else if (f[0] == fence[0] && f.size() >= fence.size()) { inFence = false; }
            continue;
        }
        if (inFence) continue;

        const auto m = atx.match(l);
        if (m.hasMatch()) {
            out.append({int(m.captured(1).size()), cleanHeadingText(m.captured(2)), i});
            continue;
        }

        // Setext heading: a paragraph line followed by ==== or ----
        if (i + 1 < lines.size() && !l.trimmed().isEmpty()
            && !notParagraph.match(l).hasMatch()
            && (i == 0 || lines[i - 1].trimmed().isEmpty())) {
            if (setextH1.match(lines[i + 1]).hasMatch())
                { out.append({1, cleanHeadingText(l), i}); ++i; }
            else if (setextH2.match(lines[i + 1]).hasMatch())
                { out.append({2, cleanHeadingText(l), i}); ++i; }
        }
    }
    return out;
}

/// Qt's Markdown importer (md4c) treats _text_ as UNDERLINE, unlike GitHub/VS Code
/// where it means italic.  Rewrite single-underscore emphasis to *text* before
/// rendering so the preview and exports match what other tools show.
/// Code blocks, code spans, link targets and bare URLs are left untouched.
inline QString normalizeEmphasis(const QString &md)
{
    static const QRegularExpression protectedRe(R"(`[^`\n]*`|\]\([^)\n]*\)|<[^>\n]*>|https?://\S+)");
    static const QRegularExpression fenceRe(R"(^ {0,3}(`{3,}|~{3,}))");
    static const QRegularExpression emphRe(R"((?<![\w_])_(?=[^\s_])([^_\n]*[^\s_])_(?![\w_]))");

    QStringList lines = md.split('\n');
    bool inFence = false;
    QString fence;
    for (QString &line : lines) {
        const auto fm = fenceRe.match(line);
        if (fm.hasMatch()) {
            const QString f = fm.captured(1);
            if (!inFence)                                              { inFence = true; fence = f; }
            else if (f[0] == fence[0] && f.size() >= fence.size())     { inFence = false; }
            continue;
        }
        if (inFence || !line.contains('_')) continue;

        QString out;
        int pos = 0;
        auto it = protectedRe.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            QString seg = line.mid(pos, m.capturedStart() - pos);
            out += seg.replace(emphRe, "*\\1*") + m.captured();
            pos = m.capturedEnd();
        }
        QString tail = line.mid(pos);
        out += tail.replace(emphRe, "*\\1*");
        line = out;
    }
    return lines.join('\n');
}

/// Everything done to Markdown text before it is rendered or exported.
inline QString prepareMarkdown(const QString &md) { return normalizeEmphasis(stripFrontMatter(md)); }
