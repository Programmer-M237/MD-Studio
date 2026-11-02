#pragma once
// =============================================================================
//  Settings.h - all user-configurable options in one struct.
//  Values are persisted with QSettings (registry on Windows, ini on Linux).
// =============================================================================
#include <QString>

struct AppSettings {
    // ---- Editor ----------------------------------------------------------
    QString fontFamily;                 // empty = built-in monospace fallback chain
    int  fontSize              = 12;
    int  tabSize               = 4;
    bool wordWrap              = true;
    bool lineNumbers           = true;
    bool highlightCurrentLine  = true;
    bool autoClosePairs        = true;  // ( [ { " ` auto-closing
    bool suggestions           = true;  // drop-down suggestion list
    bool inlineSuggestions     = true;  // grey "ghost text" completion
    int  suggestMinChars       = 2;     // typed chars before suggestions appear

    // ---- Appearance / preview -------------------------------------------
    QString theme              = "Dark"; // "Dark" or "Light"
    int  previewFontSize       = 13;
    bool showPreview           = true;
    bool syncScroll            = true;

    // ---- Preview appearance ("how the rendered page looks") -------------------
    QString previewFontFamily;                 // empty = built-in sans-serif chain
    QString previewCodeFamily;                 // empty = built-in monospace chain
    int  previewCodePercent    = 90;           // code size relative to body text (%)
    int  previewLineHeight     = 140;          // line spacing (%)
    int  previewParagraphSpacing = 10;         // px between paragraphs
    int  previewHeadingScale   = 100;          // multiplier for heading sizes (%)
    int  previewMargin         = 24;           // px around the page
    int  previewMaxWidth       = 0;            // px, 0 = use the full pane width
    QString previewTheme       = "Follow app"; // "Follow app", "Light" or "Dark"
    bool renderMath            = true;         // typeset LaTeX ($..$, $$..$$, ```math)
    int  mathScale             = 110;          // formula size relative to the text (%)

    // ---- Files -----------------------------------------------------------
    bool autosave              = false;
    int  autosaveSeconds       = 30;

    // ---- Export ----------------------------------------------------------
    QString pandocPath;                 // empty = search PATH
    QString pdfPageSize        = "A4";  // A4, Letter, Legal, A5
    int  pdfMarginMm           = 20;

    /// Restores every preview-appearance option to its default.
    void resetPreviewDefaults();
    void load();
    void save() const;
    static AppSettings &instance();
};
