#include <assert.h>
#include "Server.h"
#include "String.h"
#include "BlockPhysics.h"
#include "Game.h"
#include "Drawer2D.h"
#include "Chat.h"
#include "Block.h"
#include "Event.h"
#include "Http.h"
#include "Funcs.h"
#include "Entity.h"
#include "Graphics.h"
#include "Gui.h"
#include "Screens.h"
#include "Formats.h"
#include "Generator.h"
#include "World.h"
#include "Camera.h"
#include "TexturePack.h"
#include "Menus.h"
#include "Logger.h"
#include "Protocol.h"
#include "Inventory.h"
#include "Platform.h"
#include "Input.h"
#include "Errors.h"
#include "Options.h"

static char nameBuffer[STRING_SIZE];
static char motdBuffer[STRING_SIZE];
static char appBuffer[STRING_SIZE];
static int ticks;
struct _ServerConnectionData Server;

/*########################################################################################################################*
*-----------------------------------------------------Common handlers-----------------------------------------------------*
*#########################################################################################################################*/
static void Server_ResetState(void) {
	Server.Disconnected            = false;
	Server.SupportsExtPlayerList   = false;
	Server.SupportsPlayerClick     = false;
	Server.SupportsPartialMessages = false;
	Server.SupportsFullCP437       = false;
}

void Server_RetrieveTexturePack(const cc_string* url) {
	if (!Game_AllowServerTextures || TextureCache_HasDenied(url)) return;

	if (!url->length || TextureCache_HasAccepted(url)) {
		TexturePack_Extract(url);
	} else {
		TexPackOverlay_Show(url);
	}
}


/*########################################################################################################################*
*--------------------------------------------------------PingList---------------------------------------------------------*
*#########################################################################################################################*/
struct PingEntry { cc_int64 sent, recv; cc_uint16 id; };
static struct PingEntry ping_entries[10];
static int ping_head;

int Ping_NextPingId(void) {
	int head = ping_head;
	int next = ping_entries[head].id + 1;

	head = (head + 1) % Array_Elems(ping_entries);
	ping_entries[head].id   = next;
	ping_entries[head].sent = DateTime_CurrentUTC_MS();
	ping_entries[head].recv = 0;
	
	ping_head = head;
	return next;
}

void Ping_Update(int id) {
	int i;
	for (i = 0; i < Array_Elems(ping_entries); i++) {
		if (ping_entries[i].id != id) continue;

		ping_entries[i].recv = DateTime_CurrentUTC_MS();
		return;
	}
}

int Ping_AveragePingMS(void) {
	int i, measures = 0, totalMs = 0;

	for (i = 0; i < Array_Elems(ping_entries); i++) {
		struct PingEntry entry = ping_entries[i];
		if (!entry.sent || !entry.recv) continue;
	
		totalMs += (int)(entry.recv - entry.sent);
		measures++;
	}

	if (!measures) return 0;
	/* (recv - send) is time for packet to be sent to server and then sent back. */
	/* However for ping, only want time to send data to server, so half the total. */
	totalMs /= 2;
	return totalMs / measures;
}

static void Ping_Reset(void) {
	Mem_Set(ping_entries, 0, sizeof(ping_entries));
	ping_head = 0;
}


/*########################################################################################################################*
*-------------------------------------------------Singleplayer connection-------------------------------------------------*
*#########################################################################################################################*/
static char autoloadBuffer[FILENAME_SIZE];
cc_string SP_AutoloadMap = String_FromArray(autoloadBuffer);

static void SPConnection_BeginConnect(void) {
	static const cc_string logName = String_FromConst("Singleplayer");
	RNGState rnd;
	Chat_SetLogName(&logName);
	Game_UseCPEBlocks = Game_Version.HasCPE;

	/* For when user drops a map file onto ClassiCube.exe */
	if (SP_AutoloadMap.length) {
		Map_LoadFrom(&SP_AutoloadMap); return;
	}

	Random_SeedFromCurrentTime(&rnd);
	World_NewMap();
#if defined CC_BUILD_LOWMEM
	World_SetDimensions(64, 64, 64);
#else
	World_SetDimensions(128, 64, 128);
#endif

	Gen_Vanilla = true;
	Gen_Seed    = Random_Next(&rnd, Int32_MaxValue);
	GeneratingScreen_Show();
}

