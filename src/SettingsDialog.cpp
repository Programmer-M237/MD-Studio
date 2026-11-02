#include "SettingsDialog.h"
#include "Exporter.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>

SettingsDialog::SettingsDialog(const AppSettings &cur, QWidget *parent)
    : QDialog(parent), m_s(cur)
{
    setWindowTitle(tr("Settings"));
    resize(680, 460);

    m_cats  = new QListWidget;
    m_cats->setFixedWidth(150);
    m_cats->setStyleSheet("QListWidget::item { padding: 8px 10px; }");
    m_pages = new QStackedWidget;

    const QPair<QString, QWidget *> pages[] = {
        {tr("Editor"),     editorPage()},
        {tr("Appearance"), appearancePage()},
        {tr("Preview"),    previewPage()},
        {tr("Files"),      filesPage()},
        {tr("Export"),     exportPage()},
    };
    for (const auto &p : pages) { m_cats->addItem(p.first); m_pages->addWidget(p.second); }
    connect(m_cats, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_cats->setCurrentRow(0);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { emit applied(result()); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { emit applied(result()); });

    auto *body = new QHBoxLayout;
    body->addWidget(m_cats);
    body->addWidget(m_pages, 1);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(body, 1);
    lay->addWidget(buttons);
}

QWidget *SettingsDialog::editorPage()
{
    auto *w = new QWidget; auto *f = new QFormLayout(w);

    m_defaultFont = new QCheckBox(tr("Use default monospace font (Cascadia Code / Consolas)"));
    m_defaultFont->setChecked(m_s.fontFamily.isEmpty());
    m_font = new QFontComboBox; m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    if (!m_s.fontFamily.isEmpty()) m_font->setCurrentFont(QFont(m_s.fontFamily));
    m_font->setEnabled(!m_s.fontFamily.isEmpty());
    connect(m_defaultFont, &QCheckBox::toggled, m_font, &QWidget::setDisabled);

    m_fontSize = new QSpinBox; m_fontSize->setRange(7, 40); m_fontSize->setValue(m_s.fontSize);
    m_tab      = new QSpinBox; m_tab->setRange(1, 12);      m_tab->setValue(m_s.tabSize);
    m_wrap     = new QCheckBox(tr("Word wrap"));                    m_wrap->setChecked(m_s.wordWrap);
    m_lines    = new QCheckBox(tr("Show line numbers"));            m_lines->setChecked(m_s.lineNumbers);
    m_curLine  = new QCheckBox(tr("Highlight current line"));       m_curLine->setChecked(m_s.highlightCurrentLine);
    m_pairs    = new QCheckBox(tr("Auto-close brackets and quotes"));m_pairs->setChecked(m_s.autoClosePairs);
    m_suggest  = new QCheckBox(tr("Drop-down suggestions"));        m_suggest->setChecked(m_s.suggestions);
    m_ghost    = new QCheckBox(tr("Inline (ghost text) suggestions - Tab to accept")); m_ghost->setChecked(m_s.inlineSuggestions);
    m_minChars = new QSpinBox; m_minChars->setRange(1, 6); m_minChars->setValue(m_s.suggestMinChars);

    f->addRow(m_defaultFont);
    f->addRow(tr("Font family"), m_font);
    f->addRow(tr("Font size"), m_fontSize);
    f->addRow(tr("Tab size (spaces)"), m_tab);
    f->addRow(m_wrap); f->addRow(m_lines); f->addRow(m_curLine); f->addRow(m_pairs);
    f->addRow(m_suggest); f->addRow(m_ghost);
    f->addRow(tr("Characters before suggesting"), m_minChars);
    return w;
}

QWidget *SettingsDialog::appearancePage()
{
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    m_theme = new QComboBox; m_theme->addItems({"Dark", "Light"}); m_theme->setCurrentText(m_s.theme);
    m_showPreview = new QCheckBox(tr("Show preview pane")); m_showPreview->setChecked(m_s.showPreview);
    m_sync = new QCheckBox(tr("Synchronise editor and preview scrolling")); m_sync->setChecked(m_s.syncScroll);
    f->addRow(tr("Colour theme"), m_theme);
    f->addRow(m_showPreview);
    f->addRow(m_sync);
    return w;
}

// ---- Preview page: everything about how the rendered Markdown looks --------------------------------
QWidget *SettingsDialog::previewPage()
{
    auto *w = new QWidget;
    auto *outer = new QVBoxLayout(w);

    // --- text
    auto *text = new QGroupBox(tr("Text"));
    auto *tf = new QFormLayout(text);
    m_pvDefaultFont = new QCheckBox(tr("Use the default sans-serif font"));
    m_pvFont = new QFontComboBox;
    connect(m_pvDefaultFont, &QCheckBox::toggled, m_pvFont, &QWidget::setDisabled);
    m_prevSize = new QSpinBox; m_prevSize->setRange(8, 40); m_prevSize->setSuffix(" pt");
    m_pvHeadScale = new QSpinBox; m_pvHeadScale->setRange(60, 200); m_pvHeadScale->setSuffix(" %");
    m_pvLineH = new QSpinBox; m_pvLineH->setRange(100, 250); m_pvLineH->setSuffix(" %");
    m_pvParaGap = new QSpinBox; m_pvParaGap->setRange(0, 48); m_pvParaGap->setSuffix(" px");
    tf->addRow(m_pvDefaultFont);
    tf->addRow(tr("Font family"), m_pvFont);
    tf->addRow(tr("Font size"), m_prevSize);
    tf->addRow(tr("Heading size"), m_pvHeadScale);
    tf->addRow(tr("Line spacing"), m_pvLineH);
    tf->addRow(tr("Paragraph spacing"), m_pvParaGap);

    // --- code
    auto *code = new QGroupBox(tr("Code"));
    auto *cf = new QFormLayout(code);
    m_pvDefaultCode = new QCheckBox(tr("Use the default monospace font"));
    m_pvCodeFont = new QFontComboBox; m_pvCodeFont->setFontFilters(QFontComboBox::MonospacedFonts);
    connect(m_pvDefaultCode, &QCheckBox::toggled, m_pvCodeFont, &QWidget::setDisabled);
    m_pvCodePct = new QSpinBox; m_pvCodePct->setRange(60, 150); m_pvCodePct->setSuffix(" % of text size");
    cf->addRow(m_pvDefaultCode);
    cf->addRow(tr("Code font"), m_pvCodeFont);
    cf->addRow(tr("Code size"), m_pvCodePct);

    // --- page
    auto *page = new QGroupBox(tr("Page"));
    auto *pf = new QFormLayout(page);
    m_pvTheme = new QComboBox; m_pvTheme->addItems({"Follow app", "Light", "Dark"});
    m_pvMargin = new QSpinBox; m_pvMargin->setRange(0, 120); m_pvMargin->setSuffix(" px");
    m_pvMaxW = new QSpinBox; m_pvMaxW->setRange(0, 3000); m_pvMaxW->setSingleStep(50); m_pvMaxW->setSuffix(" px");
    m_pvMaxW->setSpecialValueText(tr("Full width"));
    pf->addRow(tr("Preview colours"), m_pvTheme);
    pf->addRow(tr("Page margin"), m_pvMargin);
    pf->addRow(tr("Maximum text width"), m_pvMaxW);

    // --- math
    auto *math = new QGroupBox(tr("LaTeX math"));
    auto *mf = new QFormLayout(math);
    m_renderMath = new QCheckBox(tr("Render formulas: $...$  $$...$$  \\(...\\)  \\[...\\]  ```math"));
    m_mathScale = new QSpinBox; m_mathScale->setRange(50, 250); m_mathScale->setSuffix(" % of text size");
    mf->addRow(m_renderMath);
    mf->addRow(tr("Formula size"), m_mathScale);

    auto *reset = new QPushButton(tr("Reset preview settings to defaults"));
    connect(reset, &QPushButton::clicked, this, [this] {
        AppSettings d = m_s;
        d.resetPreviewDefaults();
        loadPreviewControls(d);
    });

    auto *grid = new QGridLayout;
    grid->addWidget(text, 0, 0, 2, 1);
    grid->addWidget(code, 0, 1);
    grid->addWidget(page, 1, 1);
    outer->addLayout(grid);
    outer->addWidget(math);
    outer->addWidget(reset, 0, Qt::AlignLeft);
    outer->addStretch(1);

    loadPreviewControls(m_s);
    return w;
}

void SettingsDialog::loadPreviewControls(const AppSettings &s)
{
    m_pvDefaultFont->setChecked(s.previewFontFamily.isEmpty());
    if (!s.previewFontFamily.isEmpty()) m_pvFont->setCurrentFont(QFont(s.previewFontFamily));
    m_pvFont->setEnabled(!s.previewFontFamily.isEmpty());
    m_prevSize->setValue(s.previewFontSize);
    m_pvHeadScale->setValue(s.previewHeadingScale);
    m_pvLineH->setValue(s.previewLineHeight);
    m_pvParaGap->setValue(s.previewParagraphSpacing);
    m_pvDefaultCode->setChecked(s.previewCodeFamily.isEmpty());
    if (!s.previewCodeFamily.isEmpty()) m_pvCodeFont->setCurrentFont(QFont(s.previewCodeFamily));
    m_pvCodeFont->setEnabled(!s.previewCodeFamily.isEmpty());
    m_pvCodePct->setValue(s.previewCodePercent);
    m_pvTheme->setCurrentText(s.previewTheme);
    m_pvMargin->setValue(s.previewMargin);
    m_pvMaxW->setValue(s.previewMaxWidth);
    m_renderMath->setChecked(s.renderMath);
    m_mathScale->setValue(s.mathScale);
}

QWidget *SettingsDialog::filesPage()
{
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    m_autosave = new QCheckBox(tr("Auto-save modified files")); m_autosave->setChecked(m_s.autosave);
    m_autosaveSecs = new QSpinBox; m_autosaveSecs->setRange(5, 600); m_autosaveSecs->setSuffix(tr(" s"));
    m_autosaveSecs->setValue(m_s.autosaveSeconds);
    f->addRow(m_autosave);
    f->addRow(tr("Auto-save interval"), m_autosaveSecs);
    return w;
}

QWidget *SettingsDialog::exportPage()
{
    auto *w = new QWidget; auto *f = new QFormLayout(w);

    m_pandoc = new QLineEdit(m_s.pandocPath);
    m_pandoc->setPlaceholderText(tr("Leave empty to search PATH"));
    auto *browse = new QPushButton(tr("Browse..."));
    auto *detect = new QPushButton(tr("Detect"));
    auto *status = new QLabel;
    auto refresh = [this, status] {
        AppSettings tmp = m_s; tmp.pandocPath = m_pandoc->text();
        const QString p = Exporter::findPandoc(tmp);
        status->setText(p.isEmpty() ? tr("Pandoc not found") : tr("Found: %1").arg(p));
    };
    connect(browse, &QPushButton::clicked, this, [this, refresh] {
        const QString p = QFileDialog::getOpenFileName(this, tr("Select pandoc executable"));
        if (!p.isEmpty()) { m_pandoc->setText(p); refresh(); }
    });
    connect(detect, &QPushButton::clicked, this, [this, refresh] { m_pandoc->clear(); refresh(); });
    refresh();

    auto *row = new QHBoxLayout;
    row->addWidget(m_pandoc, 1); row->addWidget(browse); row->addWidget(detect);

    m_page = new QComboBox; m_page->addItems({"A4", "Letter", "Legal", "A5"}); m_page->setCurrentText(m_s.pdfPageSize);
    m_margin = new QSpinBox; m_margin->setRange(5, 50); m_margin->setSuffix(" mm"); m_margin->setValue(m_s.pdfMarginMm);

    f->addRow(tr("Pandoc executable"), row);
    f->addRow(QString(), status);
    f->addRow(tr("PDF / DOCX page size"), m_page);
    f->addRow(tr("PDF margins"), m_margin);
    f->addRow(new QLabel(tr("Pandoc unlocks many extra formats (EPUB, LaTeX, RST, ...).\nBuilt-in exports (HTML, PDF, DOCX, ODT) need nothing installed.")));
    return w;
}

AppSettings SettingsDialog::result() const
{
    AppSettings s = m_s;
    s.fontFamily = m_defaultFont->isChecked() ? QString() : m_font->currentFont().family();
    s.fontSize = m_fontSize->value();
    s.tabSize  = m_tab->value();
    s.wordWrap = m_wrap->isChecked();
    s.lineNumbers = m_lines->isChecked();
    s.highlightCurrentLine = m_curLine->isChecked();
    s.autoClosePairs = m_pairs->isChecked();
    s.suggestions = m_suggest->isChecked();
    s.inlineSuggestions = m_ghost->isChecked();
    s.suggestMinChars = m_minChars->value();

    s.theme = m_theme->currentText();
    s.previewFontSize = m_prevSize->value();
    s.previewFontFamily = m_pvDefaultFont->isChecked() ? QString() : m_pvFont->currentFont().family();
    s.previewCodeFamily = m_pvDefaultCode->isChecked() ? QString() : m_pvCodeFont->currentFont().family();
    s.previewCodePercent = m_pvCodePct->value();
    s.previewLineHeight = m_pvLineH->value();
    s.previewParagraphSpacing = m_pvParaGap->value();
    s.previewHeadingScale = m_pvHeadScale->value();
    s.previewMargin = m_pvMargin->value();
    s.previewMaxWidth = m_pvMaxW->value();
    s.previewTheme = m_pvTheme->currentText();
    s.renderMath = m_renderMath->isChecked();
    s.mathScale = m_mathScale->value();
    s.showPreview = m_showPreview->isChecked();
    s.syncScroll = m_sync->isChecked();

    s.autosave = m_autosave->isChecked();
    s.autosaveSeconds = m_autosaveSecs->value();

    s.pandocPath = m_pandoc->text().trimmed();
    s.pdfPageSize = m_page->currentText();
    s.pdfMarginMm = m_margin->value();
    return s;
}
