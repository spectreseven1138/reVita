#include <vitasdkkern.h>
#include <taihen.h>
#include <stdio.h>
#include <string.h>
#include <psp2/motion.h> 
#include <psp2/touch.h>
#include <psp2kern/kernel/sysmem.h> 

#include "vitasdkext.h"
#include "main.h"
#include "gui/gui.h"
#include "remap.h"
#include "fio/profile.h"
#include "fio/theme.h"
#include "fio/settings.h"
#include "fio/hotkeys.h"
#include "common.h"
#include "sysactions.h"
#include "log.h"
#include "userspace.h"
#include "ds34vita.h"
#include "remapsv.h"

#define INVALID_PID    -1

#define SPACE_KERNEL    1
#define SPACE_USER      0
#define LOGIC_POSITIVE  1
#define LOGIC_NEGATIVE  0
#define TRIGGERS_EXT    1
#define TRIGGERS_NONEXT 0

#define INTERNAL        (666*666)

static tai_hook_ref_t refs[HOOKS_NUM];
static SceUID         hooks[HOOKS_NUM];
static SceUID mutex_procevent_uid = -1;
static SceUID thread_uid = -1;
static bool   thread_run = true;

char titleid[32] = "";
int processid = -1;

bool used_funcs[HOOKS_NUM];
bool ds34vitaRunning;

static uint64_t startTick;
static bool delayedStartDone = false;

SceUID (*_ksceKernelGetProcessMainModule)(SceUID pid);
int (*_ksceKernelGetModuleInfo)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);

int ksceCtrlPeekBufferPositive_internal(int port, SceCtrlData *pad_data, int count){
    pad_data->timeStamp = INTERNAL;
    int ret = ksceCtrlPeekBufferPositive(port, pad_data, count);
    return ret;
}

int ksceTouchPeek_internal(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs){
    pData->status = INTERNAL;
    int ret = ksceTouchPeek(port, pData, nBufs);
    return ret;
}

int nullButtons_user(SceCtrlData *bufs, SceUInt32 nBufs, bool isPositiveLogic){
    SceCtrlData scd;
	memset(&scd, 0, sizeof(scd));
    scd.buttons = isPositiveLogic ? 0 : 0xFFFFFFFF;
    scd.lx = scd.ly = scd.rx = scd.ry = 127;
    for (int i = 0; i < nBufs; i++)
        ksceKernelMemcpyKernelToUser((uintptr_t)&bufs[i], &scd, sizeof(SceCtrlData));
    return nBufs;
}

int nullButtons_kernel(SceCtrlData *bufs, SceUInt32 nBufs, bool isPositiveLogic){
    for (int i = 0; i < nBufs; i++){
        bufs[i].buttons = isPositiveLogic ? 0 : 0xFFFFFFFF;
        bufs[i].lx = bufs[i].ly = bufs[i].rx = bufs[i].ry = 127;
    }
    return nBufs;
}

int onInput(int port, SceCtrlData *ctrl, int nBufs, int hookId, int isKernelSpace, int isPositiveLogic, int isExt){ 
    int ret = nBufs;
    if (ret < 1 || ret > BUFFERS_NUM) 
        return ret;
    if (gui_isOpen) {
        if (isKernelSpace)
            return nullButtons_kernel(ctrl, nBufs, isPositiveLogic);
        else
            return nullButtons_user(ctrl, nBufs, isPositiveLogic);
    }
    if (!settings[SETT_REMAP_ENABLED].v.b) 
        return ret;
    if (!isExt && profile.entries[PR_CO_ENABLED].v.b) 
        return ret;

    used_funcs[hookId] = true;

    SceCtrlData* remappedBuffers;       // Remapped buffers from cache
    if (isKernelSpace) {
        ret = remap_controls(port, &ctrl[ret-1], ret, hookId, &remappedBuffers, isPositiveLogic, isExt);
        memcpy(&ctrl[0], remappedBuffers, ret * sizeof(SceCtrlData));
    } else {
        SceCtrlData scd;                // Last buffer
        ksceKernelMemcpyUserToKernel(&scd, (uintptr_t)&ctrl[ret-1], sizeof(SceCtrlData));
        ret = remap_controls(port, &scd, ret, hookId, &remappedBuffers, isPositiveLogic, isExt);
        ksceKernelMemcpyKernelToUser((uintptr_t)&ctrl[0], remappedBuffers, ret * sizeof(SceCtrlData)); 
    }
    return ret;
}

