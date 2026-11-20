/* nsync_kernel: Nativ Sync's kernel module. Two jobs: mounting a game's save decrypted for the calling process (here),
 * and the launch guard (guard.c).
 *
 * The mount works the way VitaShell and Vita Save Manager do it. The Vita's own file system driver then decrypts what the process reads through the
 * save's ordinary path (ux0:user/00/savedata/<TITLEID>/...) and re-encrypts what it writes, with the console's own key:
 * nothing here touches a key.
 *
 * How: SceAppMgr's internal MountById, found at a known offset per firmware (VitaShell's table, modules/kernel/main.c,
 * GPL-3.0, TheOfficialFloW), with mount id 0x12E and the caller's own title id. On firmware 3.65, id 0x3ED (the one
 * VitaShell's notes name for savedata) fails from an app and HANGS the shell; 0x12E works from both. A firmware this
 * doesn't know is refused, never guessed: its function pointers would be garbage. No network, no parsing: the plugin
 * and the app do that in user mode. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <taihen.h>
#include "nsync_kernel.h"

int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr);
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

typedef struct { uint32_t nid, find_off, mount_off; const char *fw; } Firmware;
static const Firmware FIRMWARES[] = {   /* SceAppMgr's module NID -> its FindProcessInfoByPid and MountById offsets */
	{ 0x94CEFE4B, 0x2DE1, 0x19E15, "3.55" }, { 0xDFBC288C, 0x2DE1, 0x19E15, "3.57" }, { 0xDBB29DB7, 0x2DE1, 0x19B51, "3.60" },
	{ 0x1C9879D6, 0x2DE1, 0x19E61, "3.65" }, { 0x54E2E984, 0x2DE1, 0x19E6D, "3.67" }, { 0xC3C538DE, 0x2DE1, 0x19E6D, "3.68" },
	{ 0x321E4852, 0x2DE9, 0x19E95, "3.69" }, { 0x700DA0CD, 0x2DE9, 0x19E95, "3.70" }, { 0xF7846B4E, 0x2DE9, 0x19E95, "3.71" },
	{ 0xA8E80BA8, 0x2DE9, 0x19E95, "3.72" }, { 0xB299D195, 0x2DE9, 0x19E95, "3.73" },
	{ 0x30007BD3, 0x2DE9, 0x19E95, "3.74" },   /* (RealYoti's VitaShell; checked against 3.74's bootimage: SceAppMgr's NID, and both offsets start a function) */
};
static const Firmware *firmware(SceUID *modid) {
	tai_module_info_t ti; ti.size = sizeof ti;
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceAppMgr", &ti) < 0) return 0;
	if (modid) *modid = ti.modid;
	for (unsigned i = 0; i < sizeof FIRMWARES / sizeof *FIRMWARES; i++) if (FIRMWARES[i].nid == ti.module_nid) return &FIRMWARES[i];
	return 0;
}

