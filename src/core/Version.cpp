#include "core/Version.h"
#include <cstdio>
#include <cstring>

#ifndef BOMA_VERSION
#error "BOMA_VERSION viene definito da CMakeLists.txt (project(boma VERSION ...))"
#endif
#ifndef BOMA_COMMIT
#define BOMA_COMMIT ""
#endif

namespace boma {

QString versionNumber() { return QStringLiteral(BOMA_VERSION); }

QString versionLine() {
    const QString commit = QStringLiteral(BOMA_COMMIT);
    return commit.isEmpty() ? QStringLiteral("boma %1").arg(versionNumber())
                            : QStringLiteral("boma %1 (%2)").arg(versionNumber(), commit);
}

QString helpText() {
    return QStringLiteral(
        "Uso: boma [opzioni]\n"
        "Client di posta personale per Linux (mbsync, mu, msmtp, gpg).\n"
        "\n"
        "Opzioni:\n"
        "  -V, --version   stampa la versione ed esce\n"
        "  -h, --help      mostra questo aiuto ed esce\n"
        "\n"
        "Senza opzioni apre la finestra principale. Le impostazioni sono in\n"
        "~/.config/boma/config.ini (si modificano da Impostazioni).\n");
}

bool handleCommandLine(int argc, char** argv, int* exitCode) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "-V")) {
            std::fprintf(stdout, "%s\n", versionLine().toUtf8().constData());
            *exitCode = std::fflush(stdout) == 0 ? 0 : 1;
            return true;
        }
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::fputs(helpText().toUtf8().constData(), stdout);
            *exitCode = std::fflush(stdout) == 0 ? 0 : 1;
            return true;
        }
    }
    return false;
}

} // namespace boma
