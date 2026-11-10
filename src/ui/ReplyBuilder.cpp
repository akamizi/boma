#include "ui/ReplyBuilder.h"

#include "ui/ComposeWindow.h"

#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocumentFragment>

namespace boma {

QString bareAddress(const QString& item) {
    static const QRegularExpression re(QStringLiteral("<([^<>]*)>\\s*$"));
    const auto mt = re.match(item);
    return (mt.hasMatch() ? mt.captured(1) : item).trimmed().toLower();
}

namespace {

QString withPrefix(const QString& subject, const QString& prefix, const QRegularExpression& existing) {
    QString s = subject.trimmed();
    // toglie TUTTI i prefissi equivalenti ripetuti, poi ne mette uno solo
    for (;;) {
        const auto m = existing.match(s);
        if (!m.hasMatch()) break;
        s = s.mid(m.capturedLength()).trimmed();
    }
    return prefix + s;
}

QString bodyText(const ParsedMessage& m) {
    if (!m.plainText.isEmpty() || m.html.isEmpty()) return m.plainText;
    return QTextDocumentFragment::fromHtml(m.html).toPlainText();
}

} // namespace

OutgoingMessage buildReply(const ParsedMessage& m, const QString& accountId, const Config& config, ReplyMode mode) {
    OutgoingMessage out;

    QSet<QString> own;
    QList<Identity> all;
    for (const Account& a : config.accounts())
        for (const Identity& i : config.identities(a.id)) {
            all << i;
            own.insert(i.address.trimmed().toLower());
        }

    const QStringList to = ComposeWindow::splitAddresses(m.to);
    const QStringList cc = ComposeWindow::splitAddresses(m.cc);

    // identita'
    QList<Identity> mine;
    for (const Identity& i : all)
        if (i.accountId == accountId) mine << i;
    Identity chosen;
    bool found = false;
    // indirizzo tra i destinatari "A" prima, poi tra i "Cc"
    for (const QStringList* l : {&to, &cc})
        for (const QString& a : *l)
            for (const Identity& i : mine)
                if (!found && bareAddress(a) == i.address.trimmed().toLower()) {
                    chosen = i;
                    found = true;
                }
    if (!found) chosen = !mine.isEmpty() ? mine.first() : (!all.isEmpty() ? all.first() : Identity());
    out.identity = chosen;

    static const QRegularExpression reRe(QStringLiteral("^(re|r|aw|sv)\\s*(\\[\\d+\\])?\\s*:\\s*"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fwRe(QStringLiteral("^(fwd?|i|inol|wg)\\s*:\\s*"), QRegularExpression::CaseInsensitiveOption);

    const QString body = bodyText(m);
    const QString dateText = m.date.isValid() ? QLocale().toString(m.date, QLocale::LongFormat) : QString();

    if (mode == ReplyMode::Forward) {
        out.subject = withPrefix(m.subject, QStringLiteral("Fwd: "), fwRe);
        out.plainText = QStringLiteral("\n\n-------- Messaggio inoltrato --------\nDa: %1\nA: %2\n%3Data: %4\nOggetto: %5\n\n%6\n")
                            .arg(m.from, m.to, m.cc.isEmpty() ? QString() : QStringLiteral("Cc: %1\n").arg(m.cc), dateText, m.subject, body);
        return out;
    }

    out.subject = withPrefix(m.subject, QStringLiteral("Re: "), reRe);
    out.inReplyToMessageId = m.messageId;
    out.referencesIds = m.references;
    if (!m.messageId.isEmpty()) out.referencesIds << m.messageId;

    const QString primary = m.replyTo.isEmpty() ? m.from : m.replyTo;
    QSet<QString> seen;
    auto add = [&](QStringList& dst, const QStringList& src, bool dropOwn) {
        for (const QString& a : src) {
            const QString b = bareAddress(a);
            if (b.isEmpty() || seen.contains(b) || (dropOwn && own.contains(b))) continue;
            seen.insert(b);
            dst << a.trimmed();
        }
    };
    const QStringList first = ComposeWindow::splitAddresses(primary);
    bool fromMe = false;
    for (const QString& a : first)
        if (own.contains(bareAddress(a))) fromMe = true;
    if (fromMe) {
        // risposta a un proprio messaggio: ai destinatari originali
        add(out.to, to, false);
        if (mode == ReplyMode::ReplyAll) add(out.cc, cc, true);
        if (out.to.isEmpty()) add(out.to, first, false);
    } else {
        add(out.to, first, false);
        if (mode == ReplyMode::ReplyAll) {
            add(out.to, to, true);
            add(out.cc, cc, true);
        }
    }

    QString quoted;
    for (const QString& line : body.trimmed().split(QLatin1Char('\n')))
        quoted += QStringLiteral("> ") + QString(line).remove(QLatin1Char('\r')) + QLatin1Char('\n');
    out.plainText = QStringLiteral("\n%1 ha scritto:\n%2\n").arg(
        dateText.isEmpty() ? m.from : QStringLiteral("Il %1, %2").arg(dateText, m.from), quoted);
    return out;
}

} // namespace boma
