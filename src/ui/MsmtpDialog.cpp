#include "ui/MsmtpDialog.h"

#include "core/ConfigCheck.h"
#include "core/MsmtpConfig.h"

#include <QFormLayout>
#include <QVBoxLayout>

namespace boma {

namespace {
void fillOnOff(QComboBox* c, const QString& def) {
    c->addItem(def.isEmpty() ? QObject::tr("(non impostato)") : QObject::tr("(predefinito: %1)").arg(def), QString());
    c->addItem(QStringLiteral("on"), QStringLiteral("on"));
    c->addItem(QStringLiteral("off"), QStringLiteral("off"));
}
void selectData(QComboBox* c, const QString& v) {
    const int i = c->findData(v);
    if (i >= 0) c->setCurrentIndex(i);
    else if (c->isEditable()) c->setEditText(v);
}
} // namespace

MsmtpDialog::MsmtpDialog(const QString& rcPath, const QString& account, QWidget* parent)
    : QDialog(parent), path_(rcPath), account_(account.trimmed()) {
    setWindowTitle(tr("Server di invio (msmtp): %1").arg(account_));
    text_ = ConfigCheck::readFile(path_);
    const QMap<QString, QString> cur = MsmtpConfig::accountSettings(text_, account_);
    const QMap<QString, QString> def = MsmtpConfig::defaultSettings(text_);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("File: %1").arg(path_.toHtmlEscaped()), this));
    auto* f = new QFormLayout;
    host_ = new QLineEdit(cur.value(QStringLiteral("host")), this);
    port_ = new QLineEdit(cur.value(QStringLiteral("port")), this);
    port_->setPlaceholderText(def.value(QStringLiteral("port"), QStringLiteral("587")));
    tls_ = new QComboBox(this);
    fillOnOff(tls_, def.value(QStringLiteral("tls")));
    starttls_ = new QComboBox(this);
    fillOnOff(starttls_, def.value(QStringLiteral("tls_starttls")));
    auth_ = new QComboBox(this);
    auth_->setEditable(true);
    const QString da = def.value(QStringLiteral("auth"));
    auth_->addItem(da.isEmpty() ? tr("(non impostato)") : tr("(predefinito: %1)").arg(da), QString());
    for (const char* a : {"on", "off", "plain", "login", "scram-sha-256", "oauthbearer", "xoauth2"})
        auth_->addItem(QString::fromLatin1(a), QString::fromLatin1(a));
    user_ = new QLineEdit(cur.value(QStringLiteral("user")), this);
    from_ = new QLineEdit(cur.value(QStringLiteral("from")), this);
    passEval_ = new QLineEdit(cur.value(QStringLiteral("passwordeval")), this);
    passEval_->setPlaceholderText(tr("es.: pass show mail/miaposta"));
    selectData(tls_, cur.value(QStringLiteral("tls")));
    selectData(starttls_, cur.value(QStringLiteral("tls_starttls")));
    selectData(auth_, cur.value(QStringLiteral("auth")));
    f->addRow(tr("Server SMTP"), host_);
    f->addRow(tr("Porta"), port_);
    f->addRow(QStringLiteral("TLS"), tls_);
    f->addRow(QStringLiteral("STARTTLS"), starttls_);
    f->addRow(tr("Autenticazione"), auth_);
    f->addRow(tr("Utente"), user_);
    f->addRow(tr("Indirizzo mittente (from)"), from_);
    f->addRow(tr("Comando password"), passEval_);
    root->addLayout(f);
    auto* note = new QLabel(tr("La password non si scrive qui: il comando (passwordeval) la fornisce a msmtp. "
                               "Le altre righe del file restano invariate; il file precedente va in .bak."),
                            this);
    note->setWordWrap(true);
    root->addWidget(note);
    if (MsmtpConfig::hasPlainPassword(text_, account_)) {
        auto* w = new QLabel(tr("Attenzione: l'account ha una `password` in chiaro nel file. Boma non la tocca; "
                                "conviene sostituirla con passwordeval."),
                             this);
        w->setWordWrap(true);
        w->setStyleSheet(QStringLiteral("color:#9d5d00;"));
        root->addWidget(w);
    }
    error_ = new QLabel(this);
    error_->setStyleSheet(QStringLiteral("color:#a4262c;"));
    error_->setWordWrap(true);
    root->addWidget(error_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    root->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &MsmtpDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &MsmtpDialog::reject);
}

void MsmtpDialog::accept() {
    auto data = [](QComboBox* c) {
        const QString t = c->currentText().trimmed();
        const int i = c->findText(t);
        return i >= 0 ? c->itemData(i).toString() : t; // "(predefinito...)" = non impostato
    };
    QMap<QString, QString> v;
    v[QStringLiteral("host")] = host_->text().trimmed();
    v[QStringLiteral("port")] = port_->text().trimmed();
    v[QStringLiteral("tls")] = tls_->currentData().toString();
    v[QStringLiteral("tls_starttls")] = starttls_->currentData().toString();
    v[QStringLiteral("auth")] = data(auth_);
    v[QStringLiteral("user")] = user_->text().trimmed();
    v[QStringLiteral("from")] = from_->text().trimmed();
    v[QStringLiteral("passwordeval")] = passEval_->text().trimmed();
    bool okPort = true;
    if (!v[QStringLiteral("port")].isEmpty()) {
        const int p = v[QStringLiteral("port")].toInt(&okPort);
        okPort = okPort && p > 0 && p < 65536;
    }
    if (!okPort) {
        error_->setText(tr("Porta non valida."));
        return;
    }
    if (v[QStringLiteral("host")].isEmpty() && !MsmtpConfig::hasAccount(text_, account_)) {
        error_->setText(tr("Il server SMTP e' obbligatorio per un account nuovo."));
        return;
    }
    QString err;
    const QByteArray updated = MsmtpConfig::applyAccount(text_, account_, v, &err);
    if (updated.isNull()) {
        error_->setText(err);
        return;
    }
    if (!MsmtpConfig::save(path_, updated, &err)) {
        error_->setText(tr("Salvataggio non riuscito: %1").arg(err));
        return;
    }
    QDialog::accept();
}

} // namespace boma
