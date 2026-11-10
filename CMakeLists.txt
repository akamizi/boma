cmake_minimum_required(VERSION 3.20)
project(boma VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET gmime-3.0)

add_library(boma_core STATIC
    src/core/Config.cpp
    src/core/ProcessRunner.cpp
    src/core/MaildirWriter.cpp
    src/core/MailActions.cpp
    src/core/SyncService.cpp
    src/core/MailIndex.cpp
    src/ui/MessageListModel.cpp
    src/ui/MainWindow.cpp
    src/mime/MessageParser.cpp
    src/ui/MessageView.cpp
)
target_include_directories(boma_core PUBLIC src)
target_link_libraries(boma_core PUBLIC Qt6::Widgets PkgConfig::GMIME)

add_executable(boma src/main.cpp)
target_link_libraries(boma PRIVATE boma_core)

enable_testing()

# boma_add_test(name src...) : eseguibile di test collegato a boma_core
function(boma_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE boma_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
