/* nsync_kernel.h: Nativ Sync's kernel module, as the app and the shell plugin call it (syscalls). */
#pragma once
#define NSYNC_ERROR_FIRMWARE 0x80F10001   /* a firmware this module has no table entry for */
#define NSYNC_ERROR_PROCESS  0x80F10002   /* SceAppMgr knows no such process */
#define NSYNC_ERROR_ARGUMENT 0x80F10003   /* not a title id */
/* mount <save_titleid>'s save decrypted for the calling process, which then reads and writes its plain files through
 * ux0:user/00/savedata/<save_titleid>/; caller_titleid is the caller's own (an app's, or NPXS19999 for the shell);
 * mount_point (16 bytes) receives what sceAppMgrUmount takes afterwards */
int nsyncMountSave(const char *save_titleid, const char *caller_titleid, char *mount_point);
int nsyncFirmware(char *name);   /* 1 and the firmware's name (6 bytes) when supported */
