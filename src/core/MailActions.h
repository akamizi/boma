#pragma once
#include "core/Config.h"
#include "core/ProcessRunner.h"
#include <QObject>
#include <QString>
#include <functional>

namespace boma {

using Callback = std::function<void(bool ok, QString error)>;

// Note:
// - Lo spostamento nel cestino usa `mu move --change-name --flags=+S`: nome
//   nuovo (senza il ",U=" di mbsync della cartella di origine, che romperebbe la
//   sincronizzazione) e bandiera S (letto; mu altrimenti la toglierebbe).
// - markRead su un messaggio in new/ sposta il file in cur/ e la callback non
//   restituisce il nuovo percorso: chi concatena azioni deve ririsolvere il
//   percorso tramite mu.
// - Si assume che la radice di mu coincida con account.maildirRoot (da
//   verificare all'avvio, in un task successivo); moveToTrash rifiuta comunque
//   i messaggi fuori da maildirRoot. I percorsi devono essere assoluti.
// Azioni sui messaggi esistenti: SEMPRE tramite `mu move`. Boma non rinomina,
// non cancella e non tocca mai direttamente i file della Maildir.
class MailActions : public QObject {
    Q_OBJECT
public:
    explicit MailActions(QObject* parent = nullptr, QString muProgram = QStringLiteral("mu"))
        : QObject(parent), m_mu(std::move(muProgram)) {}
    void setMuProgram(const QString& p) { m_mu = p; }
    // Se non vuota, passata a mu come --muhome (stessa base dati di MailIndex).
    void setMuHome(const QString& h) { m_muHome = h; }
    // Radice Maildir di mu: le cartelle di mu sono relative a questa ("/a/Trash").
    // moveToTrash la richiede (vuota o "/" = rifiuto) e rifiuta gli account che
    // non stanno sotto la radice: mai spostare in una cartella dedotta.
    void setMuRoot(const QString& r) { m_muRoot = r; }

    // Aggiunge la bandiera S (letto) e toglie N (nuovo).
    void markRead(const QString& path, Callback cb);
    // Sposta nella cartella cestino dell'account (relativa a maildirRoot).
    void moveToTrash(const QString& path, const Account& account, Callback cb);

private:
    void runMu(const QStringList& args, Callback cb);
    QString m_mu, m_muHome, m_muRoot;
    ProcessRunner m_runner;
};

} // namespace boma
