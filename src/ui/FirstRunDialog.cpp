#include "ui/FirstRunDialog.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPointer>
#include <QVBoxLayout>

namespace boma {

FirstRunDialog::FirstRunDialog(QWidget* parent) : QDialog(parent)
{
    setModal(false);
    setWindowTitle(tr("Prima sincronizzazione"));
    resize(520, 300);
    auto* lay = new QVBoxLayout(this);
    m_note = new QLabel(tr("La prima sincronizzazione scarica tutta la posta e puo' richiedere molto tempo "
                           "(per circa 15 GB, anche ore). Puoi chiudere questa finestra: la sincronizzazione "
                           "continua in background. Se Boma viene chiuso, al prossimo avvio riprende da dove era "
                           "arrivata."),
                        this);
    m_note->setWordWrap(true);
    lay->addWidget(m_note);
    m_rowsHost = new QWidget(this);
    new QVBoxLayout(m_rowsHost);
    lay->addWidget(m_rowsHost, 1);
    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    m_close = new QPushButton(tr("Chiudi"), this);
    m_close->setDefault(true);
    // Chiudere significa solo nascondere: nulla qui tocca la sincronizzazione.
    connect(m_close, &QPushButton::clicked, this, &QDialog::hide);
    btns->addWidget(m_close);
    lay->addLayout(btns);
}

void FirstRunDialog::setAccounts(const QList<QPair<QString, QString>>& idAndName)
{
    for (const QString& id : std::as_const(m_order)) {
        Row& r = m_rows[id];
        for (QWidget* w : {static_cast<QWidget*>(r.name), static_cast<QWidget*>(r.status),
                           static_cast<QWidget*>(r.progress), static_cast<QWidget*>(r.error),
                           static_cast<QWidget*>(r.bar), static_cast<QWidget*>(r.retry)})
            delete w;
    }
    m_rows.clear();
    m_order.clear();
    delete m_rowsHost->layout();
    auto* grid = new QGridLayout(m_rowsHost);
    int row = 0;
    for (const auto& p : idAndName) {
        Row r;
        r.name = new QLabel(QStringLiteral("<b>%1</b>").arg(p.second.toHtmlEscaped()), m_rowsHost);
        r.status = new QLabel(tr("In attesa"), m_rowsHost);
        r.progress = new QLabel(m_rowsHost);
        r.bar = new QProgressBar(m_rowsHost);
        r.bar->setRange(0, 0); // occupata: nessuna percentuale inventata
        r.bar->setTextVisible(false);
        r.bar->hide();
        r.error = new QLabel(m_rowsHost);
        r.error->setWordWrap(true);
        r.error->setTextInteractionFlags(Qt::TextSelectableByMouse);
        r.error->hide();
        r.retry = new QPushButton(tr("Riprova"), m_rowsHost);
        r.retry->hide();
        const QString id = p.first;
        QPointer<FirstRunDialog> self(this);
        connect(r.retry, &QPushButton::clicked, this, [self, id]() {
            if (self)
                emit self->retryRequested(id);
        });
        grid->addWidget(r.name, row, 0);
        grid->addWidget(r.status, row, 1);
        grid->addWidget(r.progress, row, 2);
        grid->addWidget(r.retry, row, 3);
        grid->addWidget(r.bar, row + 1, 0, 1, 4);
        grid->addWidget(r.error, row + 2, 0, 1, 4);
        row += 3;
        m_rows.insert(id, r);
        m_order << id;
    }
    grid->setRowStretch(row, 1);
}

void FirstRunDialog::refreshRetry(Row& r)
{
    r.retry->setVisible(r.st == FirstRunState::Failed);
    r.retry->setEnabled(m_retryAllowed);
}

void FirstRunDialog::setState(const QString& id, const FirstRunState& st)
{
    auto it = m_rows.find(id);
    if (it == m_rows.end())
        return;
    Row& r = *it;
    r.st = st.status;
    switch (st.status) {
    case FirstRunState::Waiting: r.status->setText(tr("In attesa")); break;
    case FirstRunState::Running: r.status->setText(tr("In corso")); break;
    case FirstRunState::Done: r.status->setText(tr("Completata")); break;
    case FirstRunState::Failed: r.status->setText(tr("Errore")); break;
    }
    r.progress->setText(st.done >= 0 && st.status == FirstRunState::Running
                            ? QStringLiteral("%1/%2").arg(st.done).arg(st.total)
                            : QString());
    r.bar->setRange(0, 0);
    r.bar->setVisible(st.status == FirstRunState::Running);
    r.error->setText(st.status == FirstRunState::Failed ? st.error : QString());
    r.error->setVisible(st.status == FirstRunState::Failed);
    refreshRetry(r);
}

void FirstRunDialog::setRetryAllowed(bool allowed)
{
    m_retryAllowed = allowed;
    for (Row& r : m_rows)
        refreshRetry(r);
}

QString FirstRunDialog::statusText(const QString& id) const { return m_rows.value(id).status ? m_rows.value(id).status->text() : QString(); }
QString FirstRunDialog::progressText(const QString& id) const { return m_rows.value(id).progress ? m_rows.value(id).progress->text() : QString(); }
QString FirstRunDialog::errorText(const QString& id) const { return m_rows.value(id).error ? m_rows.value(id).error->text() : QString(); }
QProgressBar* FirstRunDialog::bar(const QString& id) const { return m_rows.value(id).bar; }
QPushButton* FirstRunDialog::retryButton(const QString& id) const { return m_rows.value(id).retry; }

} // namespace boma
