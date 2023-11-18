#include "Logger.h"
#include "String.h"
#include "Platform.h"
#include "Window.h"
#include "Constants.h"
#include "Game.h"
#include "Funcs.h"
#include "Utils.h"
#include "Server.h"
#include "Options.h"

static void RunGame(void) {
	cc_string title; char titleBuffer[STRING_SIZE];
	int width  = Options_GetInt(OPT_WINDOW_WIDTH,  0, DisplayInfo.Width,  0);
	int height = Options_GetInt(OPT_WINDOW_HEIGHT, 0, DisplayInfo.Height, 0);

	/* No custom resolution has been set */
	if (width == 0 || height == 0) {
		width = 854; height = 480;
		if (DisplayInfo.Width < 854) width = 640;
	}

	String_InitArray(title, titleBuffer);
	String_Format1(&title, "%c", GAME_APP_TITLE);
	Game_Run(width, height, &title);
}

// Enable some nice features
CC_NOINLINE static void ChooseMode_Click(cc_bool classic, cc_bool classicHacks) {
	Options_SetBool(OPT_CLASSIC_MODE, classic);
	if (classic) Options_SetBool(OPT_CLASSIC_HACKS, classicHacks);

	Options_SetBool(OPT_CUSTOM_BLOCKS, !classic);
	Options_SetBool(OPT_CPE, !classic);
	Options_SetBool(OPT_SERVER_TEXTURES, !classic);
	Options_SetBool(OPT_CLASSIC_TABLIST, classic);
	Options_SetBool(OPT_CLASSIC_OPTIONS, classic);

	Options_SaveIfChanged();
}


static void SetupProgram(int argc, char** argv) {
	static char ipBuffer[STRING_SIZE];
	cc_result res;
	Logger_Hook();
	Platform_Init();
	res = Platform_SetDefaultCurrentDirectory(argc, argv);

	Options_Load();
	Window_Init();
	
	if (res) Logger_SysWarn(res, "setting current directory");
	Platform_LogConst("Starting " GAME_APP_NAME " ..");
	String_InitArray(Server.Address, ipBuffer);
}

static int RunProgram() {
	String_FromReadonly(&Game_Username, "Singleplayer");
	ChooseMode_Click(false, false);
	RunGame();
	return 0;
}

int main(int argc, char** argv) {
	cc_result res;
	SetupProgram(argc, argv);

	res = RunProgram(argc, argv);
	Process_Exit(res);
	return res;
}
