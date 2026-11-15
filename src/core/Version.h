#pragma once
#include <QString>

namespace boma {

// La versione arriva da project(boma VERSION ...) in CMakeLists.txt (define BOMA_VERSION):
// non esiste un'altra copia del numero. Il commit (BOMA_COMMIT) e' quello del build da git;
// nei build da tarball (pacchetto xbps) non c'e' e la riga riporta solo la versione.
QString versionNumber();
QString versionLine(); // "boma 0.1.4 (abc1234)" oppure "boma 0.1.4"
QString helpText();

// Gestisce --version/-V e --help/-h PRIMA di creare QApplication: nessun display, nessuna
// configurazione, nessun programma esterno. Gli altri argomenti (URL di %U, opzioni di Qt)
// non sono toccati. Ritorna true se ha risposto; *exitCode e' il codice di uscita.
bool handleCommandLine(int argc, char** argv, int* exitCode);

} // namespace boma