#define DECL_FUNC_HOOK_PATCH_CTRL(id, name, space, logic, triggers) \
    static int name##_patched(int port, SceCtrlData *ctrl, int nBufs) { \
        if ((space) == SPACE_KERNEL && ctrl->timeStamp == INTERNAL) \
            return TAI_CONTINUE(int, refs[(id)], port, ctrl, nBufs); \
		int ret = TAI_CONTINUE(int, refs[(id)], \
            (port == 1 && profile.entries[PR_CO_EMULATE_DS4].v.b) ? 0 : port, ctrl, nBufs); \
        return onInput(port, ctrl, ret, (id), (space), (logic), (triggers));\
    }
// DECL_FUNC_HOOK_PATCH_CTRL(H_CT_PEEK_P,      sceCtrlPeekBufferPositive,     SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_NONEXT)
// DECL_FUNC_HOOK_PATCH_CTRL(H_CT_READ_P,      sceCtrlReadBufferPositive,     SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_NONEXT)
// DECL_FUNC_HOOK_PATCH_CTRL(H_CT_PEEK_N,      sceCtrlPeekBufferNegative,     SPACE_USER,   LOGIC_NEGATIVE, TRIGGERS_NONEXT)
// DECL_FUNC_HOOK_PATCH_CTRL(H_CT_READ_N,      sceCtrlReadBufferNegative,     SPACE_USER,   LOGIC_NEGATIVE, TRIGGERS_NONEXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_PEEK_P_EXT,  sceCtrlPeekBufferPositiveExt,  SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_NONEXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_READ_P_EXT,  sceCtrlReadBufferPositiveExt,  SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_NONEXT)

DECL_FUNC_HOOK_PATCH_CTRL(H_CT_PEEK_P_2,    sceCtrlPeekBufferPositive2,    SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_EXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_READ_P_2,    sceCtrlReadBufferPositive2,    SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_EXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_PEEK_N_2,    sceCtrlPeekBufferNegative2,    SPACE_USER,   LOGIC_NEGATIVE, TRIGGERS_EXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_READ_N_2,    sceCtrlReadBufferNegative2,    SPACE_USER,   LOGIC_NEGATIVE, TRIGGERS_EXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_PEEK_P_EXT2, sceCtrlPeekBufferPositiveExt2, SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_EXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_CT_READ_P_EXT2, sceCtrlReadBufferPositiveExt2, SPACE_USER,   LOGIC_POSITIVE, TRIGGERS_EXT)

DECL_FUNC_HOOK_PATCH_CTRL(H_K_CT_PEEK_P,    ksceCtrlPeekBufferPositive,    SPACE_KERNEL, LOGIC_POSITIVE, TRIGGERS_NONEXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_K_CT_READ_P,    ksceCtrlReadBufferPositive,    SPACE_KERNEL, LOGIC_POSITIVE, TRIGGERS_NONEXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_K_CT_PEEK_N,    ksceCtrlPeekBufferNegative,    SPACE_KERNEL, LOGIC_NEGATIVE, TRIGGERS_NONEXT)
DECL_FUNC_HOOK_PATCH_CTRL(H_K_CT_READ_N,    ksceCtrlReadBufferNegative,    SPACE_KERNEL, LOGIC_NEGATIVE, TRIGGERS_NONEXT)

int nullTouch_kernel(SceTouchData *pData, SceUInt32 nBufs){
    pData[0] = pData[nBufs - 1];
    pData[0].reportNum = 0;
    return 1;
}

int nullTouch_user(SceTouchData *pData, SceUInt32 nBufs){
    SceUInt32 reportsNum = 0;
    ksceKernelMemcpyKernelToUser((uintptr_t)&pData[0].reportNum, &reportsNum, sizeof(SceUInt32));
    return 1;
}

