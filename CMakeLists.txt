cmake_minimum_required(VERSION 3.16.0)
# explicit version: ESP-IDF otherwise runs `git describe`, which fails in a
# repository without commits
set(PROJECT_VER "0.2.0")
# release builds (.github/workflows/flasher.yml) add the commit, e.g.
# 0.2.0-1a2b3c4: the web installer offers an update when it differs
if(DEFINED ENV{FIRMWARE_VERSION})
    set(PROJECT_VER "$ENV{FIRMWARE_VERSION}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_host_probe)