typedef struct { char path[64], caller[12], mount_point[16]; } Req;
static int mount_k(Req *r) {
	SceUID modid; const Firmware *fw = firmware(&modid);
	if (!fw) return NSYNC_ERROR_FIRMWARE;
	void *(*find_info)(void *data, SceUID pid) = 0;
	int (*mount_by_id)(SceUID pid, void *info, int id, const char *titleid, const char *path, const char *desired, const void *klicensee, char *mount_point) = 0;
	if (module_get_offset(KERNEL_PID, modid, 0, fw->find_off, (uintptr_t *)&find_info) < 0 || !find_info) return NSYNC_ERROR_FIRMWARE;
	if (module_get_offset(KERNEL_PID, modid, 0, fw->mount_off, (uintptr_t *)&mount_by_id) < 0 || !mount_by_id) return NSYNC_ERROR_FIRMWARE;
	int (*get_module_info)(SceUID pid, SceUID modid, SceKernelModuleInfo *info) = 0;
	int res = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", 0xC445FA63, 0xD269F915, (uintptr_t *)&get_module_info);
	if (res < 0) res = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", 0x92C9FFC2, 0xDAA90093, (uintptr_t *)&get_module_info);
	if (res < 0) return NSYNC_ERROR_FIRMWARE;
	SceKernelModuleInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi;
	if ((res = get_module_info(KERNEL_PID, modid, &mi)) < 0) return res;
	SceUID pid = ksceKernelGetProcessId();
	void *info = find_info((void *)((uint32_t)mi.segments[1].vaddr + 0x500), pid);
	if (!info) return NSYNC_ERROR_PROCESS;
	char klicensee[16]; memset(klicensee, 0, sizeof klicensee);
	memset(r->mount_point, 0, sizeof r->mount_point);
	return mount_by_id(pid, (char *)info + 0x580, 0x12E, r->caller, r->path, NULL, klicensee, r->mount_point);
}
static int valid_id(const char *t) {   /* four capitals and five digits: a title id, nothing else reaches the path */
	for (int i = 0; i < 9; i++) if (i < 4 ? !(t[i] >= 'A' && t[i] <= 'Z') : !(t[i] >= '0' && t[i] <= '9')) return 0;
	return t[9] == 0;
}

int nsyncMountSave(const char *save_titleid, const char *caller_titleid, char *mount_point) {
	uint32_t state; ENTER_SYSCALL(state);
	Req r; memset(&r, 0, sizeof r); char t[12]; memset(t, 0, sizeof t);
	ksceKernelStrncpyFromUser(t, save_titleid, 10);
	ksceKernelStrncpyFromUser(r.caller, caller_titleid, 10);
	int res = NSYNC_ERROR_ARGUMENT;
	if (valid_id(t) && valid_id(r.caller)) {
		memcpy(r.path, "ux0:user/00/savedata/", 21); memcpy(r.path + 21, t, 10);
		res = ksceKernelRunWithStack(0x2000, (void *)mount_k, &r);
		ksceKernelCopyToUser(mount_point, r.mount_point, sizeof r.mount_point);
	}
	EXIT_SYSCALL(state);
	return res;
}
int nsyncFirmware(char *name) {   /* 1 and the firmware's name (e.g. "3.65") when this module knows it, else 0 */
	uint32_t state; ENTER_SYSCALL(state);
	const Firmware *fw = firmware(0);
	if (fw && name) ksceKernelCopyToUser(name, fw->fw, 5);
	EXIT_SYSCALL(state);
	return fw != 0;
}

int guard_start(void); void guard_stop(void);
/* boot safety: starts not yet confirmed healthy, counted in ur0:data/nativ-sync/health.txt (the service writes 0 after a
 * healthy minute). From the second unconfirmed boot in a row the launch hook stays off; from the third the loader
 * doesn't start the service either (shell/boot.c), and the Vita boots as if Nativ Sync weren't installed until the app
 * turns it back on. The app loading this module itself (argp "app") isn't a boot. */
#define HEALTH "ur0:data/nativ-sync/health.txt"
static int health_bump(void) {
	char b[16]; int n = 0; memset(b, 0, sizeof b);
	SceUID f = ksceIoOpen(HEALTH, SCE_O_RDONLY, 0);
	if (f >= 0) { ksceIoRead(f, b, sizeof b - 1); ksceIoClose(f); }
	for (char *c = b; *c >= '0' && *c <= '9'; c++) n = n * 10 + (*c - '0');
	ksceIoMkdir("ur0:data", 0777); ksceIoMkdir("ur0:data/nativ-sync", 0777);
	f = ksceIoOpen(HEALTH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (f >= 0) { char o[16]; int l = snprintf(o, sizeof o, "%d\n", n + 1); ksceIoWrite(f, o, l); ksceIoClose(f); }
	return n;
}
void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	int from_app = args == 4 && argp && !memcmp(argp, "app", 4);
	if (from_app || health_bump() < 2) guard_start();   /* (the mount works without the hook) */
	return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp) { guard_stop(); return SCE_KERNEL_STOP_SUCCESS; }