int onTouch(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, uint8_t hookIdx, int isKernelSpace){
    int ret = nBufs;
    
    if (nBufs < 1 || nBufs > BUFFERS_NUM) 
        return nBufs;

    //Nullify input calls when UI is open
	if (gui_isOpen) {
        if (isKernelSpace)
            return nullTouch_kernel(pData, nBufs);
        else 
            return nullTouch_user(pData, nBufs);
	}
    
	if (!settings[SETT_REMAP_ENABLED].v.b) return nBufs;

    SceTouchData* remappedBuffers; 
    if (isKernelSpace){
        ret = remap_touch(port, &pData[nBufs - 1], nBufs, hookIdx, &remappedBuffers);
        memcpy(&pData[0], remappedBuffers, ret * sizeof(SceTouchData));
    } else {
        SceTouchData std;                // Last buffer
        ksceKernelMemcpyUserToKernel(&std, (uintptr_t)&pData[ret-1], sizeof(SceTouchData));
        ret = remap_touch(port, &std, nBufs, hookIdx, &remappedBuffers);
        ksceKernelMemcpyKernelToUser((uintptr_t)&pData[0], remappedBuffers, ret * sizeof(SceTouchData)); 
    }
    return ret;
}

void scaleTouchData(int port, SceTouchData *pData){
    for (int idx = 0; idx < pData->reportNum; idx++)
        pData->report[idx].y = 
            (pData->report[idx].y - T_SIZE[!port].a.y) 
            * (T_SIZE[port].b.y - T_SIZE[port].a.y) 
            / (T_SIZE[!port].b.y - T_SIZE[!port].a.y) 
            + T_SIZE[port].a.y;
}

#define DECL_FUNC_HOOK_PATCH_TOUCH(index, name, space) \
    static int name##_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs) { \
        if (pData->status == INTERNAL) \
            return TAI_CONTINUE(int, refs[(index)], port, pData, nBufs); \
        if (profile.entries[PR_TO_SWAP].v.b) \
            port = !port; \
		int ret = TAI_CONTINUE(int, refs[(index)], port, pData, nBufs); \
        if (profile.entries[PR_TO_SWAP].v.b && ret > 0 && ret < 64) \
            scaleTouchData(!port, &pData[ret - 1]); \
        used_funcs[(index)] = true; \
        return onTouch(port, pData, ret, (index) - H_K_TO_PEEK, (space)); \
    }
/*#define DECL_FUNC_HOOK_PATCH_TOUCH_REGION(index, name, space) \
    static int name##_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region) { \
        if (isInternalTouchCall) \
            return TAI_CONTINUE(int, refs[(index)], port, pData, nBufs, region); \
        if (profile.entries[PR_TO_SWAP].v.b) \
            port = !port; \
		int ret = TAI_CONTINUE(int, refs[(index)], port, pData, nBufs, region); \
        if (region != 1) return ret; \
        used_funcs[(index)] = true; \
        return onTouch(port, pData, ret, (index) - H_K_TO_PEEK, (space)); \
    }*/
DECL_FUNC_HOOK_PATCH_TOUCH(H_K_TO_PEEK, ksceTouchPeek, SPACE_KERNEL)
DECL_FUNC_HOOK_PATCH_TOUCH(H_K_TO_READ, ksceTouchRead, SPACE_KERNEL)
// DECL_FUNC_HOOK_PATCH_TOUCH_REGION(H_K_TO_PEEK_R, ksceTouchPeekRegion, SPACE_KERNEL)
// DECL_FUNC_HOOK_PATCH_TOUCH_REGION(H_K_TO_READ_R, ksceTouchReadRegion, SPACE_KERNEL)

int ksceCtrlGetControllerPortInfo_patched(SceCtrlPortInfo* info){
    int ret = TAI_CONTINUE(int, refs[H_K_CT_PORT_INFO], info);
    if (profile.entries[PR_CO_EMULATE_DS4].v.b){
        info->port[1] = SCE_CTRL_TYPE_DS4;
    }
	return ret;
}

int ksceDisplaySetFrameBufInternal_patched(int head, int index, const SceDisplayFrameBuf *pParam, int sync) {
    used_funcs[H_K_DISP_SET_FB] = 1;

    if (index && ksceAppMgrIsExclusiveProcessRunning())
        goto DISPLAY_HOOK_RET; // Do not draw over SceShell overlay

    if (pParam)  
        gui_draw(pParam);

    if (!head || !pParam)
        goto DISPLAY_HOOK_RET;

    if (gui_isOpen) {
        SceCtrlData ctrl;
        
        if(ksceCtrlPeekBufferPositive_internal(0, &ctrl, 1) == 1)
            gui_onInput(&ctrl);
    }

DISPLAY_HOOK_RET:
    return TAI_CONTINUE(int, refs[H_K_DISP_SET_FB], head, index, pParam, sync);
}

