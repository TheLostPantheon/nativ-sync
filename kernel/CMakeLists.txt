add_executable(nsync_kernel nsync_kernel.c)
set_target_properties(nsync_kernel PROPERTIES COMPILE_FLAGS "-Wl,-q -Wall -O2 -std=gnu99" LINK_FLAGS "-nostdlib")
target_link_libraries(nsync_kernel SceSysclibForDriver_stub SceSysmemForDriver_stub SceModulemgrForDriver_stub SceThreadmgrForDriver_stub
  SceProcessmgrForDriver_stub taihenForKernel_stub taihenModuleUtils_stub gcc)
vita_create_self(nsync_kernel.skprx nsync_kernel CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml UNSAFE)
vita_create_stubs(nsync_kernel_stubs nsync_kernel ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml KERNEL)
