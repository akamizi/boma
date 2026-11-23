#pragma once
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace boma {

// "Posta in arrivo" virtuale degli account Gmail che sincronizzano solo «Tutti i messaggi»:
// per ogni account l'insieme dei Message-ID (senza <>) che sul server hanno l'etichetta \Inbox.
// File JSON locale di Boma {"version":1,"inbox":{"<idAccount>":["<message-id>",...]}}: nessun
// messaggio e nessun server viene toccato. A differenza delle etichette (TagStore, che si
// accumulano) l'insieme viene SOSTITUITO a ogni importazione, perche' un messaggio archiviato
// esce dalla Posta in arrivo. Scrittura atomica; un file illeggibile e' copiato in ".corrupt".
class InboxStore : public QObject {
    Q_OBJECT
public:
    // path vuoto = <AppDataLocation>/gmail-inbox.json
    explicit InboxStore(QString path = QString(), QObject* parent = nullptr);
    QString path() const { return path_; }
    QString loadError() const { return loadError_; }
    QString lastError() const { return lastError_; }

    // Account con un'importazione gia' fatta (anche se la Posta in arrivo e' vuota).
    bool has(const QString& accountId) const { return sets_.contains(accountId); }
    QStringList accounts() const { return sets_.keys(); }
    QStringList messageIds(const QString& accountId) const;
    // Sostituisce l'insieme dell'account. false se il salvataggio fallisce (la memoria resta aggiornata).
    bool replace(const QString& accountId, const QStringList& ids);

signals:
    void changed();

private:
    void load();
    bool save();
    QString path_, loadError_, lastError_;
    QHash<QString, QStringList> sets_;
};

} // namespace boma
