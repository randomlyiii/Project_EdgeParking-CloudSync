#ifndef SOFTKEYBOARD_H
#define SOFTKEYBOARD_H
/* SoftKeyboard - on-screen keyboard for the touch panel.
 *
 * The board has no keyboard at all, so every free-text field of the settings
 * page (WiFi SSID/password, cloud API base/model/key/prompt) is edited through
 * this dialog. Keys are large on purpose (1024x600 panel, finger input).
 *
 * Usage:
 *     const QString s = SoftKeyboard::getText(this, title, current, true);
 * The dialog is frameless (linuxfb has no window manager) and centred; an
 * attached "SHOW" toggle switches the password echo mode so a mistyped WiFi
 * password can be seen before committing it.
 */
#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

class SoftKeyboard : public QDialog
{
    Q_OBJECT
public:
    /* Returns the edited text, or a null QString when cancelled. */
    static QString getText(QWidget *parent, const QString &title,
                           const QString &initial = QString(),
                           bool password = false);

private:
    explicit SoftKeyboard(QWidget *parent, const QString &title,
                          const QString &initial, bool password);

    void addKey(class QGridLayout *grid, int row, int col, const QString &label,
                const QString &insert);
    void applyCase();

    QLineEdit *m_edit = nullptr;
    QPushButton *m_shift = nullptr;
    QPushButton *m_show = nullptr;
    bool m_password = false;
    bool m_upper = false;
};

#endif /* SOFTKEYBOARD_H */