static char sp_lastCol;
static void SPConnection_AddPart(const cc_string* text) {
	cc_string tmp; char tmpBuffer[STRING_SIZE * 2];
	char col;
	int i;
	String_InitArray(tmp, tmpBuffer);

	/* Prepend color codes for subsequent lines of multi-line chat */
	if (!Drawer2D_IsWhiteColor(sp_lastCol)) {
		String_Append(&tmp, '&');
		String_Append(&tmp, sp_lastCol);
	}
	String_AppendString(&tmp, text);
	
	/* Replace all % with & */
	for (i = 0; i < tmp.length; i++) {
		if (tmp.buffer[i] == '%') tmp.buffer[i] = '&';
	}
	String_UNSAFE_TrimEnd(&tmp);

	col = Drawer2D_LastColor(&tmp, tmp.length);
	if (col) sp_lastCol = col;
	Chat_Add(&tmp);
}

static void SPConnection_SendChat(const cc_string* text) {
	cc_string left, part;
	if (!text->length) return;

	sp_lastCol = '\0';
	left = *text;

	while (left.length > STRING_SIZE) {
		part = String_UNSAFE_Substring(&left, 0, STRING_SIZE);
		SPConnection_AddPart(&part);
		left = String_UNSAFE_SubstringAt(&left, STRING_SIZE);
	}
	SPConnection_AddPart(&left);
}

static void SPConnection_SendBlock(int x, int y, int z, BlockID old, BlockID now) {
	Physics_OnBlockChanged(x, y, z, old, now);
}

static void SPConnection_SendData(const cc_uint8* data, cc_uint32 len) { }

static void SPConnection_Tick(struct ScheduledTask* task) {
	if (Server.Disconnected) return;
	/* 60 -> 20 ticks a second */
	if ((ticks++ % 3) != 0)  return;
	
	Physics_Tick();
	TexturePack_CheckPending();
}

static void SPConnection_Init(void) {
	Server_ResetState();
	Physics_Init();

	Server.BeginConnect = SPConnection_BeginConnect;
	Server.Tick         = SPConnection_Tick;
	Server.SendBlock    = SPConnection_SendBlock;
	Server.SendChat     = SPConnection_SendChat;
	Server.SendData     = SPConnection_SendData;
	
	Server.SupportsFullCP437       = !Game_ClassicMode;
	Server.SupportsPartialMessages = true;
	Server.IsSinglePlayer          = true;
}

static void MPConnection_Init(void) {
	assert(!"hey");
}

static void OnNewMap(void) {
	int i;
	if (Server.IsSinglePlayer) return;

	/* wipe all existing entities */
	for (i = 0; i < ENTITIES_SELF_ID; i++) 
	{
		Entities_Remove((EntityID)i);
	}
}

static void OnInit(void) {
	String_InitArray(Server.Name,    nameBuffer);
	String_InitArray(Server.MOTD,    motdBuffer);
	String_InitArray(Server.AppName, appBuffer);

	if (!Server.Address.length) {
		SPConnection_Init();
	} else {
		MPConnection_Init();
	}

	ScheduledTask_Add(GAME_NET_TICKS, Server.Tick);
	String_AppendConst(&Server.AppName, GAME_APP_NAME);
	String_AppendConst(&Server.AppName, Platform_AppNameSuffix);

#ifdef CC_BUILD_WEB
	if (!Input_TouchMode) return;
	Server.AppName.length = 0;
	String_AppendConst(&Server.AppName, GAME_APP_ALT);
#endif
}

static void OnClose(void);

static void OnReset(void) {
	if (Server.IsSinglePlayer) return;
	OnClose();
}

static void OnFree(void) {
	Server.Address.length = 0;
	OnClose();
}

static void OnClose(void) {
	if (Server.IsSinglePlayer) {
		Physics_Free();
	} else {
		Ping_Reset();
		if (Server.Disconnected) return;
		Server.Disconnected = true;
	}
}

struct IGameComponent Server_Component = {
	OnInit,  /* Init  */
	OnFree,  /* Free  */
	OnReset, /* Reset */
	OnNewMap /* OnNewMap */
};
