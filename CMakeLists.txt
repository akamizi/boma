cmake_minimum_required(VERSION 3.20)
project(boma VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test Network DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET gmime-3.0)

# Lo script Python delle etichette Gmail viene incorporato nel binario (nessun file da installare).
file(READ ${CMAKE_SOURCE_DIR}/src/core/gmail_labels.py BOMA_GMAIL_LABELS_PY)
file(WRITE ${CMAKE_BINARY_DIR}/generated/gmail_labels_py.h.tmp
     "static const char kGmailLabelsScript[] = R\"BOMAPY(${BOMA_GMAIL_LABELS_PY})BOMAPY\";\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/generated/gmail_labels_py.h.tmp ${CMAKE_BINARY_DIR}/generated/gmail_labels_py.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/gmail_labels.py)

add_library(boma_core STATIC
    src/core/Config.cpp
    src/core/Version.cpp
    src/core/ConfigCheck.cpp
    src/ui/SettingsDialog.cpp
    src/ui/Theme.cpp
    src/ui/MsmtpDialog.cpp
    src/core/MsmtpConfig.cpp
    src/core/FolderCleaner.cpp
    src/ui/MessageDelegate.cpp
    src/ui/HtmlDarken.cpp
    src/ui/RichTextToolBar.cpp
    src/core/ProcessRunner.cpp
    src/core/MaildirWriter.cpp
    src/core/MailActions.cpp
    src/core/SyncService.cpp
    src/core/MailIndex.cpp
    src/core/TagStore.cpp
    src/core/GmailLabels.cpp
    src/core/MailWatcher.cpp
    src/core/Notifier.cpp
    src/core/NewMailNotifier.cpp
    src/core/UnreadCounter.cpp
    src/core/SendController.cpp
    src/ui/MessageListModel.cpp
    src/ui/MainWindow.cpp
    src/ui/FolderPane.cpp
    src/ui/ReplyBuilder.cpp
    src/mime/MessageParser.cpp
    src/mime/MessageBuilder.cpp
    src/pgp/GpgService.cpp
    src/send/Sender.cpp
    src/ui/MessageView.cpp
    src/ui/ComposeWindow.cpp
)
target_include_directories(boma_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(boma_core PUBLIC Qt6::Widgets Qt6::Network Qt6::DBus PkgConfig::GMIME)

# Unica fonte della versione: project(boma VERSION ...). Il commit e' facoltativo (build da git).
target_compile_definitions(boma_core PUBLIC BOMA_VERSION="${PROJECT_VERSION}")
if(NOT DEFINED BOMA_COMMIT)
    execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=9 HEAD
                    OUTPUT_VARIABLE BOMA_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE BOMA_GIT_RC)
    if(NOT BOMA_GIT_RC EQUAL 0)
        set(BOMA_COMMIT "")
    endif()
endif()
if(BOMA_COMMIT)
    set_source_files_properties(src/core/Version.cpp PROPERTIES COMPILE_DEFINITIONS "BOMA_COMMIT=\"${BOMA_COMMIT}\"")
endif()

add_executable(boma src/main.cpp)
target_link_libraries(boma PRIVATE boma_core)

# Avvisi del compilatore solo per i sorgenti del progetto (le dipendenze non passano da qui),
# mai -Werror: un compilatore piu' nuovo non deve rompere il build per un avviso.
set(BOMA_WARNINGS $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra>)
target_compile_options(boma_core PRIVATE ${BOMA_WARNINGS})
target_compile_options(boma PRIVATE ${BOMA_WARNINGS})

include(GNUInstallDirs)
install(TARGETS boma RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/boma.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

enable_testing()

# boma_add_test(name src...) : eseguibile di test collegato a boma_core
function(boma_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE boma_core Qt6::Test)
    target_compile_options(${name} PRIVATE ${BOMA_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
