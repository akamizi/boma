#pragma once
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>

namespace boma {

// Modifica del blocco di UN account in ~/.msmtprc (server, porta, TLS, utente,
// comando della password). Riscrive solo quel blocco, con copia .bak; le password
// in chiaro non si gestiscono (si usa passwordeval). La scrittura avviene al "Salva".
class MsmtpDialog : public QDialog {
    Q_OBJECT
public:
    MsmtpDialog(const QString& rcPath, const QString& account, QWidget* parent = nullptr);

    QLineEdit* hostEdit() const { return host_; }
    QLineEdit* portEdit() const { return port_; }
    QComboBox* tlsCombo() const { return tls_; }
    QComboBox* starttlsCombo() const { return starttls_; }
    QComboBox* authCombo() const { return auth_; }
    QLineEdit* userEdit() const { return user_; }
    QLineEdit* fromEdit() const { return from_; }
    QLineEdit* passwordEvalEdit() const { return passEval_; }
    QLabel* errorLabel() const { return error_; }
    QString account() const { return account_; }

public slots:
    void accept() override;

private:
    QString path_, account_;
    QByteArray text_;
    QLineEdit *host_, *port_, *user_, *from_, *passEval_;
    QComboBox *tls_, *starttls_, *auth_;
    QLabel* error_;
};

} // namespace boma
