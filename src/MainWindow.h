#pragma once
// =============================================================================
//  MainWindow.h - application shell.
//
//  Layout (VS Code inspired)
//    left dock   : EXPLORER (project folder tree) + OUTLINE (headings), tabbed
//    centre      : editor tabs | live preview, with a Find/Replace bar below
//    status bar  : cursor position, word count, encoding, line endings
//  Overlays      : Command Palette (Ctrl+Shift+P), Quick Open (Ctrl+P),
//                  Go to Heading (Ctrl+Shift+O)
// =============================================================================
#include <QMainWindow>
#include <QTimer>
#include <QVector>
#include <functional>
#include "OutlineParser.h"
#include "Settings.h"
#include "Theme.h"

class FindBar;
class QDockWidget;
class MarkdownEditor;
class PreviewView;
class QAction;
class QFileSystemModel;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

namespace Exporter { struct PandocFormat; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

    bool openFile(const QString &path);
    void openFolder(const QString &path);

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    // ---- construction ------------------------------------------------------
    void buildUi();
    void buildDocks();
    void buildMenus();
    void buildStatusBar();
    QWidget *buildWelcomePage();
    QAction *mk(QMenu *menu, const QString &text, const QKeySequence &sc, std::function<void()> fn);
    QAction *mkEd(QMenu *menu, const QString &text, const QKeySequence &sc, std::function<void(MarkdownEditor *)> fn);

    // ---- documents ---------------------------------------------------------
    MarkdownEditor *currentEditor() const;
    MarkdownEditor *editorAt(int index) const;
    MarkdownEditor *findEditor(const QString &path) const;
    MarkdownEditor *createEditor();
    void newFile(const QString &content = QString());
    bool saveEditor(MarkdownEditor *ed);
    bool saveEditorAs(MarkdownEditor *ed);
    bool maybeSave(MarkdownEditor *ed);
    bool closeTab(int index);
    void saveAll();
    void rememberRecent(const QString &path);

    // ---- UI refresh --------------------------------------------------------
    void applySettings();
    void tabChanged(int index);
    void updateTabTitle(MarkdownEditor *ed);
    void updateWindowTitle();
    void refreshPreview();
    void refreshOutline();
    void refreshCursorStatus();
    void refreshCounts();
    void syncOutlineSelection();
    void syncPreviewScroll();
    void jumpToHeading(int index);

    // ---- overlays / dialogs -------------------------------------------------
    void showCommandPalette();
    void showQuickOpen();
    void showGoToHeading();
    void showSettings();
    void explorerMenu(const QPoint &pos);
    // Explorer actions (toolbar buttons + context menu share these)
    QString explorerTargetDir() const;          // selected folder, else the project root
    void newFileIn(const QString &dir);
    void newFolderIn(const QString &dir);
    bool ensureProjectFolder();                 // asks for a folder when none is open
    void refreshExplorer();
    void refreshExplorerIcons();
    void buildExportMenu();
    void exportBuiltIn(const QString &kind);
    void exportPandoc(const Exporter::PandocFormat &fmt);
    QString exportBaseDir(MarkdownEditor *ed) const;
    QString exportSuggestedPath(MarkdownEditor *ed, const QString &ext) const;
    void announceExport(const QString &path);

    // ---- state ---------------------------------------------------------------
    QTabWidget      *m_tabs = nullptr;
    PreviewView     *m_preview = nullptr;
    QSplitter       *m_splitter = nullptr;
    QStackedWidget  *m_stack = nullptr;
    FindBar         *m_find = nullptr;
    QTreeView       *m_tree = nullptr;
    QFileSystemModel*m_fs = nullptr;
    QLabel          *m_folderLabel = nullptr;
    QToolButton     *m_btnNewFile = nullptr, *m_btnNewFolder = nullptr,
                    *m_btnRefresh = nullptr, *m_btnCollapse = nullptr;
    QTreeWidget     *m_outline = nullptr;
    QMenu           *m_exportMenu = nullptr;
    QMenu           *m_recentMenu = nullptr;
    QAction         *m_actPreview = nullptr, *m_actTabbed = nullptr;
    QDockWidget     *m_explorerDock = nullptr, *m_outlineDock = nullptr;
    QLabel          *m_posLabel = nullptr, *m_countLabel = nullptr, *m_eolLabel = nullptr, *m_encLabel = nullptr;

    QTimer m_previewTimer, m_autosaveTimer;
    QVector<OutlineItem> m_items;
    QVector<QTreeWidgetItem *> m_outlineWidgets;
    bool m_selectingOutline = false;

    QString m_folder;
    int     m_untitledCounter = 1;
    Theme   m_theme;
};
