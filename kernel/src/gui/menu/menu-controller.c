#include <vitasdkkern.h>
#include "../../main.h"
#include "../../common.h"
#include "../../fio/settings.h"
#include "../../fio/theme.h"
#include "../gui.h"
#include "../renderer.h"

char* getControllerName(int id){
	if 		(id == 	SCE_CTRL_TYPE_UNPAIRED) return "Unpaired";
	else if (id == 	SCE_CTRL_TYPE_PHY) 		return "Physical VITA";
	else if (id == 	SCE_CTRL_TYPE_VIRT) 	return "Virtual PSTV";
	else if (id == 	SCE_CTRL_TYPE_DS3) 		return "DualShock 3";
	else if (id == 	SCE_CTRL_TYPE_DS4) 		return "DualShock 4";
	else 									return "Unknown";
}

void onButton_controller(uint32_t btn){
	switch (btn) {
		case SCE_CTRL_START: profile_resetController(); break;
		default: onButton_genericEntries(btn); break;
	}
}

void onDraw_controller(uint menuY){
    int y = menuY;
	SceCtrlPortInfo pi;
	int res = ksceCtrlGetControllerPortInfo(&pi);
	if (res != 0){//Should not ever trigger
		renderer_setColor(theme[COLOR_DANGER]);
		renderer_drawString(L_1, y+= CHA_H, "Error getting controllers info");
		return;
	}
	
	int ii = gui_calcStartingIndex(gui_menu->idx, gui_menu->num , gui_lines, BOTTOM_OFFSET);
	for (int i = ii; i < min(ii + gui_lines, gui_menu->num); i++) {		
		gui_drawEntry(L_1, y+= CHA_H, &gui_menu->entries[i], gui_menu->idx == i);
	}

	//Ports stats
	y+=CHA_H;
	renderer_setColor(theme[COLOR_HEADER]);
	renderer_drawString(L_1, y+= CHA_H, "Detected controllers:");
	for (int i = 0; i < 5; i++){
		renderer_setColor((pi.port[i] == SCE_CTRL_TYPE_UNPAIRED) ? theme[COLOR_DANGER] : theme[COLOR_ACTIVE]);
		renderer_drawStringF(L_2, y += CHA_H, "Port %i: %s", i, getControllerName(pi.port[i]));
	}
}

static struct MenuEntry menu_controllers_entries[] = {
	(MenuEntry){.name = "Force support for $,$.$($) ",  .dataPE = &profile.entries[PR_CO_PATCH_EXT]},
	(MenuEntry){.name = "Swap $[$]<>${$}", 				.dataPE = &profile.entries[PR_CO_SWAP_BUTTONS]},
	(MenuEntry){.name = "Vita as virtual DS4", 			.dataPE = &profile.entries[PR_CO_EMULATE_DS4]}};
static struct Menu menu_controller = (Menu){
	.id = MENU_CONTROLLER_ID, 
	.parent = MENU_MAIN_ID,
	.name = "$t CONTROLLER", 
	.footer = "$SRESET  $:RESET ALL",
	.onButton = onButton_controller,
	.onDraw = onDraw_controller,
	.num = SIZE(menu_controllers_entries), 
	.entries = menu_controllers_entries};

void menu_initController(){
	gui_registerMenu(&menu_controller);
}