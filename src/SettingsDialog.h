#pragma once
// =============================================================================
//  SettingsDialog.h - preferences window (sidebar categories + stacked pages).
// =============================================================================
#include <QDialog>
#include "Settings.h"

class QCheckBox; class QComboBox; class QFontComboBox; class QSpinBox; class QLineEdit;
class QListWidget; class QStackedWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const AppSettings &current, QWidget *parent = nullptr);
    AppSettings result() const;

signals:
    void applied(const AppSettings &s);

private:
    QWidget *editorPage();
    QWidget *appearancePage();
    QWidget *previewPage();
    void loadPreviewControls(const AppSettings &s);
    QWidget *filesPage();
    QWidget *exportPage();

    AppSettings m_s;
    QListWidget *m_cats; QStackedWidget *m_pages;

    // editor
    QFontComboBox *m_font; QCheckBox *m_defaultFont; QSpinBox *m_fontSize, *m_tab, *m_minChars;
    QCheckBox *m_wrap, *m_lines, *m_curLine, *m_pairs, *m_suggest, *m_ghost;
    // appearance
    QComboBox *m_theme; QCheckBox *m_showPreview, *m_sync;
    // preview (how the rendered page looks)
    QCheckBox *m_pvDefaultFont, *m_pvDefaultCode, *m_renderMath;
    QFontComboBox *m_pvFont, *m_pvCodeFont;
    QSpinBox *m_prevSize, *m_pvCodePct, *m_pvLineH, *m_pvParaGap, *m_pvHeadScale, *m_pvMargin, *m_pvMaxW, *m_mathScale;
    QComboBox *m_pvTheme;
    // files
    QCheckBox *m_autosave; QSpinBox *m_autosaveSecs;
    // export
    QLineEdit *m_pandoc; QComboBox *m_page; QSpinBox *m_margin;
};
