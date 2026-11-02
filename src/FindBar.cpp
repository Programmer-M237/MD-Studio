#include "FindBar.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

FindBar::FindBar(QWidget *parent) : QWidget(parent)
{
    auto mkBtn = [](const QString &text, const QString &tip, bool checkable = false) {
        auto *b = new QToolButton;
        b->setText(text); b->setToolTip(tip); b->setCheckable(checkable);
        b->setAutoRaise(true);
        return b;
    };
    m_find    = new QLineEdit;  m_find->setPlaceholderText(tr("Find"));
    m_replace = new QLineEdit;  m_replace->setPlaceholderText(tr("Replace"));
    m_case    = mkBtn("Aa", tr("Match case (case-sensitive)"), true);
    m_regex   = mkBtn(".*", tr("Use regular expression"), true);
    m_prev    = mkBtn("\u2191", tr("Previous match (Shift+Enter)"));
    m_next    = mkBtn("\u2193", tr("Next match (Enter)"));
    m_close   = mkBtn("\u2715", tr("Close (Esc)"));
    m_replOne = mkBtn(tr("Replace"), tr("Replace this match"));
    m_replAll = mkBtn(tr("All"), tr("Replace all matches"));
    m_status  = new QLabel;

    auto *row1 = new QHBoxLayout;
    row1->addWidget(m_find, 1);
    row1->addWidget(m_case); row1->addWidget(m_regex);
    row1->addWidget(m_prev); row1->addWidget(m_next);
    row1->addWidget(m_status);
    row1->addWidget(m_close);

    m_replaceRow = new QWidget;
    auto *row2 = new QHBoxLayout(m_replaceRow);
    row2->setContentsMargins(0, 0, 0, 0);
    row2->addWidget(m_replace, 1);
    row2->addWidget(m_replOne); row2->addWidget(m_replAll);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 4, 8, 4);
    lay->setSpacing(3);
    lay->addLayout(row1);
    lay->addWidget(m_replaceRow);

    connect(m_next,  &QToolButton::clicked, this, [this] { find(true); });
    connect(m_prev,  &QToolButton::clicked, this, [this] { find(false); });
    connect(m_close, &QToolButton::clicked, this, [this] { hide(); if (m_editor) m_editor->setFocus(); });
    connect(m_replOne, &QToolButton::clicked, this, &FindBar::replaceOne);
    connect(m_replAll, &QToolButton::clicked, this, &FindBar::replaceAll);
    connect(m_find, &QLineEdit::returnPressed, this, [this] {
        find(!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier));
    });
    connect(m_replace, &QLineEdit::returnPressed, this, &FindBar::replaceOne);
    connect(m_find, &QLineEdit::textChanged, this, [this](const QString &) { setStatus(QString()); });
    hide();
}

void FindBar::openFind(const QString &seed)
{
    m_replaceRow->hide();
    if (!seed.isEmpty()) m_find->setText(seed);
    show(); m_find->setFocus(); m_find->selectAll();
}

void FindBar::openReplace(const QString &seed)
{
    m_replaceRow->show();
    if (!seed.isEmpty()) m_find->setText(seed);
    show(); m_find->setFocus(); m_find->selectAll();
}

void FindBar::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) { hide(); if (m_editor) m_editor->setFocus(); return; }
    QWidget::keyPressEvent(e);
}

void FindBar::setStatus(const QString &s) { m_status->setText(s); }

QRegularExpression FindBar::regex() const
{
    return QRegularExpression(m_find->text(),
        m_case->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
}

bool FindBar::find(bool forward)
{
    if (!m_editor || m_find->text().isEmpty()) return false;
    QTextDocument *doc = m_editor->document();

    QTextDocument::FindFlags fl;
    if (!forward)            fl |= QTextDocument::FindBackward;
    if (m_case->isChecked()) fl |= QTextDocument::FindCaseSensitively;

    QRegularExpression re;
    if (m_regex->isChecked()) {
        re = regex();
        if (!re.isValid()) { setStatus(tr("Invalid regex")); return false; }
    }
    auto run = [&](const QTextCursor &from) {
        return m_regex->isChecked() ? doc->find(re, from, fl) : doc->find(m_find->text(), from, fl);
    };

    QTextCursor c = run(m_editor->textCursor());
    if (c.isNull()) {                                   // wrap around
        QTextCursor start(doc);
        if (!forward) start.movePosition(QTextCursor::End);
        c = run(start);
    }
    if (c.isNull()) { setStatus(tr("No results")); return false; }
    setStatus(QString());
    m_editor->setTextCursor(c);
    m_editor->centerCursor();
    return true;
}

void FindBar::replaceOne()
{
    if (!m_editor) return;
    QTextCursor c = m_editor->textCursor();
    if (c.hasSelection()) {
        const QString sel = c.selectedText();
        bool matches;
        if (m_regex->isChecked()) {
            const auto m = regex().match(sel);
            matches = m.hasMatch() && m.capturedLength() == sel.size();
        } else {
            matches = sel.compare(m_find->text(), m_case->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0;
        }
        if (matches) {
            QString r = m_replace->text();
            if (m_regex->isChecked()) { QString s = sel; s.replace(regex(), r); r = s; }
            c.insertText(r);
        }
    }
    find(true);
}

void FindBar::replaceAll()
{
    if (!m_editor || m_find->text().isEmpty()) return;
    QTextDocument *doc = m_editor->document();
    QTextDocument::FindFlags fl;
    if (m_case->isChecked()) fl |= QTextDocument::FindCaseSensitively;
    QRegularExpression re;
    if (m_regex->isChecked()) {
        re = regex();
        if (!re.isValid()) { setStatus(tr("Invalid regex")); return; }
    }

    int n = 0;
    QTextCursor c(doc);
    c.beginEditBlock();
    while (true) {
        QTextCursor f = m_regex->isChecked() ? doc->find(re, c, fl) : doc->find(m_find->text(), c, fl);
        if (f.isNull() || f.selectedText().isEmpty()) break;
        QString r = m_replace->text();
        if (m_regex->isChecked()) { QString s = f.selectedText(); s.replace(re, r); r = s; }
        f.insertText(r);
        c = f;
        ++n;
    }
    c.endEditBlock();
    setStatus(tr("%n replaced", "", n));
}