void changeActiveApp(char* tId, int pid){
    if (!streq(titleid, tId)) {
        strnclone(titleid, tId, sizeof(titleid));
        processid = pid;
        for (int i = 0; i < HOOKS_NUM; i++)
            used_funcs[i] = false;
        profile_load(titleid);
        remap_resetBuffers();
        gui_close();
        delayedStartDone = false;
    }
}

int ksceKernelInvokeProcEventHandler_patched(int pid, int ev, int a3, int a4, int *a5, int a6) {
    used_funcs[H_K_INV_PROC_EV_HANDLER] = 1;
    char titleidLocal[sizeof(titleid)];
    int ret = ksceKernelLockMutex(mutex_procevent_uid, 1, NULL);
    if (ret < 0)
        goto PROCEVENT_EXIT;
    ksceKernelGetProcessTitleId(pid, titleidLocal, sizeof(titleidLocal));
    if (streq(titleidLocal, "main"))
        strnclone(titleidLocal, HOME, sizeof(titleidLocal));
    switch (ev) {
        case 1: //Start
        case 5: //Resume
            if (STREQANY(titleidLocal,  // If test app
                    "TSTCTRL00", "TSTCTRL20", "TSTCTRLE0", "TSTCTRLE2", "TSTCTRLN0", "TSTCTRLN2", "VSDK00019"))
                break;                  // Use MAIN profile
                
            if (strStartsWith(titleidLocal, "NPXS")){ //If system app
                SceKernelModuleInfo info;
                info.size = sizeof(SceKernelModuleInfo);
                _ksceKernelGetModuleInfo(pid, _ksceKernelGetProcessMainModule(pid), &info);
                if(!streq(info.module_name, "ScePspemu") &&  // If Not Adrenaline
			        !STREQANY(titleidLocal, "NPXS10012",     //        PS3Link
                                            "NPXS10013"))    //        PS4Link
                        break;                               // Use MAIN profile
            }
            changeActiveApp(titleidLocal, pid);
            break;
        case 3: //Close
        case 4: //Suspend
            if (streq(titleid, titleidLocal)){ //If current app suspended
                changeActiveApp(HOME, pid);
            }
            break;
        default:
            break;
    }

    ksceKernelUnlockMutex(mutex_procevent_uid, 1);

PROCEVENT_EXIT:
    return TAI_CONTINUE(int, refs[H_K_INV_PROC_EV_HANDLER], pid, ev, a3, a4, a5, a6);
}

static int main_thread(SceSize args, void *argp) {
    uint32_t oldBtns = 0;
    while (thread_run) {
        //Activate delayed start
        if (!delayedStartDone 
            && startTick + settings[SETT_DELAY_INIT].v.u * 1000000 < ksceKernelGetSystemTimeWide()){
            remap_setup();
	        delayedStartDone = true;
        }

        SceCtrlData ctrl;
        if(ksceCtrlPeekBufferPositive_internal(0, &ctrl, 1) != 1){
            ksceKernelDelayThread(30 * 1000);
            continue;
        }
        remap_swapSideButtons(&ctrl.buttons);

        if (!gui_isOpen){
            for (int i = 0; i < HOTKEY__NUM; i++){
                if (hotkeys[i].v.u != 0 && 
                        btn_has(ctrl.buttons, hotkeys[i].v.u) && 
                        !btn_has(oldBtns, hotkeys[i].v.u)){
                    switch(i){
                        case HOTKEY_MENU:
                            gui_open();
                            remap_resetBuffers();
                            break;
                        case HOTKEY_REMAPS_TOOGLE: FLIP(settings[SETT_REMAP_ENABLED].v.b); break;
                        case HOTKEY_RESET_SOFT: sysactions_softReset();  break;
                        case HOTKEY_RESET_COLD: sysactions_coldReset();  break;
                        case HOTKEY_STANDBY: sysactions_standby();  break;
                        case HOTKEY_SUSPEND: sysactions_suspend();  break;
                        case HOTKEY_DISPLAY_OFF: sysactions_displayOff();  break;
                        case HOTKEY_KILL_APP: sysactions_killCurrentApp();  break;
                    }
                }
            }
        }

        oldBtns = ctrl.buttons;
       
        ksceKernelDelayThread(30 * 1000);
    }
    return 0;
}

