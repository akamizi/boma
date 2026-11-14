#pragma once
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace boma {

// Etichette dei messaggi, salvate in un file JSON locale (Message-ID -> etichette).
// Non tocca mai i messaggi (nessun header scritto) ne' i server: il file e' di Boma.
// - Formato: {"version":1,"tags":{"<message-id senza <>>":["a","b"]}}.
// - Scrittura atomica (QSaveFile) a ogni modifica; un file illeggibile non viene mai
//   sovrascritto: viene copiato in "<file>.corrupt" e si riparte vuoti (loadError()).
// - Nomi: spazi ripuliti, max 40 caratteri, confronto senza distinzione di maiuscole
//   (vince la grafia gia' presente).
class TagStore : public QObject {
    Q_OBJECT
public:
    // path vuoto = <AppDataLocation>/tags.json
    explicit TagStore(QString path = QString(), QObject* parent = nullptr);
    QString path() const { return path_; }
    QString loadError() const { return loadError_; }

    static QString normalize(const QString& tag);

    QStringList tags(const QString& messageId) const;
    // Etichette esistenti con il numero di messaggi, in ordine alfabetico.
    QMap<QString, int> counts() const;
    QStringList messageIds(const QString& tag) const;

    // false se il salvataggio fallisce (lo stato in memoria resta aggiornato).
    bool setTag(const QStringList& messageIds, const QString& tag, bool on);
    bool renameTag(const QString& from, const QString& to);
    bool removeTag(const QString& tag);
    QString lastError() const { return lastError_; }

signals:
    void changed();

private:
    void load();
    bool save();
    QString canonical(const QString& tag) const; // grafia esistente o normalizzata

    QString path_, loadError_, lastError_;
    QHash<QString, QStringList> byId_;
};

} // namespace boma
