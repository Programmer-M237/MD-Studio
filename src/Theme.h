#pragma once
// =============================================================================
//  Theme.h - colour scheme (Dark+ / Light+ inspired) for the whole application.
// =============================================================================
#include <QColor>
#include <QString>

class QApplication;

struct Theme {
    bool dark = true;
    // UI chrome
    QColor window, sidebar, base, text, dimText, button, border, accent, selection;
    // Editor
    QColor editorBg, editorFg, gutterBg, gutterFg, gutterActive, currentLine, ghost;
    // Markdown syntax colours
    QColor heading, bold, italic, code, codeBg, link, quote, listMarker, rule, comment, strike, math;

    static Theme dark_();
    static Theme light_();
    static Theme byName(const QString &name);

    /// Applies Fusion style + palette + a small stylesheet to the application.
    void apply(QApplication &app) const;
    /// CSS used by the preview pane (QTextBrowser rich-text subset).
    QString previewCss() const;
};
