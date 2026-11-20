/* nsync_user: the app's bridge to the kernel module. An app's imports are bound when it starts, and the kernel module is
 * loaded by the app afterwards, so the app can't import the kernel's syscalls itself (its calls would jump to an unbound
 * stub: a crash). This module is loaded after the kernel module, binds to it then, and the app calls it through weak
 * imports: the arrangement VitaShell's user.suprx has. (The shell plugin needs none: taiHEN loads the kernel module first.) */
#include <psp2/kernel/modulemgr.h>
#include "../kernel/nsync_kernel.h"
int nsyncUserMountSave(const char *save_titleid, const char *caller_titleid, char *mount_point) { return nsyncMountSave(save_titleid, caller_titleid, mount_point); }
int nsyncUserFirmware(char *name) { return nsyncFirmware(name); }
void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize args, void *argp) { return SCE_KERNEL_START_SUCCESS; }
int module_stop(SceSize args, void *argp) { return SCE_KERNEL_STOP_SUCCESS; }
