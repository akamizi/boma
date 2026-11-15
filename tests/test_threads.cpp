#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "core/TagStore.h"
#include "ui/MessageListModel.h"
using namespace boma;

namespace {
// mu finto: risponde sempre con lo stesso JSON (letto da un file accanto allo script).
QString fakeMu(const QString& dir, const QByteArray& json) {
    QFile j(dir + "/out.json");
    if (!j.open(QIODevice::WriteOnly)) qFatal("non scrivibile: %s", qPrintable(j.fileName()));
    j.write(json);
    j.close();
    QFile f(dir + "/mu");
    if (!f.open(QIODevice::WriteOnly)) qFatal("non scrivibile: %s", qPrintable(f.fileName()));
    f.write("#!/bin/sh\ncat \"$(dirname \"$0\")/out.json\"\n");
    f.close();
    f.setPermissions(QFile::permissions(f.fileName()) | QFile::ExeOwner);
    return f.fileName();
}
QByteArray msg(const char* path, const char* id, const char* refs, bool unread, const char* subj, int date) {
    return QByteArray("{\":path\":\"/m/") + path + "\",\":message-id\":\"" + id + "\",\":subject\":\"" + subj +
           "\",\":date-unix\":" + QByteArray::number(date) + ",\":size\":100,\":flags\":[\"" +
           (unread ? "unread" : "seen") + "\"],\":references\":[" + refs + "]}";
}
} // namespace

class T : public QObject {
    Q_OBJECT
    QTemporaryDir dir_{QDir::currentPath() + QStringLiteral("/thr-XXXXXX")};
    MailIndex* idx_ = nullptr;
    // c risponde a b, che risponde ad a; d e' un messaggio isolato. Ordine: dal piu' recente.
    QByteArray sample() {
        return "[" + msg("d", "d@x", "", false, "Isolato", 500) + "," + msg("c", "c@x", "\"a@x\",\"b@x\"", true, "Re: Re: Tema", 400) +
               "," + msg("b", "b@x", "\"a@x\"", false, "Re: Tema", 300) + "," + msg("a", "a@x", "", false, "Tema", 200) + "]";
    }
private slots:
    void testFlatByDefault() {
        MailIndex idx(nullptr, fakeMu(dir_.path(), sample()));
        MessageListModel m(&idx);
        m.setQuery("x");
        m.fetchMore({});
        QTRY_COMPARE(m.rowCount(), 4);
        QCOMPARE(m.index(0, 0).data(MessageListModel::ThreadSizeRole).toInt(), 0);
    }
    void testTagsRoleFollowsTagStore() {
        MailIndex idx(nullptr, fakeMu(dir_.path(), sample()));
        TagStore ts(dir_.path() + "/tags.json");
        MessageListModel m(&idx);
        m.setTagStore(&ts);
        m.setQuery("x");
        m.fetchMore({});
        QTRY_COMPARE(m.rowCount(), 4);
        QVERIFY(m.index(0, 0).data(MessageListModel::TagsRole).toStringList().isEmpty());
        QSignalSpy sp(&m, &QAbstractItemModel::dataChanged);
        const QString id = m.summary(0).messageId;
        QVERIFY(!id.isEmpty());
        ts.setTag({id}, "Lavoro", true);
        QCOMPARE(sp.count(), 1);
        QCOMPARE(m.index(0, 0).data(MessageListModel::TagsRole).toStringList(), QStringList{"Lavoro"});
        QVERIFY(m.index(1, 0).data(MessageListModel::TagsRole).toStringList().isEmpty());
    }
    void testGroupsAndExpands() {
        MailIndex idx(nullptr, fakeMu(dir_.path(), sample()));
        MessageListModel m(&idx);
        m.setThreaded(true);
        m.setQuery("x");
        m.fetchMore({});
        QTRY_COMPARE(m.rowCount(), 2); // isolato + conversazione chiusa
        QCOMPARE(m.index(0, 0).data(Qt::DisplayRole).toString(), QString("Isolato"));
        QCOMPARE(m.index(1, 0).data(MessageListModel::ThreadSizeRole).toInt(), 3);
        QVERIFY(m.index(1, 0).data(MessageListModel::UnreadRole).toBool()); // c non letto
        QCOMPARE(m.index(1, 0).data(Qt::DisplayRole).toString(), QString("Re: Re: Tema")); // il piu' recente
        m.toggleExpanded(1);
        QCOMPARE(m.rowCount(), 4);
        QCOMPARE(m.index(2, 0).data(MessageListModel::ThreadLevelRole).toInt(), 1);
        m.toggleExpanded(1);
        QCOMPARE(m.rowCount(), 2);
    }
    void testCheckingClosedThreadSelectsAllMessages() {
        MailIndex idx(nullptr, fakeMu(dir_.path(), sample()));
        MessageListModel m(&idx);
        m.setThreaded(true);
        m.setQuery("x");
        m.fetchMore({});
        QTRY_COMPARE(m.rowCount(), 2);
        m.setData(m.index(1, 0), true, MessageListModel::CheckedRole);
        QCOMPARE(m.checkedCount(), 3);
        QCOMPARE(m.checkedSummaries().size(), 3);
        QVERIFY(m.index(1, 0).data(MessageListModel::CheckedRole).toBool());
        m.setData(m.index(1, 0), false, MessageListModel::CheckedRole);
        QCOMPARE(m.checkedCount(), 0);
    }
    void testRemoveHiddenMessageByPath() {
        MailIndex idx(nullptr, fakeMu(dir_.path(), sample()));
        MessageListModel m(&idx);
        m.setThreaded(true);
        m.setQuery("x");
        m.fetchMore({});
        QTRY_COMPARE(m.rowCount(), 2);
        QVERIFY(m.hasPath("/m/a"));
        m.removePath("/m/a"); // nascosto nella conversazione chiusa
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.index(1, 0).data(MessageListModel::ThreadSizeRole).toInt(), 2);
        m.removePath("/m/c");
        m.removePath("/m/b");
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.index(0, 0).data(MessageListModel::ThreadSizeRole).toInt(), 0);
    }
    void testTurningThreadsOffRestoresFlatList() {
        MailIndex idx(nullptr, fakeMu(dir_.path(), sample()));
        MessageListModel m(&idx);
        m.setThreaded(true);
        m.setQuery("x");
        m.fetchMore({});
        QTRY_COMPARE(m.rowCount(), 2);
        m.setThreaded(false);
        QCOMPARE(m.rowCount(), 4);
    }
};
int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    T t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_threads.moc"
