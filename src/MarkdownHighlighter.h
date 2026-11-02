#pragma once
// =============================================================================
//  MarkdownHighlighter.h - live syntax colouring for the editor.
//  Block state: 0 = normal text, 1 = inside a fenced code block,
//               2 = inside a multi-line $$ ... $$ math block.
// =============================================================================
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QVector>
#include "Theme.h"

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit MarkdownHighlighter(QTextDocument *doc);
    void setTheme(const Theme &theme);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule { QRegularExpression re; QTextCharFormat fmt; int group; };
    QVector<Rule> m_rules;
    QTextCharFormat m_fenceFmt, m_quoteFmt, m_mathFmt;
};
