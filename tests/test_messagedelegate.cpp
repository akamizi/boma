#include <QtTest>
#include "core/MailIndex.h"
#include "ui/MessageDelegate.h"
using namespace boma;

class T : public QObject {
    Q_OBJECT
private slots:
    void testFormatSize() {
        QCOMPARE(MessageDelegate::formatSize(500), QString("500 B"));
        QCOMPARE(MessageDelegate::formatSize(52 * 1024), QString("52 KiB"));
        QCOMPARE(MessageDelegate::formatSize(3 * 1024 * 1024), QString("3.0 MiB"));
    }
    void testFormatDate() {
        const QDateTime now(QDate(2026, 11, 13), QTime(15, 0));
        QCOMPARE(MessageDelegate::formatDate(now.addSecs(-120), now), QString("2 min fa"));
        QCOMPARE(MessageDelegate::formatDate(now.addSecs(-2 * 3600), now), QString("2 ore fa"));
        QCOMPARE(MessageDelegate::formatDate(QDateTime(QDate(2026, 11, 12), QTime(18, 2)), now), QString("ieri alle 18:02"));
        QVERIFY(MessageDelegate::formatDate(QDateTime(QDate(2026, 10, 30), QTime(12, 14)), now).endsWith("12:14"));
        QVERIFY(!MessageDelegate::formatDate(QDateTime(QDate(2024, 1, 5), QTime(9, 0)), now).contains(":"));
    }
    void testParseSizeAndFlags() {
        MailIndex idx;
        QString err;
        const auto l = idx.parse(R"([{":path":"/a/cur/x",":size":53248,":flags":["flagged","attach","unread"]},{":path":"/a/cur/y"}])", &err);
        QCOMPARE(l.size(), 2);
        QCOMPARE(l[0].size, qint64(53248));
        QVERIFY(l[0].flagged && l[0].hasAttachment && l[0].unread);
        QVERIFY(!l[1].flagged);
    }
};
QTEST_MAIN(T)
#include "test_messagedelegate.moc"
