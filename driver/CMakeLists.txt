cmake_minimum_required(VERSION 3.21)
project(MouseFilter)

# -------------------------------------------------------
# Этот CMakeLists используется ТОЛЬКО для подсказки IDE.
# Реальная сборка идёт через WDK + MSBuild (см. README).
# -------------------------------------------------------

# Эмулируем пути WDK чтобы IntelliSense не ругался
set(WDK_INC "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0")

add_library(MouseFilter SHARED MouseFilter.c MouseFilter.h)

target_include_directories(MouseFilter PRIVATE
    "${WDK_INC}/km"
    "${WDK_INC}/km/crt"
    "${WDK_INC}/shared"
)

target_compile_definitions(MouseFilter PRIVATE
    _AMD64_
    NTDDI_VERSION=0x0A000000
    _WIN32_WINNT=0x0A00
    WINNT=1
    POOL_NX_OPTIN=1
)
