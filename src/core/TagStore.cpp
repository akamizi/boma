#include "core/TagStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace boma {

TagStore::TagStore(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {
    if (path_.isEmpty())
        path_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tags.json");
    load();
}

QString TagStore::normalize(const QString& tag) {
    QString t = tag.simplified();
    for (QChar& c : t)
        if (c.isNonCharacter() || c.category() == QChar::Other_Control) c = QLatin1Char(' ');
    return t.simplified().left(40);
}

void TagStore::load() {
    QFile f(path_);
    if (!f.exists()) return;
    QJsonParseError pe;
    QByteArray data;
    if (f.open(QIODevice::ReadOnly)) data = f.readAll();
    const QJsonDocument d = QJsonDocument::fromJson(data, &pe);
    if (data.isEmpty() || pe.error != QJsonParseError::NoError || !d.isObject() ||
        !d.object().value(QStringLiteral("tags")).isObject()) {
        loadError_ = QObject::tr("File delle etichette illeggibile (%1): copiato in .corrupt, si riparte da zero.").arg(path_);
        QFile::remove(path_ + QStringLiteral(".corrupt"));
        QFile::copy(path_, path_ + QStringLiteral(".corrupt"));
        return;
    }
    const QJsonObject o = d.object().value(QStringLiteral("tags")).toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QStringList l;
        for (const QJsonValue& v : it.value().toArray()) {
            const QString t = normalize(v.toString());
            if (!t.isEmpty() && !l.contains(t, Qt::CaseInsensitive)) l << t;
        }
        if (!l.isEmpty()) byId_.insert(it.key(), l);
    }
}

bool TagStore::save() {
    QJsonObject tags;
    for (auto it = byId_.cbegin(); it != byId_.cend(); ++it) tags.insert(it.key(), QJsonArray::fromStringList(it.value()));
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("tags"), tags);
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

QStringList TagStore::tags(const QString& id) const { return byId_.value(id); }

QMap<QString, int> TagStore::counts() const {
    QMap<QString, int> m;
    for (const QStringList& l : byId_)
        for (const QString& t : l) ++m[t];
    return m;
}

QStringList TagStore::messageIds(const QString& tag) const {
    QStringList out;
    for (auto it = byId_.cbegin(); it != byId_.cend(); ++it)
        if (it.value().contains(tag, Qt::CaseInsensitive)) out << it.key();
    out.sort();
    return out;
}

QString TagStore::canonical(const QString& tag) const {
    const QString n = normalize(tag);
    for (const QStringList& l : byId_)
        for (const QString& t : l)
            if (t.compare(n, Qt::CaseInsensitive) == 0) return t;
    return n;
}

bool TagStore::setTag(const QStringList& ids, const QString& tag, bool on) {
    const QString t = canonical(tag);
    if (t.isEmpty()) return true;
    bool any = false;
    for (const QString& id : ids) {
        if (id.isEmpty()) continue;
        QStringList l = byId_.value(id);
        const int pos = [&] {
            for (int i = 0; i < l.size(); ++i)
                if (l.at(i).compare(t, Qt::CaseInsensitive) == 0) return i;
            return -1;
        }();
        if (on && pos < 0) {
            l << t;
            l.sort(Qt::CaseInsensitive);
            byId_.insert(id, l);
            any = true;
        } else if (!on && pos >= 0) {
            l.removeAt(pos);
            if (l.isEmpty()) byId_.remove(id);
            else byId_.insert(id, l);
            any = true;
        }
    }
    if (!any) return true;
    const bool ok = save();
    emit changed();
    return ok;
}

bool TagStore::renameTag(const QString& from, const QString& to) {
    const QString n = normalize(to);
    if (n.isEmpty() || from.compare(n, Qt::CaseInsensitive) == 0 && from == n) return true;
    const QString target = canonical(n);
    bool any = false;
    for (auto it = byId_.begin(); it != byId_.end(); ++it) {
        QStringList& l = it.value();
        for (int i = 0; i < l.size(); ++i) {
            if (l.at(i).compare(from, Qt::CaseInsensitive) != 0) continue;
            l.removeAt(i);
            if (!l.contains(target, Qt::CaseInsensitive)) l << target;
            l.sort(Qt::CaseInsensitive);
            any = true;
            break;
        }
    }
    if (!any) return true;
    const bool ok = save();
    emit changed();
    return ok;
}

bool TagStore::removeTag(const QString& tag) {
    bool any = false;
    for (auto it = byId_.begin(); it != byId_.end();) {
        QStringList& l = it.value();
        const qsizetype before = l.size();
        for (int i = l.size() - 1; i >= 0; --i)
            if (l.at(i).compare(tag, Qt::CaseInsensitive) == 0) l.removeAt(i);
        any |= l.size() != before;
        it = l.isEmpty() ? byId_.erase(it) : std::next(it);
    }
    if (!any) return true;
    const bool ok = save();
    emit changed();
    return ok;
}

} // namespace boma
