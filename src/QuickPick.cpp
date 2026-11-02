#include "QuickPick.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <algorithm>

QuickPick::QuickPick(const QString &placeholder, const QStringList &items, QWidget *parent)
    : QDialog(parent, Qt::Popup), m_items(items)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_edit = new QLineEdit;
    m_edit->setPlaceholderText(placeholder);
    m_list = new QListWidget;
    m_list->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(m_edit);
    lay->addWidget(m_list);

    resize(620, 360);
    if (parent) {
        const QPoint top = parent->mapToGlobal(QPoint(0, 0));
        move(top.x() + (parent->width() - width()) / 2, top.y() + 50);
    }

    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, &QuickPick::filter);
    connect(m_list, &QListWidget::itemActivated, this, [this] { choose(); });
    connect(m_list, &QListWidget::itemClicked,   this, [this] { choose(); });
    filter(QString());
    m_edit->setFocus();
}

void QuickPick::filter(const QString &query)
{
    // Score: substring match beats a scattered (subsequence) match.
    struct Hit { int score; int index; };
    QVector<Hit> hits;
    const QString q = query.trimmed().toLower();

    for (int i = 0; i < m_items.size(); ++i) {
        if (q.isEmpty()) { hits.append({0, i}); continue; }
        const QString s = m_items[i].toLower();
        const int pos = s.indexOf(q);
        if (pos >= 0) { hits.append({pos, i}); continue; }
        int qi = 0;
        for (int k = 0; k < s.size() && qi < q.size(); ++k)
            if (s[k] == q[qi]) ++qi;
        if (qi == q.size()) hits.append({1000, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score < b.score; });

    m_list->clear();
    for (int n = 0; n < hits.size() && n < 300; ++n) {
        auto *it = new QListWidgetItem(m_items[hits[n].index]);
        it->setData(Qt::UserRole, hits[n].index);
        m_list->addItem(it);
    }
    if (m_list->count()) m_list->setCurrentRow(0);
}

void QuickPick::choose()
{
    if (auto *it = m_list->currentItem()) {
        m_result = it->data(Qt::UserRole).toInt();
        accept();
    }
}

bool QuickPick::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_edit && ev->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        const int n = m_list->count();
        switch (ke->key()) {
        case Qt::Key_Down: if (n) m_list->setCurrentRow(qMin(n - 1, m_list->currentRow() + 1)); return true;
        case Qt::Key_Up:   if (n) m_list->setCurrentRow(qMax(0, m_list->currentRow() - 1));     return true;
        case Qt::Key_PageDown: if (n) m_list->setCurrentRow(qMin(n - 1, m_list->currentRow() + 8)); return true;
        case Qt::Key_PageUp:   if (n) m_list->setCurrentRow(qMax(0, m_list->currentRow() - 8));     return true;
        case Qt::Key_Return: case Qt::Key_Enter: choose(); return true;
        case Qt::Key_Escape: reject(); return true;
        default: break;
        }
    }
    return QDialog::eventFilter(obj, ev);
}
