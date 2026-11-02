#pragma once
// =============================================================================
//  FindBar.h - inline Find / Replace strip (Ctrl+F / Ctrl+H), regex capable.
// =============================================================================
#include <QWidget>
#include <QPointer>

class QPlainTextEdit;
class QLineEdit;
class QToolButton;
class QLabel;

class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget *parent = nullptr);
    void setEditor(QPlainTextEdit *ed) { m_editor = ed; }
    void openFind(const QString &seed = QString());
    void openReplace(const QString &seed = QString());
    bool find(bool forward);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void replaceOne();
    void replaceAll();
    void setStatus(const QString &s);
    QRegularExpression regex() const;

    QPointer<QPlainTextEdit> m_editor;
    QLineEdit   *m_find, *m_replace;
    QToolButton *m_case, *m_regex, *m_prev, *m_next, *m_close, *m_replOne, *m_replAll;
    QWidget     *m_replaceRow;
    QLabel      *m_status;
};
