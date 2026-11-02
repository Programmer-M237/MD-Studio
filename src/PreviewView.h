#pragma once
// =============================================================================
//  PreviewView.h - rendered Markdown pane (QTextBrowser + Qt's md4c importer).
//
//  * LaTeX math ($..$, $$..$$, \(..\), \[..\], ```math) is typeset by
//    MathRenderer and inserted as baseline-aligned images
//  * every visual aspect (fonts, sizes, spacing, margins, max width, colours)
//    comes from AppSettings - see applyStyle()
//  * heading lookup for outline jumps, anchor links, double-click a heading to
//    jump to the source, scroll position preserved across refreshes
// =============================================================================
#include <QTextBrowser>
#include "Settings.h"
#include "Theme.h"

class PreviewView : public QTextBrowser {
    Q_OBJECT
public:
    explicit PreviewView(QWidget *parent = nullptr);

    /// Applies the appearance settings (fonts, spacing, margins, colours...).
    void applyStyle(const AppSettings &s, const Theme &appTheme);
    /// `markdown` is the raw editor text; front matter / math / emphasis are handled here.
    void setMarkdownText(const QString &markdown, const QString &baseDir);
    int  headingCount() const;
    void scrollToHeading(int index);
    void scrollToFraction(double f);

signals:
    void headingActivated(int index);      // double-click on a heading
    void openFileRequested(const QString &path);

protected:
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void postProcess();
    void updateViewportMargins();
    void handleLink(const QUrl &url);
    QVector<QTextBlock> headingBlocks() const;

    AppSettings m_s;
    Theme   m_theme;                       // effective colours (may differ from the app theme)
    QString m_baseDir;
};
