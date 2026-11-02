#pragma once
// =============================================================================
//  MarkdownEditor.h - the writing surface.
//
//  Features
//    * line-number gutter, current-line highlight, tab/indent handling
//    * live Markdown syntax highlighting (MarkdownHighlighter)
//    * auto-closing pairs, wrap-selection, list / quote auto-continuation
//    * suggestions: a drop-down list (QCompleter) PLUS inline "ghost text".
//      Candidates = snippet triggers ("table", "code", "link"...) + words
//      already used in the document.  Tab / Enter accepts, Esc dismisses,
//      Ctrl+Space forces the list open.
// =============================================================================
#include <QHash>
#include <QPlainTextEdit>
#include <QTimer>
#include "Settings.h"
#include "Theme.h"

class QCompleter;
class QStandardItemModel;
class MarkdownHighlighter;

class MarkdownEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit MarkdownEditor(QWidget *parent = nullptr);

    QString filePath;          // empty => untitled
    bool    crlf = false;      // file used Windows line endings when loaded

    void applySettings(const AppSettings &s, const Theme &t);

    // gutter (called by the LineNumberArea helper widget)
    int  lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *e);

    // formatting helpers (used by menus / shortcuts)
    void wrapSelection(const QString &before, const QString &after, const QString &placeholder = QString());
    void toggleLinePrefix(const QString &prefix);
    void setHeadingLevel(int level);
    void insertSnippet(const QString &text);   // "$0" marks the caret position
    void gotoLine(int zeroBasedLine);
    int  currentLine() const { return textCursor().blockNumber(); }

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    struct Suggestion { QString key, display, insert; bool snippet; };

    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();

    void updateSuggestions(bool force);
    QList<Suggestion> candidatesFor(const QString &prefix, bool force) const;
    void applyInsert(QString text, int prefixLen);
    void rebuildWordIndex();
    void hideSuggestions();
    bool handleEnter();
    bool handlePair(QKeyEvent *e);
    void indentSelection(bool outdent);

    QWidget             *m_lineArea;
    MarkdownHighlighter *m_highlighter;
    QCompleter          *m_completer;
    QStandardItemModel  *m_model;
    QTimer               m_wordTimer;
    QStringList          m_words;
    QList<Suggestion>    m_cands;
    QString              m_ghost;
    int                  m_prefixLen = 0;
    bool                 m_typing = false;   // last edit came from the keyboard
    AppSettings          m_settings;
    Theme                m_theme;
};
