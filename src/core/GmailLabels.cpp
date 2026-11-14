#include "core/GmailLabels.h"
#include "gmail_labels_py.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace boma {

QString GmailLabels::script() { return QString::fromUtf8(kGmailLabelsScript); }

QHash<QString, QStringList> GmailLabels::parseOutput(const QByteArray& out, int* done) {
    QHash<QString, QStringList> m;
    for (const QByteArray& line : out.split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.contains(QLatin1String("done"))) {
            if (done) *done = o.value(QLatin1String("done")).toInt();
            continue;
        }
        const QString id = o.value(QLatin1String("id")).toString();
        if (id.isEmpty()) continue;
        QStringList l;
        for (const QJsonValue& v : o.value(QLatin1String("labels")).toArray())
            if (!v.toString().isEmpty()) l << v.toString();
        if (!l.isEmpty()) m.insert(id, l);
    }
    return m;
}

void GmailLabels::fetch(ProcessRunner* runner, const QString& python, const QString& mbsyncConfig,
                        const QString& channel, std::function<void(QHash<QString, QStringList>, QString)> cb) {
    const QString cfg = mbsyncConfig.isEmpty() ? QDir::homePath() + QStringLiteral("/.mbsyncrc") : mbsyncConfig;
    const QString py = python.isEmpty() ? QStringLiteral("python3") : python;
    // -I: modalita' isolata (niente directory corrente ne' variabili PYTHON* nel percorso di import)
    runner->run(py, {QStringLiteral("-I"), QStringLiteral("-c"), script(), cfg, channel}, std::chrono::minutes(10),
                [cb = std::move(cb)](ProcessResult r) {
                    int done = -1;
                    auto map = parseOutput(r.out, &done);
                    if (r.exitCode != 0 || done < 0) {
                        QString e = QString::fromUtf8(r.err).trimmed();
                        if (e.isEmpty()) e = r.timedOut ? QObject::tr("tempo scaduto") : QObject::tr("importazione non riuscita");
                        cb({}, e);
                        return;
                    }
                    cb(map, QString());
                });
}

} // namespace boma
