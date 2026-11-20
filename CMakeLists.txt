cmake_minimum_required(VERSION 3.5)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(nativ_sync C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Nativ Sync")
set(VITA_TITLEID "NSYN00001")   # (provisional: checked against VitaDB before a release)
set(VITA_VERSION "00.01")

add_subdirectory(kernel)
add_subdirectory(user)

add_executable(nativ_sync app/main.c)
add_dependencies(nativ_sync nsync_user_stubs)
target_link_directories(nativ_sync PRIVATE ${CMAKE_BINARY_DIR}/user/nsync_user_stubs)
target_compile_options(nativ_sync PRIVATE -Wall -O2)
target_link_libraries(nativ_sync NativSyncUser_stub_weak taihen_stub vita2d SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub
  ScePgf_stub ScePvf_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub freetype png jpeg z m c)
vita_create_self(eboot.bin nativ_sync UNSAFE)
vita_create_vpk(nativ_sync.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
  FILE ${CMAKE_BINARY_DIR}/kernel/nsync_kernel.skprx nsync_kernel.skprx
  FILE ${CMAKE_BINARY_DIR}/user/nsync_user.suprx nsync_user.suprx)
add_dependencies(nativ_sync.vpk-vpk nsync_kernel.skprx-self nsync_user.suprx-self)   # (the modules go in the vpk: made first)
