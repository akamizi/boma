#include "core/InboxStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace boma {

InboxStore::InboxStore(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {
    if (path_.isEmpty())
        path_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/gmail-inbox.json");
    load();
}

void InboxStore::load() {
    QFile f(path_);
    if (!f.exists()) return;
    QJsonParseError pe;
    QByteArray data;
    if (f.open(QIODevice::ReadOnly)) data = f.readAll();
    const QJsonDocument d = QJsonDocument::fromJson(data, &pe);
    if (data.isEmpty() || pe.error != QJsonParseError::NoError || !d.isObject() ||
        !d.object().value(QStringLiteral("inbox")).isObject()) {
        loadError_ = QObject::tr("File della Posta in arrivo Gmail illeggibile (%1): copiato in .corrupt, si riparte da zero.").arg(path_);
        QFile::remove(path_ + QStringLiteral(".corrupt"));
        QFile::copy(path_, path_ + QStringLiteral(".corrupt"));
        return;
    }
    const QJsonObject o = d.object().value(QStringLiteral("inbox")).toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QStringList l;
        for (const QJsonValue& v : it.value().toArray())
            if (!v.toString().isEmpty()) l << v.toString();
        sets_.insert(it.key(), l);
    }
}

bool InboxStore::save() {
    QJsonObject inbox;
    for (auto it = sets_.cbegin(); it != sets_.cend(); ++it) inbox.insert(it.key(), QJsonArray::fromStringList(it.value()));
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("inbox"), inbox);
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly)) {
        lastError_ = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!f.commit()) {
        lastError_ = f.errorString();
        return false;
    }
    lastError_.clear();
    return true;
}

QStringList InboxStore::messageIds(const QString& accountId) const { return sets_.value(accountId); }

bool InboxStore::replace(const QString& accountId, const QStringList& ids) {
    QStringList l = QSet<QString>(ids.begin(), ids.end()).values();
    l.sort();
    sets_.insert(accountId, l);
    const bool ok = save();
    emit changed();
    return ok;
}

} // namespace boma
