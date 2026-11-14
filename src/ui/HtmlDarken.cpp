#include "ui/HtmlDarken.h"
#include <QColor>
#include <QList>
#include <QRegularExpression>
#include <algorithm>

namespace boma {

namespace {

// Luminosita' invertita, limitata per non arrivare a nero/bianco puri.
QColor invert(const QColor& c) {
    float h, s, l, a;
    c.getHslF(&h, &s, &l, &a);
    if (h < 0) h = 0; // grigi: tinta indefinita
    l = std::clamp(1.0f - l, 0.1f, 0.92f);
    return QColor::fromHslF(h, s, l, a);
}

bool parseColorToken(const QString& tok, QColor* out, QString* alpha) {
    alpha->clear();
    if (tok.startsWith(QLatin1Char('#'))) {
        if (tok.size() != 4 && tok.size() != 7) return false;
        *out = QColor(tok);
        return out->isValid();
    }
    if (tok.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        const qsizetype a = tok.indexOf(QLatin1Char('(')), b = tok.lastIndexOf(QLatin1Char(')'));
        if (a < 0 || b < a) return false;
        const QStringList p = tok.mid(a + 1, b - a - 1).split(QRegularExpression(QStringLiteral("[,\\s/]+")),
                                                              Qt::SkipEmptyParts);
        if (p.size() < 3) return false;
        int v[3];
        for (int i = 0; i < 3; ++i) {
            if (p[i].contains(QLatin1Char('%'))) return false;
            bool ok = false;
            v[i] = p[i].toInt(&ok);
            if (!ok) return false;
            v[i] = std::clamp(v[i], 0, 255);
        }
        if (p.size() > 3) *alpha = p[3];
        *out = QColor(v[0], v[1], v[2]);
        return true;
    }
    const QString w = tok.toLower();
    if (w == QLatin1String("transparent") || w == QLatin1String("inherit") || w == QLatin1String("currentcolor"))
        return false;
    if (!QColor::isValidColorName(w)) return false;
    *out = QColor::fromString(w);
    return out->isValid();
}

struct Ctx {
    int darkBgs = 0, lightBgs = 0; // sfondi dichiarati scuri / chiari
    bool changed = false;
};

// Rimappa i colori in un valore (di attributo o di proprieta' CSS).
QString mapValue(const QString& v, bool isBg, Ctx& ctx) {
    static const QRegularExpression tokRe(
        QStringLiteral("#[0-9a-fA-F]{3,8}\\b|rgba?\\([^)]*\\)|[a-zA-Z]+"));
    QString out;
    qsizetype last = 0;
    auto it = tokRe.globalMatch(v);
    while (it.hasNext()) {
        const auto m = it.next();
        QColor c;
        QString alpha;
        if (!parseColorToken(m.captured(), &c, &alpha)) continue;
        if (isBg) {
            if (c.lightnessF() < 0.35) ++ctx.darkBgs;
            else ++ctx.lightBgs;
        }
        const QColor n = invert(c);
        QString repl = n.name();
        if (!alpha.isEmpty())
            repl = QStringLiteral("rgba(%1,%2,%3,%4)").arg(n.red()).arg(n.green()).arg(n.blue()).arg(alpha);
        out += QStringView(v).mid(last, m.capturedStart() - last);
        out += repl;
        last = m.capturedEnd();
        ctx.changed = true;
    }
    return out + QStringView(v).mid(last);
}

// Proprieta' CSS con colori (dentro style="" o <style>).
QString mapCss(const QString& css, Ctx& ctx) {
    static const QRegularExpression propRe(
        QStringLiteral("(?<![-\\w])(color|background-color|background|border(?:-(?:top|right|bottom|left))?(?:-color)?)"
                       "\\s*:\\s*([^;}\"]+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    auto it = propRe.globalMatch(css);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString prop = m.captured(1).toLower();
        const bool bg = prop.startsWith(QLatin1String("background"));
        out += QStringView(css).mid(last, m.capturedStart(2) - last);
        out += mapValue(m.captured(2), bg, ctx);
        last = m.capturedEnd(2);
    }
    return out + QStringView(css).mid(last);
}

QString mapTag(const QString& tag, Ctx& ctx) {
    static const QRegularExpression attrRe(
        QStringLiteral("(?<![-\\w])(bgcolor|color|text|link|vlink|alink|style)=\"([^\"]*)\""),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    auto it = attrRe.globalMatch(tag);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString name = m.captured(1).toLower();
        out += QStringView(tag).mid(last, m.capturedStart(2) - last);
        out += name == QLatin1String("style") ? mapCss(m.captured(2), ctx)
                                              : mapValue(m.captured(2), name == QLatin1String("bgcolor"), ctx);
        last = m.capturedEnd(2);
    }
    return out + QStringView(tag).mid(last);
}

} // namespace

QString darkenHtml(const QString& html, bool* changed) {
    static const QRegularExpression partRe(
        QStringLiteral("<style\\b[^>]*>.*?</style>|<[a-zA-Z][^>]*>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    Ctx ctx;
    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    auto it = partRe.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        const QString part = m.captured();
        if (part.startsWith(QLatin1String("<style"), Qt::CaseInsensitive)) {
            const qsizetype a = part.indexOf(QLatin1Char('>')) + 1;
            const qsizetype b = part.lastIndexOf(QLatin1String("</"));
            out += QStringView(part).left(a) + mapCss(part.mid(a, b - a), ctx) + QStringView(part).mid(b);
        } else {
            out += mapTag(part, ctx);
        }
        last = m.capturedEnd();
    }
    out += QStringView(html).mid(last);
    // gia' scura se gli sfondi scuri prevalgono (un solo dettaglio nero, es. icona, non basta)
    const bool keep = ctx.darkBgs > 0 && ctx.darkBgs >= ctx.lightBgs;
    if (changed) *changed = !keep && out != html;
    return keep ? html : out;
}

} // namespace boma
