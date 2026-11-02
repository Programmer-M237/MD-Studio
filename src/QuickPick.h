#pragma once
// =============================================================================
//  QuickPick.h - VS Code style "type to filter, Enter to pick" popup.
//  Used for: Command Palette (Ctrl+Shift+P), Quick Open (Ctrl+P) and
//  Go to Heading (Ctrl+Shift+O).
// =============================================================================
#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;

class QuickPick : public QDialog {
    Q_OBJECT
public:
    QuickPick(const QString &placeholder, const QStringList &items, QWidget *parent);
    /// Index into the original `items` list, or -1 if cancelled.
    int selectedIndex() const { return m_result; }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void filter(const QString &query);
    void choose();

    QLineEdit   *m_edit;
    QListWidget *m_list;
    QStringList  m_items;
    int          m_result = -1;
};
