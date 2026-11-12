#pragma once
#include "core/Config.h"
#include "core/MailActions.h"
#include "core/ProcessRunner.h"
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <chrono>
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
//   stderr) e' SEMPRE un risultato vuoto; l'eventuale stderr va in un qWarning
//   (non fatale). ProcessRunner non permette di impostare LC_ALL=C: nessun
//   confronto di testi localizzati. mu e' tollerante con le query malformate: spesso le tratta come
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
    // Radice Maildir di mu: serve a escludere Cestino, Bozze e Inviata di ogni
    // account dalla vista "Non lette". Vuota = nessuna esclusione per cartella.
    void setMuRoot(const QString& r) { m_muRoot = r; }
    QString muRoot() const { return m_muRoot; }
    // Query della vista "Non lette" (con le esclusioni per cartella).
    QString unreadQuery() const;
    // Query di una cartella `maildir:"/<rel>"`, relativa alla radice di mu.
    // Vuota se la Maildir dell'account non e' sotto la radice o la cartella e' vuota.
    static QString maildirQuery(const QString& muRoot, const QString& maildirRoot, const QString& folder);

    void search(const QString& query, int offset, int limit, SearchCallback cb);
    void unread(int offset, int limit, SearchCallback cb);
    // Numero di messaggi non letti (non cestinati) che soddisfano `query` (vuota = la vista
    // "Non lette"). Asincrono; cb(-1, errore) se mu fallisce.
    void countUnread(const QString& query, std::function<void(int count, QString error)> cb);
    // Percorsi dei messaggi non letti che soddisfano `query` (cestino compreso).
    void unreadPaths(const QString& query, std::function<void(QStringList paths, QString error)> cb);
    void reindex(const QString& accountId, Callback cb);
    // Radice Maildir di mu (`mu info store`, riga "maildir"). Le cartelle di mu
    // ("/a/INBOX") sono relative a questa radice, non a quella di un account.
    void storeRoot(std::function<void(QString root, QString error)> cb);
    static QString parseStoreRoot(const QByteArray& infoStoreOutput);

    // Esposto per i test.
    QList<MessageSummary> parse(const QByteArray& json, QString* error) const;

private:
    QStringList baseArgs(const QString& sub) const;
    QString accountFor(const QString& path) const;
    QString m_mu, m_muHome, m_muRoot;
    QList<Account> m_accounts;
    void reindexAttempt(Callback cb, std::chrono::milliseconds waited, std::chrono::milliseconds delay);
    ProcessRunner m_runner;
    // Ultima ricerca letta da mu: serve le pagine successive senza rilanciare mu.
    static constexpr int kCacheChunk = 1000;
    QList<MessageSummary> m_cache;
    QString m_cacheQuery;
    bool m_cacheValid = false, m_cacheComplete = false;
};

} // namespace boma
