#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMap>
#include <QRegularExpression>
#include <QUrl>
#include "MainWindow.h"

#include "Exporter.h"
#include "FindBar.h"
#include "MarkdownEditor.h"
#include "PreviewView.h"
#include "QuickPick.h"
#include "SettingsDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const char *kCheatSheet = R"MD(# Markdown cheat sheet

Welcome to **MD Studio**. Press `Ctrl+Shift+P` for the command palette, `Ctrl+P` to open a file in your project,
and `Ctrl+Shift+O` to jump to a heading. Start typing a word such as `table`, `code` or `link` and press `Tab`.

## Text formatting

You can write **bold**, _italic_, ~~strikethrough~~ and `inline code`. Links look like [this](https://www.qt.io).

## Lists

- Bullet item
  - Nested item
- [x] A finished task
- [ ] Something still to do

1. First
2. Second
3. Third

## Quotes and code

> A block quote. Press Enter twice to leave it.

```cpp
int main() { return 0; }
```

## Tables

| Feature | Shortcut |
| --- | --- |
| Bold | Ctrl+B |
| Italic | Ctrl+I |
| Preview | Ctrl+Shift+V |

---

## LaTeX math

Inline formulas use single dollars: $E = mc^2$ and $\frac{a}{b} + \sqrt{x^2+1}$.
Display equations use double dollars (or a ```math block):

$$
\sum_{n=1}^{\infty} \frac{1}{n^2} = \frac{\pi^2}{6}
\qquad
\begin{pmatrix} a & b \\ c & d \end{pmatrix}
$$

Type `\fr` and press **Tab** for `\frac{}{}`, or use **Ctrl+M** / **Ctrl+Shift+M**.
A price like $5 and $10 is left alone.

### Export

Use **File > Export** to create HTML, PDF, Word (DOCX) or ODT files. With Pandoc installed you also get EPUB, LaTeX and more.
)MD";

// Draws the small monochrome toolbar icons (so they follow the theme colour).
// kind: 0 = new file, 1 = new folder, 2 = refresh, 3 = collapse all
QIcon explorerIcon(int kind, const QColor &col)
{
    QPixmap pm(32, 32);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(col, 1.2);
    pen.setJoinStyle(Qt::RoundJoin); pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    auto plus = [&](qreal cx, qreal cy) {
        p.save();
        p.setPen(QPen(col, 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(cx - 2.5, cy), QPointF(cx + 2.5, cy));
        p.drawLine(QPointF(cx, cy - 2.5), QPointF(cx, cy + 2.5));
        p.restore();
    };
    switch (kind) {
    case 0: {   // file with folded corner + plus
        QPolygonF f; f << QPointF(2.5, 1.5) << QPointF(8, 1.5) << QPointF(10.5, 4) << QPointF(10.5, 14.5) << QPointF(2.5, 14.5);
        p.drawPolygon(f);
        p.drawLine(QPointF(8, 1.5), QPointF(8, 4)); p.drawLine(QPointF(8, 4), QPointF(10.5, 4));
        p.setBrush(Qt::NoBrush);
        plus(12.5, 11.5);
        break; }
    case 1: {   // folder + plus
        QPainterPath fp;
        fp.moveTo(1.5, 3.5); fp.lineTo(5.5, 3.5); fp.lineTo(7, 5); fp.lineTo(11, 5);
        fp.lineTo(11, 13.5); fp.lineTo(1.5, 13.5); fp.closeSubpath();
        p.drawPath(fp);
        plus(12.5, 11.5);
        break; }
    case 2: {   // refresh: open circle with arrow head
        p.drawArc(QRectF(2.5, 2.5, 11, 11), 40 * 16, 290 * 16);
        QPolygonF a; a << QPointF(11.2, 1.8) << QPointF(13.8, 4.4) << QPointF(10.2, 5);
        p.setBrush(col); p.drawPolygon(a);
        break; }
    default: {  // collapse all: two stacked chevrons pointing up
        p.drawPolyline(QPolygonF() << QPointF(3.5, 7) << QPointF(8, 2.8) << QPointF(12.5, 7));
        p.drawPolyline(QPolygonF() << QPointF(3.5, 13) << QPointF(8, 8.8) << QPointF(12.5, 13));
        break; }
    }
    return QIcon(pm);
}

bool looksBinary(const QByteArray &data)
{
    return data.left(8192).contains('\0');
}

} // namespace

// =============================================================================
//  Construction
// =============================================================================
MainWindow::MainWindow()
{
    AppSettings::instance().load();
    m_theme = Theme::byName(AppSettings::instance().theme);
    m_theme.apply(*qApp);

    setWindowTitle("MD Studio");
    resize(1360, 840);
    setAcceptDrops(true);

    buildUi();
    buildDocks();
    buildMenus();
    buildStatusBar();

    // Debounced preview/outline refresh: re-render 250 ms after the last keystroke.
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(250);
    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        refreshPreview(); refreshOutline(); refreshCounts();
    });
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] {
        for (int i = 0; i < m_tabs->count(); ++i) {
            MarkdownEditor *ed = editorAt(i);
            if (ed && ed->document()->isModified() && !ed->filePath.isEmpty()) saveEditor(ed);
        }
    });

    applySettings();

    // Restore the previous session.
    QSettings q;
    restoreGeometry(q.value("window/geometry").toByteArray());
    restoreState(q.value("window/state").toByteArray());
    const QString folder = q.value("session/folder").toString();
    if (!folder.isEmpty() && QFileInfo(folder).isDir()) openFolder(folder);
    for (const QString &f : q.value("session/files").toStringList())
        if (QFileInfo::exists(f)) openFile(f);
    if (m_tabs->count() == 0) m_stack->setCurrentIndex(0);
}

void MainWindow::buildUi()
{
    m_stack = new QStackedWidget;
    m_stack->addWidget(buildWelcomePage());

    auto *editorPage = new QWidget;
    auto *lay = new QVBoxLayout(editorPage);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_preview = new PreviewView;

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(m_tabs);
    m_splitter->addWidget(m_preview);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);

    m_find = new FindBar;
    lay->addWidget(m_splitter, 1);
    lay->addWidget(m_find);

    m_stack->addWidget(editorPage);
    setCentralWidget(m_stack);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::tabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
    connect(m_preview, &PreviewView::headingActivated, this, [this](int idx) {
        if (MarkdownEditor *ed = currentEditor(); ed && idx < m_items.size()) ed->gotoLine(m_items[idx].line);
    });
    connect(m_preview, &PreviewView::openFileRequested, this, [this](const QString &p) { openFile(p); });
}

QWidget *MainWindow::buildWelcomePage()
{
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    lay->setAlignment(Qt::AlignCenter);

    auto *title = new QLabel("<h1 style='font-weight:300'>MD Studio</h1>"
                             "<p style='color:gray'>A Markdown editor with a project explorer, live preview, outline and export.</p>");
    title->setAlignment(Qt::AlignCenter);

    auto btn = [this](const QString &text, std::function<void()> fn) {
        auto *b = new QPushButton(text);
        b->setMinimumWidth(220); b->setMinimumHeight(34);
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        return b;
    };
    auto *keys = new QLabel("<table cellpadding='4'>"
        "<tr><td>Command palette</td><td><b>Ctrl+Shift+P</b></td></tr>"
        "<tr><td>Quick open file</td><td><b>Ctrl+P</b></td></tr>"
        "<tr><td>Go to heading</td><td><b>Ctrl+Shift+O</b></td></tr>"
        "<tr><td>Toggle preview</td><td><b>Ctrl+Shift+V</b></td></tr></table>");
    keys->setAlignment(Qt::AlignCenter);

    lay->addWidget(title);
    lay->addSpacing(12);
    for (QPushButton *b : {btn(tr("Open Folder..."), [this] {
                                const QString d = QFileDialog::getExistingDirectory(this, tr("Open Folder"), m_folder);
                                if (!d.isEmpty()) openFolder(d); }),
                           btn(tr("New File"), [this] { newFile(); }),
                           btn(tr("Open File..."), [this] {
                                const QStringList fs = QFileDialog::getOpenFileNames(this, tr("Open File"), m_folder,
                                    tr("Markdown (*.md *.markdown *.mdown *.txt);;All files (*)"));
                                for (const QString &f : fs) openFile(f); }),
                           btn(tr("Markdown Cheat Sheet"), [this] { newFile(kCheatSheet); })})
        lay->addWidget(b, 0, Qt::AlignHCenter);
    lay->addSpacing(16);
    lay->addWidget(keys, 0, Qt::AlignHCenter);
    return page;
}

void MainWindow::buildDocks()
{
    // ---- Explorer -------------------------------------------------------------
    m_fs = new QFileSystemModel(this);
    m_fs->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_tree = new QTreeView;
    m_tree->setModel(m_fs);
    m_tree->setHeaderHidden(true);
    m_tree->setAnimated(true);
    m_tree->setIndentation(14);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int c = 1; c < 4; ++c) m_tree->setColumnHidden(c, true);
    m_tree->setVisible(true);

    m_folderLabel = new QLabel(tr("No folder opened"));
    m_folderLabel->setContentsMargins(8, 6, 8, 6);
    auto *openBtn = new QPushButton(tr("Open Folder..."));
    connect(openBtn, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Open Folder"), m_folder);
        if (!d.isEmpty()) openFolder(d);
    });

    auto *explorer = new QWidget;
    auto *el = new QVBoxLayout(explorer);
    el->setContentsMargins(0, 0, 0, 0); el->setSpacing(0);
    // Header row like VS Code: project name on the left, action buttons on the right.
    auto mkBtn = [this](const QString &tip) {
        auto *b = new QToolButton;
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    m_btnNewFile   = mkBtn(tr("New File..."));
    m_btnNewFolder = mkBtn(tr("New Folder..."));
    m_btnRefresh   = mkBtn(tr("Refresh Explorer"));
    m_btnCollapse  = mkBtn(tr("Collapse Folders in Explorer"));
    connect(m_btnNewFile,   &QToolButton::clicked, this, [this] { newFileIn(explorerTargetDir()); });
    connect(m_btnNewFolder, &QToolButton::clicked, this, [this] { newFolderIn(explorerTargetDir()); });
    connect(m_btnRefresh,   &QToolButton::clicked, this, &MainWindow::refreshExplorer);
    connect(m_btnCollapse,  &QToolButton::clicked, this, [this] { m_tree->collapseAll(); });

    auto *header = new QWidget;
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->setSpacing(0);
    hl->addWidget(m_folderLabel, 1);
    for (QToolButton *b : {m_btnNewFile, m_btnNewFolder, m_btnRefresh, m_btnCollapse}) hl->addWidget(b);

    el->addWidget(header);
    el->addWidget(m_tree, 1);
    el->addWidget(openBtn);

    connect(m_tree, &QTreeView::activated, this, [this](const QModelIndex &i) {
        if (!m_fs->isDir(i)) openFile(m_fs->filePath(i));
    });
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &MainWindow::explorerMenu);

    auto *explorerDock = m_explorerDock = new QDockWidget(tr("EXPLORER"), this);
    explorerDock->setObjectName("explorerDock");
    explorerDock->setWidget(explorer);
    addDockWidget(Qt::LeftDockWidgetArea, explorerDock);

    // ---- Outline ------------------------------------------------------------
    m_outline = new QTreeWidget;
    m_outline->setHeaderHidden(true);
    m_outline->setIndentation(14);
    m_outline->setExpandsOnDoubleClick(false);
    connect(m_outline, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        if (m_selectingOutline) return;
        jumpToHeading(it->data(0, Qt::UserRole).toInt());
    });

    auto *outlineDock = m_outlineDock = new QDockWidget(tr("OUTLINE"), this);
    outlineDock->setObjectName("outlineDock");
    outlineDock->setWidget(m_outline);
    addDockWidget(Qt::LeftDockWidgetArea, outlineDock);

    // When the user stacks the docks into one tabbed group (drag one onto the other, or use
    // View > Tabbed Sidebar) the tab switcher belongs on TOP, like the editor tabs - not at the bottom.
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);

    // Stacked vertically so both are visible at once; users can tab them by dragging.
    splitDockWidget(explorerDock, outlineDock, Qt::Vertical);
    resizeDocks({explorerDock, outlineDock}, {420, 300}, Qt::Vertical);
    resizeDocks({explorerDock}, {280}, Qt::Horizontal);

    explorerDock->toggleViewAction()->setShortcut(QKeySequence("Ctrl+Shift+E"));
    outlineDock->toggleViewAction()->setShortcut(QKeySequence("Ctrl+Shift+U"));
}

QAction *MainWindow::mk(QMenu *menu, const QString &text, const QKeySequence &sc, std::function<void()> fn)
{
    QAction *a = menu->addAction(text);
    if (!sc.isEmpty()) a->setShortcut(sc);
    connect(a, &QAction::triggered, this, [fn] { fn(); });
    return a;
}

QAction *MainWindow::mkEd(QMenu *menu, const QString &text, const QKeySequence &sc, std::function<void(MarkdownEditor *)> fn)
{
    return mk(menu, text, sc, [this, fn] { if (MarkdownEditor *e = currentEditor()) fn(e); });
}

void MainWindow::buildMenus()
{
    AppSettings &S = AppSettings::instance();

    // ---- File ------------------------------------------------------------------
    QMenu *file = menuBar()->addMenu(tr("&File"));
    mk(file, tr("&New File"), QKeySequence::New, [this] { newFile(); });
    mk(file, tr("&Open File..."), QKeySequence::Open, [this] {
        const QStringList fs = QFileDialog::getOpenFileNames(this, tr("Open File"), m_folder,
            tr("Markdown (*.md *.markdown *.mdown *.txt);;All files (*)"));
        for (const QString &f : fs) openFile(f);
    });
    mk(file, tr("Open &Folder..."), QKeySequence("Ctrl+K, Ctrl+O"), [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Open Folder"), m_folder);
        if (!d.isEmpty()) openFolder(d);
    });
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, [this] {
        m_recentMenu->clear();
        const QStringList r = QSettings().value("recent/files").toStringList();
        for (const QString &p : r) {
            QAction *a = m_recentMenu->addAction(QDir::toNativeSeparators(p));
            connect(a, &QAction::triggered, this, [this, p] { openFile(p); });
        }
        if (r.isEmpty()) m_recentMenu->addAction(tr("(none)"))->setEnabled(false);
    });
    file->addSeparator();
    mkEd(file, tr("&Save"), QKeySequence::Save, [this](MarkdownEditor *e) { saveEditor(e); });
    mkEd(file, tr("Save &As..."), QKeySequence::SaveAs, [this](MarkdownEditor *e) { saveEditorAs(e); });
    mk(file, tr("Save A&ll"), QKeySequence("Ctrl+Alt+S"), [this] { saveAll(); });
    file->addSeparator();
    m_exportMenu = file->addMenu(tr("&Export As"));
    connect(m_exportMenu, &QMenu::aboutToShow, this, &MainWindow::buildExportMenu);
    file->addSeparator();
    mk(file, tr("&Close Tab"), QKeySequence("Ctrl+W"), [this] { if (m_tabs->count()) closeTab(m_tabs->currentIndex()); });
    mk(file, tr("Close Fol&der"), QKeySequence(), [this] {
        m_folder.clear(); m_fs->setRootPath(QString()); m_tree->setRootIndex(QModelIndex());
        m_folderLabel->setText(tr("No folder opened")); updateWindowTitle();
    });
    file->addSeparator();
    mk(file, tr("E&xit"), QKeySequence("Ctrl+Q"), [this] { close(); });

    // ---- Edit ------------------------------------------------------------------
    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    mkEd(edit, tr("&Undo"), QKeySequence::Undo, [](MarkdownEditor *e) { e->undo(); });
    mkEd(edit, tr("&Redo"), QKeySequence::Redo, [](MarkdownEditor *e) { e->redo(); });
    edit->addSeparator();
    mkEd(edit, tr("Cu&t"), QKeySequence::Cut, [](MarkdownEditor *e) { e->cut(); });
    mkEd(edit, tr("&Copy"), QKeySequence::Copy, [](MarkdownEditor *e) { e->copy(); });
    mkEd(edit, tr("&Paste"), QKeySequence::Paste, [](MarkdownEditor *e) { e->paste(); });
    mkEd(edit, tr("Select &All"), QKeySequence::SelectAll, [](MarkdownEditor *e) { e->selectAll(); });
    edit->addSeparator();
    mkEd(edit, tr("&Find"), QKeySequence::Find, [this](MarkdownEditor *e) {
        m_find->setEditor(e); m_find->openFind(e->textCursor().selectedText()); });
    mkEd(edit, tr("&Replace"), QKeySequence("Ctrl+H"), [this](MarkdownEditor *e) {
        m_find->setEditor(e); m_find->openReplace(e->textCursor().selectedText()); });
    mkEd(edit, tr("Find &Next"), QKeySequence("F3"), [this](MarkdownEditor *e) { m_find->setEditor(e); m_find->find(true); });
    mkEd(edit, tr("Find Pre&vious"), QKeySequence("Shift+F3"), [this](MarkdownEditor *e) { m_find->setEditor(e); m_find->find(false); });
    mkEd(edit, tr("&Go to Line..."), QKeySequence("Ctrl+G"), [](MarkdownEditor *e) {
        bool ok = false;
        const int n = QInputDialog::getInt(e, QObject::tr("Go to Line"), QObject::tr("Line number:"),
                                           e->currentLine() + 1, 1, e->blockCount(), 1, &ok);
        if (ok) e->gotoLine(n - 1);
    });
    edit->addSeparator();
    mkEd(edit, tr("Trigger &Suggestions"), QKeySequence("Ctrl+Space"), [](MarkdownEditor *e) {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier, " ");
        QCoreApplication::sendEvent(e, &ev);
    });
    edit->addSeparator();
    mk(edit, tr("&Preferences..."), QKeySequence("Ctrl+,"), [this] { showSettings(); });

    // ---- Format ----------------------------------------------------------------
    QMenu *fmt = menuBar()->addMenu(tr("F&ormat"));
    mkEd(fmt, tr("&Bold"), QKeySequence("Ctrl+B"), [](MarkdownEditor *e) { e->wrapSelection("**", "**", "bold"); });
    mkEd(fmt, tr("&Italic"), QKeySequence("Ctrl+I"), [](MarkdownEditor *e) { e->wrapSelection("_", "_", "italic"); });
    mkEd(fmt, tr("&Strikethrough"), QKeySequence("Alt+Shift+5"), [](MarkdownEditor *e) { e->wrapSelection("~~", "~~", "text"); });
    mkEd(fmt, tr("Inline &Code"), QKeySequence("Ctrl+E"), [](MarkdownEditor *e) { e->wrapSelection("`", "`", "code"); });
    fmt->addSeparator();
    mkEd(fmt, tr("Heading &1"), QKeySequence("Ctrl+1"), [](MarkdownEditor *e) { e->setHeadingLevel(1); });
    mkEd(fmt, tr("Heading &2"), QKeySequence("Ctrl+2"), [](MarkdownEditor *e) { e->setHeadingLevel(2); });
    mkEd(fmt, tr("Heading &3"), QKeySequence("Ctrl+3"), [](MarkdownEditor *e) { e->setHeadingLevel(3); });
    mkEd(fmt, tr("Heading &4"), QKeySequence("Ctrl+4"), [](MarkdownEditor *e) { e->setHeadingLevel(4); });
    mkEd(fmt, tr("Remove Heading"), QKeySequence(), [](MarkdownEditor *e) { e->setHeadingLevel(0); });
    fmt->addSeparator();
    mkEd(fmt, tr("Bulleted &List"), QKeySequence("Ctrl+Shift+8"), [](MarkdownEditor *e) { e->toggleLinePrefix("- "); });
    mkEd(fmt, tr("&Numbered List"), QKeySequence("Ctrl+Shift+7"), [](MarkdownEditor *e) { e->toggleLinePrefix("1. "); });
    mkEd(fmt, tr("&Task List"), QKeySequence("Ctrl+Shift+9"), [](MarkdownEditor *e) { e->toggleLinePrefix("- [ ] "); });
    mkEd(fmt, tr("Block &Quote"), QKeySequence("Ctrl+Shift+Q"), [](MarkdownEditor *e) { e->toggleLinePrefix("> "); });
    fmt->addSeparator();
    mkEd(fmt, tr("Insert &Link"), QKeySequence("Ctrl+Alt+L"), [](MarkdownEditor *e) {
        QString sel = e->textCursor().selectedText(); sel.replace(QChar::ParagraphSeparator, ' ');
        e->insertSnippet(sel.isEmpty() ? "[$0](https://)" : "[" + sel + "]($0)");
    });
    mkEd(fmt, tr("Insert I&mage"), QKeySequence(), [](MarkdownEditor *e) { e->insertSnippet("![$0](path/to/image.png)"); });
    mkEd(fmt, tr("Insert Code Bloc&k"), QKeySequence("Ctrl+Alt+E"), [](MarkdownEditor *e) {
        if (e->textCursor().hasSelection()) e->wrapSelection("```\n", "\n```");
        else e->insertSnippet("```\n$0\n```");
    });
    mkEd(fmt, tr("Insert &Table"), QKeySequence(), [](MarkdownEditor *e) {
        e->insertSnippet("| Column 1 | Column 2 | Column 3 |\n| --- | --- | --- |\n| $0 |  |  |\n");
    });
    mkEd(fmt, tr("Insert Horizontal &Rule"), QKeySequence(), [](MarkdownEditor *e) { e->insertSnippet("\n---\n$0"); });
    fmt->addSeparator();
    mkEd(fmt, tr("Inline &Math  $...$"), QKeySequence("Ctrl+M"), [](MarkdownEditor *e) { e->wrapSelection("$", "$", "x^2"); });
    mkEd(fmt, tr("Math &Block  $$...$$"), QKeySequence("Ctrl+Shift+M"), [](MarkdownEditor *e) {
        if (e->textCursor().hasSelection()) e->wrapSelection("$$\n", "\n$$");
        else e->insertSnippet("$$\n$0\n$$");
    });

    // ---- View ------------------------------------------------------------------
    QMenu *view = menuBar()->addMenu(tr("&View"));
    mk(view, tr("&Command Palette..."), QKeySequence("Ctrl+Shift+P"), [this] { showCommandPalette(); });
    mk(view, tr("Quick &Open File..."), QKeySequence("Ctrl+P"), [this] { showQuickOpen(); });
    mk(view, tr("Go to &Heading..."), QKeySequence("Ctrl+Shift+O"), [this] { showGoToHeading(); });
    view->addSeparator();
    for (QDockWidget *d : findChildren<QDockWidget *>()) view->addAction(d->toggleViewAction());
    m_actTabbed = mk(view, tr("&Tabbed Sidebar (Explorer + Outline)"), QKeySequence(), [this] {
        const bool tabbed = tabifiedDockWidgets(m_explorerDock).contains(m_outlineDock);
        if (tabbed) {                                       // -> stack them vertically
            splitDockWidget(m_explorerDock, m_outlineDock, Qt::Vertical);
            resizeDocks({m_explorerDock, m_outlineDock}, {420, 300}, Qt::Vertical);
        } else {                                            // -> one tabbed group, switcher on top
            tabifyDockWidget(m_explorerDock, m_outlineDock);
            m_explorerDock->raise();
        }
    });
    m_actTabbed->setCheckable(true);
    connect(view, &QMenu::aboutToShow, this, [this] {
        m_actTabbed->setChecked(tabifiedDockWidgets(m_explorerDock).contains(m_outlineDock));
    });
    m_actPreview = mk(view, tr("&Preview"), QKeySequence("Ctrl+Shift+V"), [this, &S] {
        S.showPreview = !S.showPreview; m_preview->setVisible(S.showPreview); S.save();
        if (S.showPreview) refreshPreview();
    });
    m_actPreview->setCheckable(true);
    m_actPreview->setChecked(S.showPreview);
    mk(view, tr("Word &Wrap"), QKeySequence("Alt+Z"), [this, &S] { S.wordWrap = !S.wordWrap; S.save(); applySettings(); });
    mk(view, tr("Toggle Dark / Light &Theme"), QKeySequence("Ctrl+Alt+T"), [this, &S] {
        S.theme = (S.theme == "Dark") ? "Light" : "Dark"; S.save(); applySettings();
    });
    view->addSeparator();
    mk(view, tr("Zoom &In"),  QKeySequence("Ctrl+="), [this, &S] { S.fontSize = qMin(40, S.fontSize + 1); S.save(); applySettings(); });
    mk(view, tr("Zoom &Out"), QKeySequence("Ctrl+-"), [this, &S] { S.fontSize = qMax(7, S.fontSize - 1);  S.save(); applySettings(); });
    mk(view, tr("&Reset Zoom"), QKeySequence("Ctrl+0"), [this, &S] { S.fontSize = 12; S.save(); applySettings(); });
    view->addSeparator();
    mk(view, tr("Next Tab"), QKeySequence("Ctrl+PgDown"), [this] {
        if (m_tabs->count()) m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count()); });
    mk(view, tr("Previous Tab"), QKeySequence("Ctrl+PgUp"), [this] {
        if (m_tabs->count()) m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count()); });

    // ---- Help ------------------------------------------------------------------
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    mk(help, tr("Markdown &Cheat Sheet"), QKeySequence(), [this] { newFile(kCheatSheet); });
    mk(help, tr("&About MD Studio"), QKeySequence(), [this] {
        QMessageBox::about(this, tr("About MD Studio"),
            tr("<h3>MD Studio %1</h3><p>A Markdown editor built with Qt %2.</p>"
               "<p>Explorer, live preview, outline, find/replace, smart suggestions, "
               "and export to HTML, PDF, DOCX, ODT (plus Pandoc formats).</p>")
                .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    });
}

void MainWindow::buildStatusBar()
{
    m_posLabel   = new QLabel; m_countLabel = new QLabel;
    m_eolLabel   = new QLabel; m_encLabel   = new QLabel("UTF-8");
    statusBar()->addPermanentWidget(m_posLabel);
    statusBar()->addPermanentWidget(m_countLabel);
    statusBar()->addPermanentWidget(m_encLabel);
    statusBar()->addPermanentWidget(m_eolLabel);
}

// =============================================================================
//  Settings
// =============================================================================
void MainWindow::applySettings()
{
    const AppSettings &S = AppSettings::instance();
    m_theme = Theme::byName(S.theme);
    m_theme.apply(*qApp);

    for (int i = 0; i < m_tabs->count(); ++i)
        if (MarkdownEditor *ed = editorAt(i)) ed->applySettings(S, m_theme);

    refreshExplorerIcons();
    m_preview->applyStyle(S, m_theme);
    m_preview->setVisible(S.showPreview && m_tabs->count() > 0);
    if (m_actPreview) m_actPreview->setChecked(S.showPreview);

    if (S.autosave) m_autosaveTimer.start(S.autosaveSeconds * 1000);
    else            m_autosaveTimer.stop();

    refreshPreview();
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(AppSettings::instance(), this);
    connect(&dlg, &SettingsDialog::applied, this, [this](const AppSettings &s) {
        AppSettings::instance() = s;
        s.save();
        applySettings();
    });
    dlg.exec();
}

// =============================================================================
//  Documents
// =============================================================================
MarkdownEditor *MainWindow::editorAt(int i) const { return qobject_cast<MarkdownEditor *>(m_tabs->widget(i)); }
MarkdownEditor *MainWindow::currentEditor() const { return qobject_cast<MarkdownEditor *>(m_tabs->currentWidget()); }

MarkdownEditor *MainWindow::findEditor(const QString &path) const
{
    const QString canon = QFileInfo(path).canonicalFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        MarkdownEditor *ed = editorAt(i);
        if (ed && !ed->filePath.isEmpty() && QFileInfo(ed->filePath).canonicalFilePath() == canon) return ed;
    }
    return nullptr;
}

MarkdownEditor *MainWindow::createEditor()
{
    auto *ed = new MarkdownEditor;
    ed->applySettings(AppSettings::instance(), m_theme);

    connect(ed, &QPlainTextEdit::textChanged, this, [this, ed] { if (ed == currentEditor()) m_previewTimer.start(); });
    connect(ed->document(), &QTextDocument::modificationChanged, this, [this, ed] { updateTabTitle(ed); updateWindowTitle(); });
    connect(ed, &QPlainTextEdit::cursorPositionChanged, this, [this, ed] {
        if (ed == currentEditor()) { refreshCursorStatus(); syncOutlineSelection(); }
    });
    connect(ed->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, ed] {
        if (ed == currentEditor()) syncPreviewScroll();
    });
    return ed;
}

void MainWindow::newFile(const QString &content)
{
    MarkdownEditor *ed = createEditor();
    if (!content.isEmpty()) ed->setPlainText(content);
    ed->document()->setModified(false);
    const int idx = m_tabs->addTab(ed, QString("Untitled-%1").arg(m_untitledCounter++));
    m_tabs->setCurrentIndex(idx);
    updateTabTitle(ed);
    ed->setFocus();
}

bool MainWindow::openFile(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return false;

    if (MarkdownEditor *existing = findEditor(path)) {
        m_tabs->setCurrentWidget(existing);
        return true;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Open File"), tr("Cannot read %1:\n%2").arg(path, f.errorString()));
        return false;
    }
    const QByteArray raw = f.readAll();
    if (looksBinary(raw)) {                      // images, PDFs ... => hand to the OS
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return false;
    }

    QString text = QString::fromUtf8(raw);
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);          // strip BOM
    const bool crlf = text.contains("\r\n");
    text.replace("\r\n", "\n");

    MarkdownEditor *ed = createEditor();
    ed->filePath = fi.absoluteFilePath();
    ed->crlf = crlf;
    ed->setPlainText(text);
    ed->document()->setModified(false);
    ed->moveCursor(QTextCursor::Start);

    const int idx = m_tabs->addTab(ed, fi.fileName());
    m_tabs->setTabToolTip(idx, QDir::toNativeSeparators(fi.absoluteFilePath()));
    m_tabs->setCurrentIndex(idx);
    updateTabTitle(ed);
    rememberRecent(fi.absoluteFilePath());
    ed->setFocus();
    return true;
}

void MainWindow::openFolder(const QString &path)
{
    m_folder = QFileInfo(path).absoluteFilePath();
    m_fs->setRootPath(m_folder);
    m_tree->setRootIndex(m_fs->index(m_folder));
    m_folderLabel->setText(QFileInfo(m_folder).fileName().toUpper());
    m_folderLabel->setToolTip(QDir::toNativeSeparators(m_folder));
    updateWindowTitle();
    statusBar()->showMessage(tr("Opened folder %1").arg(QDir::toNativeSeparators(m_folder)), 4000);
}

bool MainWindow::saveEditor(MarkdownEditor *ed)
{
    if (ed->filePath.isEmpty()) return saveEditorAs(ed);
    QString text = ed->toPlainText();
    if (ed->crlf) text.replace("\n", "\r\n");

    QSaveFile f(ed->filePath);                   // atomic write: no half-written files
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit()) {
        QMessageBox::warning(this, tr("Save"), tr("Cannot save %1:\n%2").arg(ed->filePath, f.errorString()));
        return false;
    }
    ed->document()->setModified(false);
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(ed->filePath).fileName()), 2500);
    return true;
}

bool MainWindow::saveEditorAs(MarkdownEditor *ed)
{
    const QString start = !ed->filePath.isEmpty() ? ed->filePath
                          : (m_folder.isEmpty() ? QDir::homePath() : m_folder) + "/untitled.md";
    const QString path = QFileDialog::getSaveFileName(this, tr("Save As"), start,
                            tr("Markdown (*.md *.markdown);;Text (*.txt);;All files (*)"));
    if (path.isEmpty()) return false;
    ed->filePath = path;
    const int idx = m_tabs->indexOf(ed);
    m_tabs->setTabToolTip(idx, QDir::toNativeSeparators(path));
    rememberRecent(path);
    const bool ok = saveEditor(ed);
    updateTabTitle(ed);
    updateWindowTitle();
    return ok;
}

bool MainWindow::maybeSave(MarkdownEditor *ed)
{
    if (!ed->document()->isModified()) return true;
    m_tabs->setCurrentWidget(ed);
    const QString name = ed->filePath.isEmpty() ? m_tabs->tabText(m_tabs->indexOf(ed)) : QFileInfo(ed->filePath).fileName();
    const auto r = QMessageBox::warning(this, tr("Unsaved changes"),
        tr("Do you want to save the changes to %1?").arg(name.chopped(name.endsWith(" \u25CF") ? 2 : 0)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Save)    return saveEditor(ed);
    if (r == QMessageBox::Discard) return true;
    return false;
}

bool MainWindow::closeTab(int index)
{
    MarkdownEditor *ed = editorAt(index);
    if (!ed || !maybeSave(ed)) return false;
    m_tabs->removeTab(index);
    ed->deleteLater();
    if (m_tabs->count() == 0) tabChanged(-1);
    return true;
}

void MainWindow::saveAll()
{
    for (int i = 0; i < m_tabs->count(); ++i)
        if (MarkdownEditor *ed = editorAt(i); ed && ed->document()->isModified()) saveEditor(ed);
}

void MainWindow::rememberRecent(const QString &path)
{
    QSettings q;
    QStringList r = q.value("recent/files").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 12) r.removeLast();
    q.setValue("recent/files", r);
}

// =============================================================================
//  UI refresh
// =============================================================================
void MainWindow::tabChanged(int)
{
    MarkdownEditor *ed = currentEditor();
    m_stack->setCurrentIndex(ed ? 1 : 0);
    m_preview->setVisible(ed && AppSettings::instance().showPreview);
    m_find->setEditor(ed);
    refreshPreview();
    refreshOutline();
    refreshCursorStatus();
    refreshCounts();
    updateWindowTitle();
    if (ed) ed->setFocus();
}

void MainWindow::updateTabTitle(MarkdownEditor *ed)
{
    const int idx = m_tabs->indexOf(ed);
    if (idx < 0) return;
    QString name = ed->filePath.isEmpty() ? m_tabs->tabText(idx) : QFileInfo(ed->filePath).fileName();
    if (name.endsWith(" \u25CF")) name.chop(2);
    m_tabs->setTabText(idx, ed->document()->isModified() ? name + " \u25CF" : name);
}

void MainWindow::updateWindowTitle()
{
    QString t;
    if (MarkdownEditor *ed = currentEditor()) {
        QString name = ed->filePath.isEmpty() ? m_tabs->tabText(m_tabs->currentIndex()) : QFileInfo(ed->filePath).fileName();
        if (name.endsWith(" \u25CF")) name.chop(2);
        t = (ed->document()->isModified() ? "\u25CF " : "") + name + " \u2014 ";
    }
    if (!m_folder.isEmpty()) t += QFileInfo(m_folder).fileName() + " \u2014 ";
    setWindowTitle(t + "MD Studio");
}

void MainWindow::refreshPreview()
{
    MarkdownEditor *ed = currentEditor();
    if (!ed || !m_preview->isVisible()) return;
    const QString dir = !ed->filePath.isEmpty() ? QFileInfo(ed->filePath).absolutePath()
                        : (m_folder.isEmpty() ? QDir::currentPath() : m_folder);
    m_preview->setMarkdownText(ed->toPlainText(), dir);   // the preview strips front matter and typesets math itself
    syncPreviewScroll();
}

void MainWindow::refreshOutline()
{
    m_outline->clear();
    m_outlineWidgets.clear();
    m_items.clear();
    MarkdownEditor *ed = currentEditor();
    if (!ed) return;

    m_items = parseOutline(ed->toPlainText());
    QVector<QPair<int, QTreeWidgetItem *>> stack;
    for (int i = 0; i < m_items.size(); ++i) {
        const OutlineItem &it = m_items[i];
        while (!stack.isEmpty() && stack.last().first >= it.level) stack.removeLast();
        auto *node = stack.isEmpty() ? new QTreeWidgetItem(m_outline) : new QTreeWidgetItem(stack.last().second);
        node->setText(0, it.title.isEmpty() ? tr("(empty heading)") : it.title);
        node->setData(0, Qt::UserRole, i);
        node->setToolTip(0, tr("Line %1").arg(it.line + 1));
        QFont f = node->font(0);
        f.setBold(it.level <= 2);
        node->setFont(0, f);
        stack.append({it.level, node});
        m_outlineWidgets.append(node);
    }
    m_outline->expandAll();
    syncOutlineSelection();
}

void MainWindow::syncOutlineSelection()
{
    MarkdownEditor *ed = currentEditor();
    if (!ed || m_items.isEmpty()) return;
    const int line = ed->currentLine();
    int best = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].line <= line) best = i; else break;
    }
    m_selectingOutline = true;
    if (best >= 0) { m_outline->setCurrentItem(m_outlineWidgets[best]); }
    else           { m_outline->clearSelection(); }
    m_selectingOutline = false;
}

void MainWindow::syncPreviewScroll()
{
    MarkdownEditor *ed = currentEditor();
    if (!ed || !AppSettings::instance().syncScroll || !m_preview->isVisible()) return;
    QScrollBar *sb = ed->verticalScrollBar();
    if (sb->maximum() <= 0) return;
    m_preview->scrollToFraction(double(sb->value()) / sb->maximum());
}

void MainWindow::jumpToHeading(int index)
{
    MarkdownEditor *ed = currentEditor();
    if (!ed || index < 0 || index >= m_items.size()) return;
    ed->gotoLine(m_items[index].line);
    if (m_preview->isVisible()) m_preview->scrollToHeading(index);
}

void MainWindow::refreshCursorStatus()
{
    MarkdownEditor *ed = currentEditor();
    if (!ed) { m_posLabel->clear(); m_eolLabel->clear(); return; }
    const QTextCursor c = ed->textCursor();
    m_posLabel->setText(tr("Ln %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
    m_eolLabel->setText(ed->crlf ? "CRLF" : "LF");
}

void MainWindow::refreshCounts()
{
    MarkdownEditor *ed = currentEditor();
    if (!ed) { m_countLabel->clear(); return; }
    const QString t = ed->toPlainText();
    const int words = t.split(QRegularExpression(R"(\s+)"), Qt::SkipEmptyParts).size();
    m_countLabel->setText(tr("%1 words, %2 chars").arg(words).arg(t.size()));
}

// =============================================================================
//  Overlays: command palette, quick open, go to heading
// =============================================================================
void MainWindow::showCommandPalette()
{
    struct Entry { QString label; QAction *act; };
    QVector<Entry> entries;
    for (QAction *a : findChildren<QAction *>()) {
        if (a->isSeparator() || a->menu() || a->text().isEmpty() || !a->isEnabled()) continue;
        QString t = a->text(); t.remove('&');
        if (t == tr("(none)")) continue;
        if (!a->shortcut().isEmpty()) t += "      " + a->shortcut().toString(QKeySequence::NativeText);
        entries.append({t, a});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.label < b.label; });
    QStringList labels;
    for (const Entry &e : entries) labels << e.label;

    QuickPick dlg(tr("Type a command..."), labels, this);
    if (dlg.exec() == QDialog::Accepted && dlg.selectedIndex() >= 0)
        entries[dlg.selectedIndex()].act->trigger();
}

void MainWindow::showQuickOpen()
{
    if (m_folder.isEmpty()) {
        statusBar()->showMessage(tr("Open a folder first (File > Open Folder)"), 4000);
        return;
    }
    QStringList rel;
    QDirIterator it(m_folder, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir root(m_folder);
    while (it.hasNext() && rel.size() < 20000) {
        const QString p = root.relativeFilePath(it.next());
        if (p.startsWith('.') || p.contains("/.") || p.contains("node_modules/") || p.startsWith("build/")) continue;
        rel << p;
    }
    rel.sort(Qt::CaseInsensitive);

    QuickPick dlg(tr("Go to file..."), rel, this);
    if (dlg.exec() == QDialog::Accepted && dlg.selectedIndex() >= 0)
        openFile(root.absoluteFilePath(rel[dlg.selectedIndex()]));
}

void MainWindow::showGoToHeading()
{
    if (m_items.isEmpty()) { statusBar()->showMessage(tr("No headings in this document"), 3000); return; }
    QStringList labels;
    for (const OutlineItem &i : m_items)
        labels << QString(i.level * 2 - 2, ' ') + QString("H%1  ").arg(i.level) + i.title;

    QuickPick dlg(tr("Go to heading..."), labels, this);
    if (dlg.exec() == QDialog::Accepted) jumpToHeading(dlg.selectedIndex());
}

// =============================================================================
//  Explorer context menu
// =============================================================================
void MainWindow::explorerMenu(const QPoint &pos)
{
    if (m_folder.isEmpty()) return;
    const QModelIndex idx = m_tree->indexAt(pos);
    const QString path = idx.isValid() ? m_fs->filePath(idx) : m_folder;
    const QString dir = (idx.isValid() && !m_fs->isDir(idx)) ? QFileInfo(path).absolutePath() : path;

    QMenu menu;
    menu.addAction(tr("New File..."), this, [this, dir] { newFileIn(dir); });
    menu.addAction(tr("New Folder..."), this, [this, dir] { newFolderIn(dir); });

    if (idx.isValid()) {
        menu.addSeparator();
        menu.addAction(tr("Rename..."), this, [this, path] {
            const QFileInfo fi(path);
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, fi.fileName(), &ok);
            if (!ok || name.isEmpty() || name == fi.fileName()) return;
            const QString np = fi.absolutePath() + "/" + name;
            if (!QFile::rename(path, np)) { QMessageBox::warning(this, tr("Rename"), tr("Could not rename.")); return; }
            if (MarkdownEditor *ed = findEditor(path); ed) { ed->filePath = np; updateTabTitle(ed); updateWindowTitle(); }
        });
        menu.addAction(tr("Delete (move to Trash)"), this, [this, path] {
            if (QMessageBox::question(this, tr("Delete"), tr("Move \"%1\" to the Trash?").arg(QFileInfo(path).fileName())) != QMessageBox::Yes) return;
            if (!QFile::moveToTrash(path)) { QMessageBox::warning(this, tr("Delete"), tr("Could not move to Trash.")); return; }
            if (MarkdownEditor *ed = findEditor(path)) { ed->document()->setModified(false); closeTab(m_tabs->indexOf(ed)); }
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    menu.addAction(tr("Copy Relative Path"), this, [this, path] {
        QApplication::clipboard()->setText(QDir(m_folder).relativeFilePath(path)); });
    menu.addAction(tr("Reveal in File Manager"), this, [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}


// =============================================================================
//  Explorer actions (shared by the header buttons and the context menu)
// =============================================================================

// Like VS Code: new items go into the selected folder (or the folder that
// contains the selected file); with nothing selected they go to the project root.
QString MainWindow::explorerTargetDir() const
{
    if (m_folder.isEmpty()) return QString();
    const QModelIndex i = m_tree->currentIndex();
    if (!i.isValid()) return m_folder;
    const QString p = m_fs->filePath(i);
    const QString dir = m_fs->isDir(i) ? p : QFileInfo(p).absolutePath();
    return dir.startsWith(m_folder) ? dir : m_folder;
}

bool MainWindow::ensureProjectFolder()
{
    if (!m_folder.isEmpty()) return true;
    const QString d = QFileDialog::getExistingDirectory(this, tr("Open a project folder first"), QDir::homePath());
    if (d.isEmpty()) return false;
    openFolder(d);
    return true;
}

namespace {
// Validates a relative path typed by the user ("notes/todo.md" is allowed).
bool validRelativeName(const QString &name, QString *why)
{
    static const QString bad = "<>:\"|?*";
    const QStringList parts = name.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty()) { *why = QObject::tr("The name is empty."); return false; }
    for (const QString &part : parts) {
        if (part == "." || part == "..") { *why = QObject::tr("\".\" and \"..\" are not valid names."); return false; }
        for (const QChar &ch : part)
            if (bad.contains(ch) || ch.unicode() < 32) {
                *why = QObject::tr("The name contains an invalid character: %1").arg(ch);
                return false;
            }
        if (part.endsWith('.') || part.endsWith(' ')) { *why = QObject::tr("Names cannot end with a dot or a space."); return false; }
    }
    return true;
}
} // namespace

void MainWindow::newFileIn(const QString &dirArg)
{
    if (!ensureProjectFolder()) return;
    const QString dir = dirArg.isEmpty() || !dirArg.startsWith(m_folder) ? m_folder : dirArg;

    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New File"),
        tr("File name (in %1):").arg(QDir(m_folder).relativeFilePath(dir) == "." ? QFileInfo(m_folder).fileName()
                                                                              : QDir(m_folder).relativeFilePath(dir)),
        QLineEdit::Normal, "untitled.md", &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) return;
    QString why;
    if (!validRelativeName(name, &why)) { QMessageBox::warning(this, tr("New File"), why); return; }
    if (!QFileInfo(name).fileName().contains('.')) name += ".md";   // bare names become Markdown files

    const QString path = QDir(dir).filePath(name);
    if (QFileInfo::exists(path)) { QMessageBox::warning(this, tr("New File"), tr("\"%1\" already exists.").arg(name)); return; }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { QMessageBox::warning(this, tr("New File"), tr("Could not create the folder.")); return; }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, tr("New File"), f.errorString()); return; }
    f.close();

    // Reveal it in the tree, then open it in the editor.
    QTimer::singleShot(150, this, [this, path] {
        const QModelIndex idx = m_fs->index(path);
        if (idx.isValid()) { m_tree->scrollTo(idx); m_tree->setCurrentIndex(idx); }
    });
    openFile(path);
}

void MainWindow::newFolderIn(const QString &dirArg)
{
    if (!ensureProjectFolder()) return;
    const QString dir = dirArg.isEmpty() || !dirArg.startsWith(m_folder) ? m_folder : dirArg;

    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Folder"),
        tr("Folder name (in %1):").arg(QDir(m_folder).relativeFilePath(dir) == "." ? QFileInfo(m_folder).fileName()
                                                                                : QDir(m_folder).relativeFilePath(dir)),
        QLineEdit::Normal, QString(), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) return;
    QString why;
    if (!validRelativeName(name, &why)) { QMessageBox::warning(this, tr("New Folder"), why); return; }

    const QString path = QDir(dir).filePath(name);
    if (QFileInfo::exists(path)) { QMessageBox::warning(this, tr("New Folder"), tr("\"%1\" already exists.").arg(name)); return; }
    if (!QDir().mkpath(path))    { QMessageBox::warning(this, tr("New Folder"), tr("Could not create the folder.")); return; }

    QTimer::singleShot(150, this, [this, path, dir] {
        m_tree->expand(m_fs->index(dir));
        const QModelIndex idx = m_fs->index(path);
        if (idx.isValid()) { m_tree->scrollTo(idx); m_tree->setCurrentIndex(idx); }
    });
    statusBar()->showMessage(tr("Created folder %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::refreshExplorer()
{
    if (m_folder.isEmpty()) return;
    m_fs->setRootPath(QString());          // force the model to rescan the disk
    m_fs->setRootPath(m_folder);
    m_tree->setRootIndex(m_fs->index(m_folder));
}

void MainWindow::refreshExplorerIcons()
{
    const QColor c = m_theme.text;
    if (m_btnNewFile)   m_btnNewFile->setIcon(explorerIcon(0, c));
    if (m_btnNewFolder) m_btnNewFolder->setIcon(explorerIcon(1, c));
    if (m_btnRefresh)   m_btnRefresh->setIcon(explorerIcon(2, c));
    if (m_btnCollapse)  m_btnCollapse->setIcon(explorerIcon(3, c));
}

// =============================================================================
//  Export
// =============================================================================
void MainWindow::buildExportMenu()
{
    m_exportMenu->clear();
    const bool have = currentEditor() != nullptr;
    struct B { QString label, kind; };
    for (const B &b : {B{tr("HTML (.html)"), "html"}, B{tr("PDF (.pdf)"), "pdf"},
                       B{tr("Word document (.docx)"), "docx"}, B{tr("OpenDocument text (.odt)"), "odt"},
                       B{tr("Plain text (.txt)"), "txt"}}) {
        QAction *a = m_exportMenu->addAction(b.label, this, [this, k = b.kind] { exportBuiltIn(k); });
        a->setEnabled(have);
    }
    m_exportMenu->addSeparator();
    QMenu *pandoc = m_exportMenu->addMenu(tr("More formats via Pandoc"));
    if (Exporter::findPandoc(AppSettings::instance()).isEmpty()) {
        pandoc->addAction(tr("Pandoc not found - set path in Settings > Export"))->setEnabled(false);
    } else {
        for (const auto &f : Exporter::pandocFormats()) {
            QAction *a = pandoc->addAction(f.label, this, [this, f] { exportPandoc(f); });
            a->setEnabled(have);
        }
    }
}

QString MainWindow::exportBaseDir(MarkdownEditor *ed) const
{
    if (!ed->filePath.isEmpty()) return QFileInfo(ed->filePath).absolutePath();
    return m_folder.isEmpty() ? QDir::homePath() : m_folder;
}

QString MainWindow::exportSuggestedPath(MarkdownEditor *ed, const QString &ext) const
{
    const QString base = ed->filePath.isEmpty() ? QString("document") : QFileInfo(ed->filePath).completeBaseName();
    return exportBaseDir(ed) + "/" + base + "." + ext;
}

void MainWindow::announceExport(const QString &path)
{
    statusBar()->showMessage(tr("Exported to %1").arg(QDir::toNativeSeparators(path)), 6000);
    QMessageBox box(QMessageBox::Information, tr("Export complete"), tr("Saved to:\n%1").arg(QDir::toNativeSeparators(path)),
                    QMessageBox::Ok, this);
    QPushButton *open = box.addButton(tr("Open"), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::exportBuiltIn(const QString &kind)
{
    MarkdownEditor *ed = currentEditor();
    if (!ed) return;
    const AppSettings &S = AppSettings::instance();

    struct Info { QString title, filter; };
    const QMap<QString, Info> info = {
        {"html", {tr("Export HTML"), tr("HTML (*.html)")}},
        {"pdf",  {tr("Export PDF"),  tr("PDF (*.pdf)")}},
        {"docx", {tr("Export Word"), tr("Word document (*.docx)")}},
        {"odt",  {tr("Export ODT"),  tr("OpenDocument text (*.odt)")}},
        {"txt",  {tr("Export text"), tr("Text (*.txt)")}},
    };
    QString out = QFileDialog::getSaveFileName(this, info[kind].title, exportSuggestedPath(ed, kind), info[kind].filter);
    if (out.isEmpty()) return;
    if (QFileInfo(out).suffix().isEmpty()) out += "." + kind;

    const QString md   = ed->toPlainText();
    const QString dir  = exportBaseDir(ed);
    const QString name = ed->filePath.isEmpty() ? tr("Document") : QFileInfo(ed->filePath).completeBaseName();
    QString err; bool ok = false;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    if      (kind == "html") ok = Exporter::toHtml(md, name, dir, out, &err);
    else if (kind == "pdf")  ok = Exporter::toPdf(md, dir, S, out, &err);
    else if (kind == "docx") ok = Exporter::toDocx(md, dir, S, out, &err);
    else if (kind == "odt")  ok = Exporter::toOdt(md, dir, out, &err);
    else                     ok = Exporter::toText(md, dir, out, &err);
    QApplication::restoreOverrideCursor();

    if (ok) announceExport(out);
    else    QMessageBox::warning(this, tr("Export failed"), err);
}

void MainWindow::exportPandoc(const Exporter::PandocFormat &fmt)
{
    MarkdownEditor *ed = currentEditor();
    if (!ed) return;
    QString out = QFileDialog::getSaveFileName(this, tr("Export with Pandoc"), exportSuggestedPath(ed, fmt.extension),
                                               QString("%1 (*.%2)").arg(fmt.label, fmt.extension));
    if (out.isEmpty()) return;
    if (QFileInfo(out).suffix().isEmpty()) out += "." + fmt.extension;

    QString err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = Exporter::viaPandoc(ed->toPlainText(),
                        ed->filePath.isEmpty() ? tr("Document") : QFileInfo(ed->filePath).completeBaseName(),
                        exportBaseDir(ed), AppSettings::instance(), fmt, out, &err);
    QApplication::restoreOverrideCursor();
    if (ok) announceExport(out);
    else    QMessageBox::warning(this, tr("Pandoc export failed"), err);
}

// =============================================================================
//  Window events
// =============================================================================
void MainWindow::closeEvent(QCloseEvent *e)
{
    QStringList files;
    for (int i = 0; i < m_tabs->count(); ++i) {
        MarkdownEditor *ed = editorAt(i);
        if (!ed) continue;
        if (!maybeSave(ed)) { e->ignore(); return; }
        if (!ed->filePath.isEmpty()) files << ed->filePath;
    }
    QSettings q;
    q.setValue("window/geometry", saveGeometry());
    q.setValue("window/state", saveState());
    q.setValue("session/folder", m_folder);
    q.setValue("session/files", files);
    AppSettings::instance().save();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    for (const QUrl &u : e->mimeData()->urls()) {
        if (!u.isLocalFile()) continue;
        const QFileInfo fi(u.toLocalFile());
        if (fi.isDir()) openFolder(fi.absoluteFilePath());
        else            openFile(fi.absoluteFilePath());
    }
}
