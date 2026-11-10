#pragma once
#include "core/Config.h"
#include "core/MailActions.h"
#include "core/ProcessRunner.h"
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

namespace boma {

struct MessageSummary {
    QString path, messageId, from, subject;
    QDateTime date;
    bool unread = false, hasAttachment = false;
    QString accountId;
};

using SearchCallback = std::function<void(QList<MessageSummary>, QString error)>;

// Indice di ricerca basato su `mu` (per messaggio, senza conversazioni; mu
// find --skip-dups). Tutte le operazioni sono asincrone. Risultati ordinati
// per data, dal piu' recente.
// - mu find non ha un offset: per la pagina [offset, offset+limit) si chiede
//   --maxnum=offset+limit e si scartano i primi "offset" risultati. Il costo
//   cresce linearmente con la profondita' della pagina (tutte le righe sono
//   rianalizzate sul thread della GUI): misurato 493 ms a offset 5000 su 50k
//   messaggi. Limite noto della v1; MessageListModel limita la finestra
//   (kMaxWindow) e segnala truncated(). Se l'indice cambia tra due pagine gli
//   offset si spostano: azzerare il modello dopo reindex/azioni.
// - "nessuna corrispondenza" (mu esce con codice 2 e scrive "no matches" su
//   stderr) e' un risultato vuoto; codice 2 con altro stderr e' un errore. mu e' tollerante con le query malformate: spesso le tratta come
//   ricerca senza risultati.
// - mu index lavora sull'intera radice configurata: accountId in reindex()
//   serve solo al chiamante.
class MailIndex : public QObject {
    Q_OBJECT
public:
    explicit MailIndex(QObject* parent = nullptr, QString muProgram = QStringLiteral("mu"),
                       QString muHome = QString())
        : QObject(parent), m_mu(std::move(muProgram)), m_muHome(std::move(muHome)) {}
    void setMuProgram(const QString& p) { m_mu = p; }
    void setMuHome(const QString& h) { m_muHome = h; }
    // Usati per ricavare accountId dal percorso del messaggio.
    void setAccounts(const QList<Account>& a) { m_accounts = a; }

    void search(const QString& query, int offset, int limit, SearchCallback cb);
    void unread(int offset, int limit, SearchCallback cb);
    void reindex(const QString& accountId, Callback cb);

    // Esposto per i test.
    QList<MessageSummary> parse(const QByteArray& json, QString* error) const;

private:
    QStringList baseArgs(const QString& sub) const;
    QString accountFor(const QString& path) const;
    QString m_mu, m_muHome;
    QList<Account> m_accounts;
    ProcessRunner m_runner;
};

} // namespace boma