// Check if ds34vita running and set appropriate functions
void initDs34vita(){
    if (!ds34vitaRunning){
        LOG("initDs34vita();\n");
        tai_module_info_t info;
        info.size = sizeof(tai_module_info_t);
        ds34vitaRunning = taiGetModuleInfoForKernel(KERNEL_PID, "ds34vita", &info) == 0;

    }
}

//Sync ds4vita config
void syncDS34Vita(){
    LOG("syncDS34Vita()\n");
    initDs34vita();
    if (!ds34vitaRunning)
        return;
    ds34vita_setIsPort1Allowed(!profile.entries[PR_CO_EMULATE_DS4].v.b);
    LOG("ds34vita_setIsPort1Allowed(%i)", !profile.entries[PR_CO_EMULATE_DS4].v.b);
}
//Sync configurations across other plugins
void sync(){
    syncDS34Vita();
}

void hookE(uint8_t hookId, const char *module, uint32_t library_nid, uint32_t func_nid, const void *func) {
    hooks[hookId] = taiHookFunctionExportForKernel(KERNEL_PID, &refs[hookId], module, library_nid, func_nid, func);
}
void hookI(uint8_t hookId, const char *module, uint32_t library_nid, uint32_t func_nid, const void *func) {
    hooks[hookId] = taiHookFunctionImportForKernel(KERNEL_PID, &refs[hookId], module, library_nid, func_nid, func);
}
void exportF(const char *module, uint32_t library_nid_360, uint32_t func_nid_360, 
        uint32_t library_nid_365, uint32_t func_nid_365, const void *func){
    if (module_get_export_func(KERNEL_PID, module, library_nid_360, func_nid_360, (uintptr_t*)func) < 0) // 3.60
        module_get_export_func(KERNEL_PID, module, library_nid_365, func_nid_365, (uintptr_t*)func);     // 3.65
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
    LOG("Plugin started\n");

    snprintf(titleid, sizeof(titleid), HOME);
    settings_init();
    hotkeys_init();
    theme_init();
    profile_init();
    gui_init();
    remap_init();
    userspace_init();
    startTick = ksceKernelGetSystemTimeWide();
	theme_load(settings[SETT_THEME].v.u);

    mutex_procevent_uid = ksceKernelCreateMutex("remaPSV2_mutex_procevent", 0, 0, NULL);

	// PS TV uses SceTouchDummy instead of SceTouch
    tai_module_info_t modInfo;
	modInfo.size = sizeof(modInfo);
	int ret = taiGetModuleInfoForKernel(KERNEL_PID, "SceTouch", &modInfo);
	char* sceTouchModuleName = (ret >= 0) ? "SceTouch" : "SceTouchDummy";

    // Hooking functions
    for (int i = 0; i < HOOKS_NUM; i++)
        hooks[i] = 0;
    // hookE(H_CT_PEEK_P,     "SceCtrl", 0xD197E3C7, 0xA9C3CED6, sceCtrlPeekBufferPositive_patched);
    // hookE(H_CT_READ_P,     "SceCtrl", 0xD197E3C7, 0x67E7AB83, sceCtrlReadBufferPositive_patched);
    // hookE(H_CT_PEEK_N,     "SceCtrl", 0xD197E3C7, 0x104ED1A7, sceCtrlPeekBufferNegative_patched);
    // hookE(H_CT_READ_N,     "SceCtrl", 0xD197E3C7, 0x15F96FB0, sceCtrlReadBufferNegative_patched);
	hookE(H_CT_PEEK_P_EXT, "SceCtrl", 0xD197E3C7, 0xA59454D3, sceCtrlPeekBufferPositiveExt_patched);
    hookE(H_CT_READ_P_EXT, "SceCtrl", 0xD197E3C7, 0xE2D99296, sceCtrlReadBufferPositiveExt_patched);

    hookE(H_CT_PEEK_P_2,   "SceCtrl", 0xD197E3C7, 0x15F81E8C, sceCtrlPeekBufferPositive2_patched);
    hookE(H_CT_READ_P_2,   "SceCtrl", 0xD197E3C7, 0xC4226A3E, sceCtrlReadBufferPositive2_patched);
    hookE(H_CT_PEEK_N_2,   "SceCtrl", 0xD197E3C7, 0x81A89660, sceCtrlPeekBufferNegative2_patched);
    hookE(H_CT_READ_N_2,   "SceCtrl", 0xD197E3C7, 0x27A0C5FB, sceCtrlReadBufferNegative2_patched);
    hookE(H_CT_PEEK_P_EXT2,"SceCtrl", 0xD197E3C7, 0x860BF292, sceCtrlPeekBufferPositiveExt2_patched);
    hookE(H_CT_READ_P_EXT2,"SceCtrl", 0xD197E3C7, 0xA7178860, sceCtrlReadBufferPositiveExt2_patched);
    
    hookE(H_K_CT_PEEK_P,   "SceCtrl", 0x7823A5D1, 0xEA1D3A34, ksceCtrlPeekBufferPositive_patched);
    hookE(H_K_CT_READ_P,   "SceCtrl", 0x7823A5D1, 0x9B96A1AA, ksceCtrlReadBufferPositive_patched);
    hookE(H_K_CT_PEEK_N,   "SceCtrl", 0x7823A5D1, 0x19895843, ksceCtrlPeekBufferNegative_patched);
    hookE(H_K_CT_READ_N,   "SceCtrl", 0x7823A5D1, 0x8D4E0DD1, ksceCtrlReadBufferNegative_patched);

    hookE(H_K_TO_PEEK, sceTouchModuleName, TAI_ANY_LIBRARY, 0xBAD1960B, ksceTouchPeek_patched);
    hookE(H_K_TO_READ, sceTouchModuleName, TAI_ANY_LIBRARY, 0x70C8AACE, ksceTouchRead_patched);
    // hookE(H_K_TO_PEEK_R, sceTouchModuleName, TAI_ANY_LIBRARY, 0x9B3F7207, ksceTouchPeekRegion_patched);
    // hookE(H_K_TO_READ_R, sceTouchModuleName, TAI_ANY_LIBRARY, 0x9A91F624, ksceTouchReadRegion_patched);

	hookE(H_K_CT_PORT_INFO,        "SceCtrl",       TAI_ANY_LIBRARY, 0xF11D0D30, ksceCtrlGetControllerPortInfo_patched);
	hookE(H_K_DISP_SET_FB,         "SceDisplay",    0x9FED47AC,      0x16466675, ksceDisplaySetFrameBufInternal_patched);
	hookI(H_K_INV_PROC_EV_HANDLER, "SceProcessmgr", 0x887F19D0,      0x414CC813, ksceKernelInvokeProcEventHandler_patched);

    exportF("SceKernelModulemgr", 0xC445FA63, 0x20A27FA9, 0x92C9FFC2, 0x679F5144, &_ksceKernelGetProcessMainModule);
    exportF("SceKernelModulemgr", 0xC445FA63, 0xD269F915, 0x92C9FFC2, 0xDAA90093, &_ksceKernelGetModuleInfo);

    thread_uid = ksceKernelCreateThread("remaPSV2_thread", main_thread, 0x3C, 0x3000, 0, 0x10000, 0);
    ksceKernelStartThread(thread_uid, 0, NULL);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    if (thread_uid >= 0) {
        thread_run = 0;
        ksceKernelWaitThreadEnd(thread_uid, NULL, NULL);
        ksceKernelDeleteThread(thread_uid);
    }

    for (int i = 0; i < HOOKS_NUM; i++) {
        if (hooks[i] >= 0)
            taiHookReleaseForKernel(hooks[i], refs[i]);
    }

    if (mutex_procevent_uid >= 0)
        ksceKernelDeleteMutex(mutex_procevent_uid);

    settings_destroy();
    hotkeys_destroy();
    theme_destroy();
    profile_destroy();
    gui_destroy();
    remap_destroy();
    userspace_destroy();
    return SCE_KERNEL_STOP_SUCCESS;
}
