#pragma once

#include <QDialog>
#include <QHash>
#include <QLabel>
#include <QList>
#include <QPair>
#include <QProgressBar>
#include <QPushButton>
#include <QString>
#include <QStringList>

namespace boma {

// Stato di un account nella prima sincronizzazione (tenuto da MainWindow, che
// puo' ricostruire il dialogo in qualsiasi momento).
struct FirstRunState {
    enum Status { Waiting, Running, Done, Failed };
    Status status = Waiting;
    int done = -1, total = 0; // contatori di mbsync -V; done < 0 = ancora nessuno
    QString error;
};

// Dialogo NON modale con l'avanzamento per account. E' solo una vista: non
// possiede ne' avvia la sincronizzazione (appartiene a SyncService/MainWindow).
// "Chiudi" nasconde il dialogo e basta; "Riprova" emette soltanto
// retryRequested: chi ascolta decide (e rispetta la serializzazione di
// SyncService). L'avanzamento e' quello dei contatori di mbsync (cartelle o
// canali, "N/M"): non e' una percentuale dei dati, quindi la barra resta
// "occupata" e mai si mostra un valore inventato.
class FirstRunDialog : public QDialog {
    Q_OBJECT
public:
    explicit FirstRunDialog(QWidget* parent = nullptr);

    void setAccounts(const QList<QPair<QString, QString>>& idAndName);
    void setState(const QString& id, const FirstRunState& st);
    // I pulsanti "Riprova" sono attivi solo se nessuna sincronizzazione e' in corso.
    void setRetryAllowed(bool allowed);

    QStringList accountIds() const { return m_order; }
    QString statusText(const QString& id) const;
    QString progressText(const QString& id) const;
    QString errorText(const QString& id) const;
    QProgressBar* bar(const QString& id) const;
    QPushButton* retryButton(const QString& id) const;
    QPushButton* closeButton() const { return m_close; }
    QLabel* noteLabel() const { return m_note; }

signals:
    void retryRequested(QString accountId);

private:
    struct Row {
        QLabel *name = nullptr, *status = nullptr, *progress = nullptr, *error = nullptr;
        QProgressBar* bar = nullptr;
        QPushButton* retry = nullptr;
        FirstRunState::Status st = FirstRunState::Waiting;
    };
    void refreshRetry(Row& r);

    QHash<QString, Row> m_rows;
    QStringList m_order;
    QWidget* m_rowsHost = nullptr;
    QLabel* m_note = nullptr;
    QPushButton* m_close = nullptr;
    bool m_retryAllowed = true;
};

} // namespace boma
