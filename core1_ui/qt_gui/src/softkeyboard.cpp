#include "softkeyboard.h"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

SoftKeyboard::SoftKeyboard(QWidget *parent, const QString &title,
                           const QString &initial, bool password)
    : QDialog(parent), m_password(password)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel *cap = new QLabel(title, this);
    cap->setStyleSheet(QStringLiteral("QLabel{color:#eceff1;font-size:20px;}"));
    root->addWidget(cap);

    m_edit = new QLineEdit(this);
    m_edit->setText(initial);
    m_edit->setEchoMode(password ? QLineEdit::Password : QLineEdit::Normal);
    m_edit->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#101418;color:#eceff1;font-size:20px;"
        "border:2px solid #37474f;border-radius:6px;padding:6px;}"));
    m_edit->setMinimumHeight(48);
    root->addWidget(m_edit);

    QGridLayout *grid = new QGridLayout();
    grid->setSpacing(6);
    root->addLayout(grid);

    const char *digits = "1234567890";
    for (int i = 0; i < 10; ++i)
        addKey(grid, 0, i, QString(QLatin1Char(digits[i])),
               QString(QLatin1Char(digits[i])));

    const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    for (int r = 0; r < 3; ++r) {
        const QString row = QString::fromLatin1(rows[r]);
        for (int i = 0; i < row.size(); ++i) {
            /* letters are inserted in the current case by applyCase() */
            const QString lower = QString(row.at(i));
            addKey(grid, r + 1, i, lower.toUpper(), lower);
        }
    }

    const char *syms = ".-_:@/+";
    for (int i = 0; i < 7; ++i)
        addKey(grid, 4, i, QString(QLatin1Char(syms[i])),
               QString(QLatin1Char(syms[i])));

    /* control row */
    m_shift = new QPushButton(QStringLiteral("SHIFT"), this);
    m_shift->setCheckable(true);
    m_shift->setMinimumSize(120, 52);
    connect(m_shift, &QPushButton::toggled, this, [this](bool on) {
        m_upper = on;
        applyCase();
    });
    grid->addWidget(m_shift, 5, 0, 1, 2);

    QPushButton *space = new QPushButton(QStringLiteral("SPACE"), this);
    space->setMinimumSize(160, 52);
    connect(space, &QPushButton::clicked, this, [this]() {
        m_edit->insert(QStringLiteral(" "));
    });
    grid->addWidget(space, 5, 2, 1, 3);

    QPushButton *del = new QPushButton(QStringLiteral("DEL"), this);
    del->setMinimumSize(110, 52);
    connect(del, &QPushButton::clicked, this, [this]() {
        QString t = m_edit->text();
        if (!t.isEmpty()) {
            t.chop(1);
            m_edit->setText(t);
        }
    });
    grid->addWidget(del, 5, 5, 1, 2);

    QPushButton *clear = new QPushButton(QStringLiteral("CLEAR"), this);
    clear->setMinimumSize(110, 52);
    connect(clear, &QPushButton::clicked, this, [this]() { m_edit->clear(); });
    grid->addWidget(clear, 5, 7, 1, 1);

    if (password) {
        m_show = new QPushButton(QStringLiteral("SHOW"), this);
        m_show->setCheckable(true);
        m_show->setMinimumSize(110, 52);
        connect(m_show, &QPushButton::toggled, this, [this](bool on) {
            m_edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        });
        grid->addWidget(m_show, 5, 8, 1, 1);
    }

    QPushButton *cancel = new QPushButton(QStringLiteral("CANCEL"), this);
    cancel->setMinimumSize(140, 52);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    grid->addWidget(cancel, 6, 7, 1, 2);

    QPushButton *ok = new QPushButton(QStringLiteral("OK"), this);
    ok->setMinimumSize(160, 56);
    ok->setStyleSheet(QStringLiteral(
        "QPushButton{background:#1b5e20;color:#e8f5e9;font-size:20px;"
        "border:1px solid #2e7d32;border-radius:6px;}"
        "QPushButton:pressed{background:#2e7d32;}"));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    grid->addWidget(ok, 6, 0, 1, 4);

    QLabel *hint = new QLabel(
        QStringLiteral("touch input / ASCII only - the panel has no keyboard"),
        this);
    hint->setStyleSheet(QStringLiteral("QLabel{color:#78909c;font-size:14px;}"));
    grid->addWidget(hint, 6, 4, 1, 3);

    setStyleSheet(QStringLiteral(
        "QDialog{background:#1c232b;}"
        "QPushButton{background:#263238;color:#eceff1;font-size:20px;"
        "border:1px solid #37474f;border-radius:6px;}"
        "QPushButton:pressed{background:#37474f;}"
        "QPushButton:checked{background:#0d47a1;}"));

    setMinimumSize(1000, 500);
    adjustSize();
    QRect scr;
    if (QApplication::primaryScreen() != nullptr)
        scr = QApplication::primaryScreen()->geometry();
    if (!scr.isEmpty())
        move(scr.x() + (scr.width() - width()) / 2,
             scr.y() + (scr.height() - height()) / 2);
    applyCase();
}

void SoftKeyboard::addKey(QGridLayout *grid, int row, int col,
                          const QString &label, const QString &insert)
{
    QPushButton *b = new QPushButton(label, this);
    b->setMinimumSize(88, 52);
    b->setProperty("insertText", insert);
    connect(b, &QPushButton::clicked, this, [this, b]() {
        QString t = b->property("insertText").toString();
        if (m_upper && t.size() == 1 && t.at(0).isLetter())
            t = t.toUpper();
        m_edit->insert(t);
    });
    grid->addWidget(b, row, col);
}

/* Letters are shown/inserted upper-case while SHIFT is active. */
void SoftKeyboard::applyCase()
{
    const QList<QPushButton *> keys = findChildren<QPushButton *>();
    for (int i = 0; i < keys.size(); ++i) {
        QPushButton *b = keys.at(i);
        const QString ins = b->property("insertText").toString();
        if (ins.size() != 1 || !ins.at(0).isLetter())
            continue;
        b->setText(m_upper ? ins.toUpper() : ins.toLower());
    }
}

QString SoftKeyboard::getText(QWidget *parent, const QString &title,
                              const QString &initial, bool password)
{
    SoftKeyboard kb(parent, title, initial, password);
    if (kb.exec() == QDialog::Accepted)
        return kb.m_edit->text();
    return QString();
}
