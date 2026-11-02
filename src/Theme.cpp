#include "Theme.h"
#include <QApplication>
#include <QPalette>

Theme Theme::dark_()
{
    Theme t; t.dark = true;
    t.window   = "#252526"; t.sidebar = "#252526"; t.base = "#1e1e1e";
    t.text     = "#cccccc"; t.dimText = "#8b8b8b"; t.button = "#3c3c3c";
    t.border   = "#3c3c3c"; t.accent  = "#007acc"; t.selection = "#264f78";
    t.editorBg = "#1e1e1e"; t.editorFg = "#d4d4d4";
    t.gutterBg = "#1e1e1e"; t.gutterFg = "#858585"; t.gutterActive = "#c6c6c6";
    t.currentLine = "#2a2d2e"; t.ghost = "#6e6e6e";
    t.heading = "#569cd6"; t.bold = "#dcdcaa"; t.italic = "#ce9178";
    t.code = "#d7ba7d"; t.codeBg = "#2d2d2d"; t.link = "#4ec9b0";
    t.quote = "#6a9955"; t.listMarker = "#c586c0"; t.rule = "#808080";
    t.comment = "#6a9955"; t.strike = "#9a9a9a"; t.math = "#b5cea8";
    return t;
}

Theme Theme::light_()
{
    Theme t; t.dark = false;
    t.window   = "#f3f3f3"; t.sidebar = "#f3f3f3"; t.base = "#ffffff";
    t.text     = "#333333"; t.dimText = "#767676"; t.button = "#e6e6e6";
    t.border   = "#d4d4d4"; t.accent  = "#007acc"; t.selection = "#add6ff";
    t.editorBg = "#ffffff"; t.editorFg = "#1f1f1f";
    t.gutterBg = "#ffffff"; t.gutterFg = "#237893"; t.gutterActive = "#0b216f";
    t.currentLine = "#f0f0f0"; t.ghost = "#a0a0a0";
    t.heading = "#0451a5"; t.bold = "#795e26"; t.italic = "#a31515";
    t.code = "#a31515"; t.codeBg = "#f0f0f0"; t.link = "#0b7a75";
    t.quote = "#008000"; t.listMarker = "#af00db"; t.rule = "#999999";
    t.comment = "#008000"; t.strike = "#777777"; t.math = "#098658";
    return t;
}

Theme Theme::byName(const QString &name)
{
    return name.compare("Light", Qt::CaseInsensitive) == 0 ? light_() : dark_();
}

void Theme::apply(QApplication &app) const
{
    app.setStyle("Fusion");

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::Highlight, selection);
    p.setColor(QPalette::HighlightedText, dark ? QColor("#ffffff") : QColor("#000000"));
    p.setColor(QPalette::PlaceholderText, dimText);
    p.setColor(QPalette::Disabled, QPalette::Text, dimText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dimText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dimText);
    app.setPalette(p);

    // A touch of VS Code styling: flat tabs with an accent bar, accent status bar.
    const QString qss = QString(R"(
        QTabWidget::pane { border: none; }
        QTabBar::tab { padding: 7px 14px; background: %1; color: %2; border-right: 1px solid %3; }
        QTabBar::tab:selected { background: %4; color: %5; border-top: 2px solid %6; }
        QTabBar::tab:hover:!selected { background: %7; }
        QDockWidget { font-weight: normal; }
        QDockWidget::title { background: %1; padding: 6px 8px; text-align: left; }
        QTreeView, QTreeWidget, QListWidget { border: none; background: %8; outline: 0; }
        QTreeView::item, QTreeWidget::item { padding: 2px 0; }
        QStatusBar { background: %6; color: white; }
        QStatusBar QLabel { color: white; padding: 0 8px; }
        QStatusBar::item { border: none; }
        QToolTip { background: %4; color: %5; border: 1px solid %3; }
        QMenu { border: 1px solid %3; }
        QLineEdit { padding: 4px; border: 1px solid %3; background: %4; }
        QLineEdit:focus { border: 1px solid %6; }
        QSplitter::handle { background: %3; }
    )")
        .arg(window.name(), dimText.name(), border.name(), base.name(), text.name(),
             accent.name(), currentLine.name(), sidebar.name());
    app.setStyleSheet(qss);
}

QString Theme::previewCss() const
{
    // QTextBrowser understands only a subset of CSS; keep this simple.
    return QString(R"(
        a { color: %1; text-decoration: none; }
        h1, h2 { color: %2; }
        h3, h4, h5, h6 { color: %2; }
        code { background-color: %3; color: %4; }
        pre { background-color: %3; color: %5; }
        blockquote { color: %6; }
        th { background-color: %3; }
    )").arg(link.name(), heading.name(), codeBg.name(), code.name(), editorFg.name(), quote.name());
}
