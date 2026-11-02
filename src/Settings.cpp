#include "Settings.h"
#include <QSettings>

AppSettings &AppSettings::instance()
{
    static AppSettings s;
    return s;
}

void AppSettings::load()
{
    QSettings q;
    fontFamily           = q.value("editor/fontFamily", fontFamily).toString();
    fontSize             = q.value("editor/fontSize", fontSize).toInt();
    tabSize              = q.value("editor/tabSize", tabSize).toInt();
    wordWrap             = q.value("editor/wordWrap", wordWrap).toBool();
    lineNumbers          = q.value("editor/lineNumbers", lineNumbers).toBool();
    highlightCurrentLine = q.value("editor/highlightCurrentLine", highlightCurrentLine).toBool();
    autoClosePairs       = q.value("editor/autoClosePairs", autoClosePairs).toBool();
    suggestions          = q.value("editor/suggestions", suggestions).toBool();
    inlineSuggestions    = q.value("editor/inlineSuggestions", inlineSuggestions).toBool();
    suggestMinChars      = q.value("editor/suggestMinChars", suggestMinChars).toInt();

    theme                = q.value("appearance/theme", theme).toString();
    previewFontSize      = q.value("appearance/previewFontSize", previewFontSize).toInt();
    showPreview          = q.value("appearance/showPreview", showPreview).toBool();
    syncScroll           = q.value("appearance/syncScroll", syncScroll).toBool();

    previewFontFamily    = q.value("preview/fontFamily", previewFontFamily).toString();
    previewCodeFamily    = q.value("preview/codeFamily", previewCodeFamily).toString();
    previewCodePercent   = q.value("preview/codePercent", previewCodePercent).toInt();
    previewLineHeight    = q.value("preview/lineHeight", previewLineHeight).toInt();
    previewParagraphSpacing = q.value("preview/paragraphSpacing", previewParagraphSpacing).toInt();
    previewHeadingScale  = q.value("preview/headingScale", previewHeadingScale).toInt();
    previewMargin        = q.value("preview/margin", previewMargin).toInt();
    previewMaxWidth      = q.value("preview/maxWidth", previewMaxWidth).toInt();
    previewTheme         = q.value("preview/theme", previewTheme).toString();
    renderMath           = q.value("preview/renderMath", renderMath).toBool();
    mathScale            = q.value("preview/mathScale", mathScale).toInt();

    autosave             = q.value("files/autosave", autosave).toBool();
    autosaveSeconds      = q.value("files/autosaveSeconds", autosaveSeconds).toInt();

    pandocPath           = q.value("export/pandocPath", pandocPath).toString();
    pdfPageSize          = q.value("export/pdfPageSize", pdfPageSize).toString();
    pdfMarginMm          = q.value("export/pdfMarginMm", pdfMarginMm).toInt();
}

void AppSettings::save() const
{
    QSettings q;
    q.setValue("editor/fontFamily", fontFamily);
    q.setValue("editor/fontSize", fontSize);
    q.setValue("editor/tabSize", tabSize);
    q.setValue("editor/wordWrap", wordWrap);
    q.setValue("editor/lineNumbers", lineNumbers);
    q.setValue("editor/highlightCurrentLine", highlightCurrentLine);
    q.setValue("editor/autoClosePairs", autoClosePairs);
    q.setValue("editor/suggestions", suggestions);
    q.setValue("editor/inlineSuggestions", inlineSuggestions);
    q.setValue("editor/suggestMinChars", suggestMinChars);

    q.setValue("appearance/theme", theme);
    q.setValue("appearance/previewFontSize", previewFontSize);
    q.setValue("appearance/showPreview", showPreview);
    q.setValue("appearance/syncScroll", syncScroll);

    q.setValue("preview/fontFamily", previewFontFamily);
    q.setValue("preview/codeFamily", previewCodeFamily);
    q.setValue("preview/codePercent", previewCodePercent);
    q.setValue("preview/lineHeight", previewLineHeight);
    q.setValue("preview/paragraphSpacing", previewParagraphSpacing);
    q.setValue("preview/headingScale", previewHeadingScale);
    q.setValue("preview/margin", previewMargin);
    q.setValue("preview/maxWidth", previewMaxWidth);
    q.setValue("preview/theme", previewTheme);
    q.setValue("preview/renderMath", renderMath);
    q.setValue("preview/mathScale", mathScale);

    q.setValue("files/autosave", autosave);
    q.setValue("files/autosaveSeconds", autosaveSeconds);

    q.setValue("export/pandocPath", pandocPath);
    q.setValue("export/pdfPageSize", pdfPageSize);
    q.setValue("export/pdfMarginMm", pdfMarginMm);
}

void AppSettings::resetPreviewDefaults()
{
    const AppSettings d;                       // a default-constructed instance holds the defaults
    previewFontSize = d.previewFontSize;       previewFontFamily = d.previewFontFamily;
    previewCodeFamily = d.previewCodeFamily;   previewCodePercent = d.previewCodePercent;
    previewLineHeight = d.previewLineHeight;   previewParagraphSpacing = d.previewParagraphSpacing;
    previewHeadingScale = d.previewHeadingScale; previewMargin = d.previewMargin;
    previewMaxWidth = d.previewMaxWidth;       previewTheme = d.previewTheme;
    renderMath = d.renderMath;                 mathScale = d.mathScale;
}
