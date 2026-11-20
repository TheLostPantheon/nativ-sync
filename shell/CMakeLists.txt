# nsync_service.suprx: the background service, a plugin in SceShell (taiHEN's *main section)
add_executable(nsync_service service.c saves.c ${CMAKE_SOURCE_DIR}/common/nsync_proto.c ${CMAKE_SOURCE_DIR}/third_party/monocypher/monocypher.c)
add_dependencies(nsync_service nsync_kernel_stubs)
target_include_directories(nsync_service PRIVATE ${CMAKE_SOURCE_DIR}/kernel ${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/third_party/monocypher)
target_link_directories(nsync_service PRIVATE ${CMAKE_BINARY_DIR}/kernel/nsync_kernel_stubs)
set_target_properties(nsync_service PROPERTIES COMPILE_FLAGS "-Wl,-q -Wall -O2 -std=gnu99 -fno-builtin" LINK_FLAGS "-nostdlib")
target_link_libraries(nsync_service NativSyncKernel_stub SceLibKernel_stub SceKernelThreadMgr_stub SceSysmem_stub SceIofilemgr_stub
  SceNet_stub SceNetCtl_stub SceAppMgr_stub SceRegistryMgr_stub SceRtc_stub gcc)
vita_create_self(nsync_service.suprx nsync_service CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml UNSAFE)

# nsync_boot.suprx: what the config loads at boot; it loads the service once the shell's network libraries are there
add_executable(nsync_boot boot.c)
set_target_properties(nsync_boot PROPERTIES COMPILE_FLAGS "-Wl,-q -Wall -O2 -std=gnu99" LINK_FLAGS "-nostdlib")
target_link_libraries(nsync_boot SceLibKernel_stub SceKernelThreadMgr_stub SceIofilemgr_stub gcc)
vita_create_self(nsync_boot.suprx nsync_boot CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/boot_exports.yml UNSAFE)
