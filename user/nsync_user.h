/* nsync_user.h: what the app calls (nsync_user.suprx, loaded after the kernel module) */
#pragma once
#include "../kernel/nsync_kernel.h"
int nsyncUserMountSave(const char *save_titleid, const char *caller_titleid, char *mount_point);
int nsyncUserFirmware(char *name);
int nsyncUserServiceAlive(void);
