#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace boma {

// Segnala l'arrivo di posta nuova scritta da altri programmi (mbsync lanciato dal cron o da
// goimapnotify): osserva le cartelle "new" dei Maildir sotto le radici indicate. Solo directory,
// mai i file dei messaggi. Le raffiche di eventi sono raccolte in un solo segnale newMail().
class MailWatcher : public QObject {
    Q_OBJECT
public:
    explicit MailWatcher(QObject* parent = nullptr);
    void setRoots(const QStringList& roots); // sostituisce le radici e rilegge l'albero
    void setDebounce(int ms) { m_timer.setInterval(ms); }
    int watchedCount() const { return m_fsw.directories().size(); }
    static QStringList findNewDirs(const QString& root);
signals:
    void newMail();

private:
    void rescan();
    QFileSystemWatcher m_fsw;
    QTimer m_timer;
    QStringList m_roots;
};

} // namespace boma
