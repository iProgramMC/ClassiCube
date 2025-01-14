#include "Core.h"
#if defined CC_BUILD_WIN

#include "_PlatformBase.h"
#include "Stream.h"
#include "SystemFonts.h"
#include "Funcs.h"
#include "Utils.h"
#include "Errors.h"

#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <ws2tcpip.h>

/* === BEGIN shellapi.h === */
#define SHELLAPI DECLSPEC_IMPORT
SHELLAPI HINSTANCE WINAPI ShellExecuteW(HWND hwnd, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT showCmd);
SHELLAPI HINSTANCE WINAPI ShellExecuteA(HWND hwnd, LPCSTR operation,  LPCSTR file,  LPCSTR  parameters, LPCSTR  directory, INT showCmd);
/* === END shellapi.h === */
/* === BEGIN wincrypt.h === */
typedef struct _CRYPTOAPI_BLOB {
	DWORD cbData;
	BYTE* pbData;
} DATA_BLOB;
/* === END wincrypt.h === */

static HANDLE heap;
const cc_result ReturnCode_FileShareViolation = ERROR_SHARING_VIOLATION;
const cc_result ReturnCode_FileNotFound     = ERROR_FILE_NOT_FOUND;
const cc_result ReturnCode_SocketInProgess  = WSAEINPROGRESS;
const cc_result ReturnCode_SocketWouldBlock = WSAEWOULDBLOCK;
const cc_result ReturnCode_DirectoryExists  = ERROR_ALREADY_EXISTS;
const char* Platform_AppNameSuffix = "";
cc_bool Platform_SingleProcess;

/*########################################################################################################################*
*---------------------------------------------------------Memory----------------------------------------------------------*
*#########################################################################################################################*/
void Mem_Set(void*  dst, cc_uint8 value,  cc_uint32 numBytes) { memset(dst, value, numBytes); }
void Mem_Copy(void* dst, const void* src, cc_uint32 numBytes) { memcpy(dst, src,   numBytes); }

void* Mem_TryAlloc(cc_uint32 numElems, cc_uint32 elemsSize) {
	cc_uint32 size = CalcMemSize(numElems, elemsSize);
	return size ? HeapAlloc(heap, 0, size) : NULL;
}

void* Mem_TryAllocCleared(cc_uint32 numElems, cc_uint32 elemsSize) {
	cc_uint32 size = CalcMemSize(numElems, elemsSize);
	return size ? HeapAlloc(heap, HEAP_ZERO_MEMORY, size) : NULL;
}

void* Mem_TryRealloc(void* mem, cc_uint32 numElems, cc_uint32 elemsSize) {
	cc_uint32 size = CalcMemSize(numElems, elemsSize);
	return size ? HeapReAlloc(heap, 0, mem, size) : NULL;
}

void Mem_Free(void* mem) {
	if (mem) HeapFree(heap, 0, mem);
}


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
/* TODO: check this is actually accurate */
static cc_uint64 sw_freqMul = 1, sw_freqDiv = 1;
cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return ((end - beg) * sw_freqMul) / sw_freqDiv;
}

static HANDLE conHandle;
static BOOL hasDebugger;

void Platform_Log(const char* msg, int len) {
	char tmp[2048 + 1];
	DWORD wrote;

	if (conHandle) {
		WriteFile(conHandle, msg,  len, &wrote, NULL);
		WriteFile(conHandle, "\n",   1, &wrote, NULL);
	}

	if (!hasDebugger) return;
	len = min(len, 2048);
	Mem_Copy(tmp, msg, len); tmp[len] = '\0';

	OutputDebugStringA(tmp);
	OutputDebugStringA("\n");
}

#define FILETIME_EPOCH 50491123200000ULL
#define FILETIME_UNIX_EPOCH 11644473600LL
#define FileTime_TotalMS(time)  ((time / 10000)    + FILETIME_EPOCH)
#define FileTime_UnixTime(time) ((time / 10000000) - FILETIME_UNIX_EPOCH)
TimeMS DateTime_CurrentUTC_MS(void) {
	FILETIME ft; 
	cc_uint64 raw;
	
	GetSystemTimeAsFileTime(&ft);
	/* in 100 nanosecond units, since Jan 1 1601 */
	raw = ft.dwLowDateTime | ((cc_uint64)ft.dwHighDateTime << 32);
	return FileTime_TotalMS(raw);
}

void DateTime_CurrentLocal(struct DateTime* t) {
	SYSTEMTIME localTime;
	GetLocalTime(&localTime);

	t->year   = localTime.wYear;
	t->month  = localTime.wMonth;
	t->day    = localTime.wDay;
	t->hour   = localTime.wHour;
	t->minute = localTime.wMinute;
	t->second = localTime.wSecond;
}

static cc_bool sw_highRes;
cc_uint64 Stopwatch_Measure(void) {
	LARGE_INTEGER t;
	FILETIME ft;

	if (sw_highRes) {
		QueryPerformanceCounter(&t);
		return (cc_uint64)t.QuadPart;
	} else {		
		GetSystemTimeAsFileTime(&ft);
		return (cc_uint64)ft.dwLowDateTime | ((cc_uint64)ft.dwHighDateTime << 32);
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
void Directory_GetCachePath(cc_string* path) { }

cc_result Directory_Create(const cc_string* path) {
	cc_winstring str;
	cc_result res;

	Platform_EncodeString(&str, path);
	if (CreateDirectoryW(str.uni, NULL)) return 0;
	/* Windows 9x does not support W API functions */
	if ((res = GetLastError()) != ERROR_CALL_NOT_IMPLEMENTED) return res;

	return CreateDirectoryA(str.ansi, NULL) ? 0 : GetLastError();
}

int File_Exists(const cc_string* path) {
	cc_winstring str;
	DWORD attribs;

	Platform_EncodeString(&str, path);
	attribs = GetFileAttributesW(str.uni);

	return attribs != INVALID_FILE_ATTRIBUTES && !(attribs & FILE_ATTRIBUTE_DIRECTORY);
}

static cc_result Directory_EnumCore(const cc_string* dirPath, const cc_string* file, DWORD attribs,
									void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[MAX_PATH + 10];
	/* ignore . and .. entry */
	if (file->length == 1 && file->buffer[0] == '.') return 0;
	if (file->length == 2 && file->buffer[0] == '.' && file->buffer[1] == '.') return 0;

	String_InitArray(path, pathBuffer);
	String_Format2(&path, "%s/%s", dirPath, file);

	if (attribs & FILE_ATTRIBUTE_DIRECTORY) return Directory_Enum(&path, obj, callback);
	callback(&path, obj);
	return 0;
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[MAX_PATH + 10];
	WIN32_FIND_DATAW eW;
	WIN32_FIND_DATAA eA;
	int i, ansi = false;
	cc_winstring str;
	HANDLE find;
	cc_result res;	

	/* Need to append \* to search for files in directory */
	String_InitArray(path, pathBuffer);
	String_Format1(&path, "%s\\*", dirPath);
	Platform_EncodeString(&str, &path);
	
	find = FindFirstFileW(str.uni, &eW);
	if (!find || find == INVALID_HANDLE_VALUE) {
		if ((res = GetLastError()) != ERROR_CALL_NOT_IMPLEMENTED) return res;
		ansi = true;

		/* Windows 9x does not support W API functions */
		find = FindFirstFileA(str.ansi, &eA);
		if (find == INVALID_HANDLE_VALUE) return GetLastError();
	}

	if (ansi) {
		do {
			path.length = 0;
			for (i = 0; i < MAX_PATH && eA.cFileName[i]; i++) {
				String_Append(&path, Convert_CodepointToCP437(eA.cFileName[i]));
			}
			if ((res = Directory_EnumCore(dirPath, &path, eA.dwFileAttributes, obj, callback))) return res;
		} while (FindNextFileA(find, &eA));
	} else {
		do {
			path.length = 0;
			for (i = 0; i < MAX_PATH && eW.cFileName[i]; i++) {
				/* TODO: UTF16 to codepoint conversion */
				String_Append(&path, Convert_CodepointToCP437(eW.cFileName[i]));
			}
			if ((res = Directory_EnumCore(dirPath, &path, eW.dwFileAttributes, obj, callback))) return res;
		} while (FindNextFileW(find, &eW));
	}

	res = GetLastError(); /* return code from FindNextFile */
	FindClose(find);
	return res == ERROR_NO_MORE_FILES ? 0 : res;
}

static cc_result DoFileRaw(cc_file* file, const cc_winstring* str, DWORD access, DWORD createMode) {
	cc_result res;

	*file = CreateFileW(str->uni,  access, FILE_SHARE_READ, NULL, createMode, 0, NULL);
	if (*file && *file != INVALID_HANDLE_VALUE) return 0;
	if ((res = GetLastError()) != ERROR_CALL_NOT_IMPLEMENTED) return res;

	/* Windows 9x does not support W API functions */
	*file = CreateFileA(str->ansi, access, FILE_SHARE_READ, NULL, createMode, 0, NULL);
	return *file != INVALID_HANDLE_VALUE ? 0 : GetLastError();
}

static cc_result DoFile(cc_file* file, const cc_string* path, DWORD access, DWORD createMode) {
	cc_winstring str;
	Platform_EncodeString(&str, path);
	return DoFileRaw(file, &str, access, createMode);
}

cc_result File_Open(cc_file* file, const cc_string* path) {
	return DoFile(file, path, GENERIC_READ, OPEN_EXISTING);
}
cc_result File_Create(cc_file* file, const cc_string* path) {
	return DoFile(file, path, GENERIC_WRITE | GENERIC_READ, CREATE_ALWAYS);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_string* path) {
	return DoFile(file, path, GENERIC_WRITE | GENERIC_READ, OPEN_ALWAYS);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	BOOL success = ReadFile(file, data, count, bytesRead, NULL);
	return success ? 0 : GetLastError();
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	BOOL success = WriteFile(file, data, count, bytesWrote, NULL);
	return success ? 0 : GetLastError();
}

cc_result File_Close(cc_file file) {
	return CloseHandle(file) ? 0 : GetLastError();
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { FILE_BEGIN, FILE_CURRENT, FILE_END };
	DWORD pos = SetFilePointer(file, offset, NULL, modes[seekType]);
	return pos != INVALID_SET_FILE_POINTER ? 0 : GetLastError();
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	*pos = SetFilePointer(file, 0, NULL, FILE_CURRENT);
	return *pos != INVALID_SET_FILE_POINTER ? 0 : GetLastError();
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	*len = GetFileSize(file, NULL);
	return *len != INVALID_FILE_SIZE ? 0 : GetLastError();
}

/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#############################################################################################################p############*/
void Thread_Sleep(cc_uint32 milliseconds) { Sleep(milliseconds); }

//

/*########################################################################################################################*
*--------------------------------------------------------Font/Text--------------------------------------------------------*
*#########################################################################################################################*/
static void FontDirCallback(const cc_string* path, void* obj) {
	static const cc_string fonExt = String_FromConst(".fon");
	/* Completely skip windows .FON files */
	if (String_CaselessEnds(path, &fonExt)) return;
	SysFonts_Register(path);
}

void Platform_LoadSysFonts(void) { 
	int i;
	char winFolder[FILENAME_SIZE];
	WCHAR winTmp[FILENAME_SIZE];
	UINT winLen;
	
	cc_string dirs[2];
	String_InitArray(dirs[0], winFolder);
	dirs[1] = String_FromReadonly("C:/WINNT35/system");

	/* System folder path may not be C:/Windows */
	winLen = GetWindowsDirectoryW(winTmp, FILENAME_SIZE);
	if (winLen) {
		String_AppendUtf16(&dirs[0], winTmp, winLen * 2);
	} else {
		String_AppendConst(&dirs[0], "C:/Windows");
	}
	String_AppendConst(&dirs[0], "/fonts");

	for (i = 0; i < Array_Elems(dirs); i++) {
		Directory_Enum(&dirs[i], NULL, FontDirCallback);
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Process/Module------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Process_OpenSupported = true;

static cc_result Process_RawGetExePath(cc_winstring* path, int* len) {
	cc_result res;

	/* If GetModuleFileNameA fails.. that's a serious problem */
	*len = GetModuleFileNameA(NULL, path->ansi, NATIVE_STR_LEN);
	path->ansi[*len] = '\0';
	if (!(*len)) return GetLastError();
	
	*len = GetModuleFileNameW(NULL, path->uni, NATIVE_STR_LEN);
	path->uni[*len]  = '\0';
	if (*len) return 0;

	/* GetModuleFileNameW can fail on Win 9x */
	res = GetLastError();
	if (res == ERROR_CALL_NOT_IMPLEMENTED) res = 0;
	return res;
}

cc_result Process_StartGame2(const cc_string* args, int numArgs) {
	cc_winstring path;
	cc_string argv; char argvBuffer[NATIVE_STR_LEN];
	STARTUPINFOW si        = { 0 };
	PROCESS_INFORMATION pi = { 0 };
	cc_winstring raw;
	cc_result res;
	int len, i;

	if ((res = Process_RawGetExePath(&path, &len))) return res;
	si.cb = sizeof(STARTUPINFOW);
	
	String_InitArray(argv, argvBuffer);
	/* Game doesn't actually care about argv[0] */
	String_AppendConst(&argv, "cc");
	for (i = 0; i < numArgs; i++)
	{
		if (String_IndexOf(&args[i], ' ') >= 0) {
			String_Format1(&argv, " \"%s\"", &args[i]);
		} else {
			String_Format1(&argv, " %s",     &args[i]);
		}
	}
	Platform_EncodeString(&raw, &argv);

	if (path.uni[0]) {
		if (!CreateProcessW(path.uni, raw.uni, NULL, NULL,
				false, 0, NULL, NULL, &si, &pi)) return GetLastError();
	} else {
		/* Windows 9x does not support W API functions */
		if (!CreateProcessA(path.ansi, raw.ansi, NULL, NULL,
				false, 0, NULL, NULL, &si, &pi)) return GetLastError();
	}

	/* Don't leak memory for process return code */
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0;
}

void Process_Exit(cc_result code) { ExitProcess(code); }
cc_result Process_StartOpen(const cc_string* args) {
	cc_winstring str;
	cc_uintptr res;
	Platform_EncodeString(&str, args);

	res = (cc_uintptr)ShellExecuteW(NULL, NULL, str.uni, NULL, NULL, SW_SHOWNORMAL);
	/* Windows 9x always returns "error 0" for ShellExecuteW */
	if (res == 0) res = (cc_uintptr)ShellExecuteA(NULL, NULL, str.ansi, NULL, NULL, SW_SHOWNORMAL);

	/* MSDN: "If the function succeeds, it returns a value greater than 32. If the function fails, */
	/*  it returns an error value that indicates the cause of the failure" */
	return res > 32 ? 0 : (cc_result)res;
}


/*########################################################################################################################*
*--------------------------------------------------------Updater----------------------------------------------------------*
*#########################################################################################################################*/
#define UPDATE_TMP TEXT("CC_prev.exe")
#define UPDATE_SRC TEXT(UPDATE_FILE)
cc_bool Updater_Supported = true;

const struct UpdaterInfo Updater_Info = {
	"&eDirect3D 9 is recommended", 2,
	{
#if _WIN64
		{ "Direct3D9", "ClassiCube.64.exe" },
		{ "OpenGL",    "ClassiCube.64-opengl.exe" }
#else
		{ "Direct3D9", "ClassiCube.exe" },
		{ "OpenGL",    "ClassiCube.opengl.exe" }
#endif
	}
};

cc_bool Updater_Clean(void) {
	return DeleteFile(UPDATE_TMP) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

cc_result Updater_Start(const char** action) {
	cc_winstring path;
	cc_result res;
	int len;

	*action = "Getting executable path";
	if ((res = Process_RawGetExePath(&path, &len))) return res;

	*action = "Moving executable to CC_prev.exe"; 
	if (!path.uni[0]) return ERR_NOT_SUPPORTED; /* MoveFileA returns ERROR_ACCESS_DENIED on Win 9x anyways */
	if (!MoveFileExW(path.uni, UPDATE_TMP, MOVEFILE_REPLACE_EXISTING)) return GetLastError();

	*action = "Replacing executable";
	if (!MoveFileExW(UPDATE_SRC, path.uni, MOVEFILE_REPLACE_EXISTING)) return GetLastError();

	*action = "Restarting game";
	return Process_StartGame2(NULL, 0);
}

cc_result Updater_GetBuildTime(cc_uint64* timestamp) {
	cc_winstring path;
	cc_file file;
	FILETIME ft;
	cc_uint64 raw;
	cc_result res;
	int len;

	if ((res = Process_RawGetExePath(&path, &len))) return res;
	if ((res = DoFileRaw(&file, &path, GENERIC_READ, OPEN_EXISTING))) return res;

	if (GetFileTime(file, NULL, NULL, &ft)) {
		raw        = ft.dwLowDateTime | ((cc_uint64)ft.dwHighDateTime << 32);
		*timestamp = FileTime_UnixTime(raw);
	} else {
		res = GetLastError();
	}

	File_Close(file);
	return res;
}

/* Don't need special execute permission on windows */
cc_result Updater_MarkExecutable(void) { return 0; }
cc_result Updater_SetNewBuildTime(cc_uint64 timestamp) {
	static const cc_string path = String_FromConst(UPDATE_FILE);
	cc_file file;
	FILETIME ft;
	cc_uint64 raw;
	cc_result res = File_OpenOrCreate(&file, &path);
	if (res) return res;

	raw = 10000000 * (timestamp + FILETIME_UNIX_EPOCH);
	ft.dwLowDateTime  = (cc_uint32)raw;
	ft.dwHighDateTime = (cc_uint32)(raw >> 32);

	if (!SetFileTime(file, NULL, NULL, &ft)) res = GetLastError();
	File_Close(file);
	return res;
}

/*########################################################################################################################*
*-------------------------------------------------------Dynamic lib-------------------------------------------------------*
*#########################################################################################################################*/
const cc_string SDynamicLib_Ext = String_FromConst(".dll");
static cc_result dynamicErr;
static cc_bool loadingPlugin;

void* NDynamicLib_Load2(const cc_string* path) {
	static cc_string plugins_dir = String_FromConst("plugins/");
	cc_winstring str;
	void* lib;

	Platform_EncodeString(&str, path);
	loadingPlugin = String_CaselessStarts(path, &plugins_dir);

	if ((lib = LoadLibraryW(str.uni))) return lib;
	dynamicErr = GetLastError();
	if (dynamicErr != ERROR_CALL_NOT_IMPLEMENTED) return NULL;

	/* Windows 9x only supports A variants */
	lib = LoadLibraryA(str.ansi);
	if (!lib) dynamicErr = GetLastError();
	return lib;
}

void* NDynamicLib_Get2(void* lib, const char* name) {
	void* addr = GetProcAddress((HMODULE)lib, name);
	if (!addr) dynamicErr = GetLastError();
	return addr;
}

cc_bool NDynamicLib_DescribeError(cc_string* dst) {
	cc_result res = dynamicErr;
	dynamicErr = 0; /* Reset error (match posix behaviour) */

	Platform_DescribeError(res, dst);
	String_Format1(dst, " (error %i)", &res);

	/* Plugin may have been compiled to load symbols from ClassiCube.exe, */
	/*  but the user might have renamed it to something else */
	if (res == ERROR_MOD_NOT_FOUND  && loadingPlugin) {
		String_AppendConst(dst, "\n    Make sure the ClassiCube executable is named ClassiCube.exe");
	}
	if (res == ERROR_PROC_NOT_FOUND && loadingPlugin) {
		String_AppendConst(dst, "\n    The plugin or your game may be outdated");
	}

	/* User might be trying to use 32 bit plugin with 64 bit executable, or vice versa */
	if (res == ERROR_BAD_EXE_FORMAT && loadingPlugin) {
		if (sizeof(cc_uintptr) == 4) {
			String_AppendConst(dst, "\n    Try using a 32 bit version of the plugin instead");
		} else {
			String_AppendConst(dst, "\n    Try using a 64 bit version of the plugin instead");
		}
	}
	return true;
}

/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
void Platform_EncodeString(cc_winstring* dst, const cc_string* src) {
	cc_unichar* uni;
	char* ansi;
	int i;
	if (src->length > FILENAME_SIZE) Logger_Abort("String too long to expand");

	uni = dst->uni;
	for (i = 0; i < src->length; i++) {
		*uni++ = Convert_CP437ToUnicode(src->buffer[i]);
	}
	*uni = '\0';

	ansi = dst->ansi;
	for (i = 0; i < src->length; i++) {
		*ansi++ = (char)dst->uni[i];
	}
	*ansi = '\0';
}

static void Platform_InitStopwatch(void) {
	LARGE_INTEGER freq;
	sw_highRes = QueryPerformanceFrequency(&freq);

	if (sw_highRes) {
		sw_freqMul = 1000 * 1000;
		sw_freqDiv = freq.QuadPart;
	} else { sw_freqDiv = 10; }
}

void Platform_Init(void) {
	WSADATA wsaData;
	cc_result res;

	Platform_InitStopwatch();
	heap = GetProcessHeap();

	conHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (conHandle == INVALID_HANDLE_VALUE) conHandle = NULL;
}

void Platform_Free(void) {
	HeapDestroy(heap);
}

cc_bool Platform_DescribeErrorExt(cc_result res, cc_string* dst, void* lib) {
	WCHAR chars[NATIVE_STR_LEN];
	DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	if (lib) flags |= FORMAT_MESSAGE_FROM_HMODULE;

	res = FormatMessageW(flags, lib, res, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), 
						 chars, NATIVE_STR_LEN, NULL);
	if (!res) return false;

	String_AppendUtf16(dst, chars, res * 2);
	return true;
}

cc_bool Platform_DescribeError(cc_result res, cc_string* dst) {
	return Platform_DescribeErrorExt(res, dst, NULL);
}


/*########################################################################################################################*
*-------------------------------------------------------Encryption--------------------------------------------------------*
*#########################################################################################################################*/
/* Encrypts data using XTEA block cipher, with OS specific method to get machine-specific key */

static void EncipherBlock(cc_uint32* v, const cc_uint32* key, cc_string* dst) {
	cc_uint32 v0 = v[0], v1 = v[1], sum = 0, delta = 0x9E3779B9;
	int i;

	for (i = 0; i < 12; i++) {
		v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
		sum += delta;
		v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
	}
	v[0] = v0; v[1] = v1;
	String_AppendAll(dst, v, 8);
}

static void DecipherBlock(cc_uint32* v, const cc_uint32* key) {
	cc_uint32 v0 = v[0], v1 = v[1], delta = 0x9E3779B9, sum = delta * 12;
	int i;

	for (i = 0; i < 12; i++) {
		v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
		sum -= delta;
		v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
	}
	v[0] = v0; v[1] = v1;
}

#define ENC1 0xCC005EC0
#define ENC2 0x0DA4A0DE 
#define ENC3 0xC0DED000
#define MACHINEID_LEN 32
#define ENC_SIZE 8 /* 2 32 bit ints per block */

int PackedCol_DeHex(char hex);

/* "b3 c5a-0d9" --> 0xB3C5A0D9 */
static void DecodeMachineID(char* tmp, int len, cc_uint32* key) {
	int hex[MACHINEID_LEN] = { 0 }, i, j, c;
	cc_uint8* dst = (cc_uint8*)key;

	/* Get each valid hex character */
	for (i = 0, j = 0; i < len && j < MACHINEID_LEN; i++) {
		c = PackedCol_DeHex(tmp[i]);
		if (c != -1) hex[j++] = c;
	}

	for (i = 0; i < MACHINEID_LEN / 2; i++) {
		dst[i] = (hex[i * 2] << 4) | hex[i * 2 + 1];
	}
}

static cc_result GetMachineID(cc_uint32* key) {
	// hack
	*key = GetTickCount();

	return 0;
}

cc_result Platform_Encrypt(const void* data, int len, cc_string* dst) {
	const cc_uint8* src = (const cc_uint8*)data;
	cc_uint32 header[4], key[4];
	cc_result res;
	if ((res = GetMachineID(key))) return res;

	header[0] = ENC1; header[1] = ENC2;
	header[2] = ENC3; header[3] = len;
	EncipherBlock(header + 0, key, dst);
	EncipherBlock(header + 2, key, dst);

	for (; len > 0; len -= ENC_SIZE, src += ENC_SIZE) {
		header[0] = 0; header[1] = 0;
		Mem_Copy(header, src, min(len, ENC_SIZE));
		EncipherBlock(header, key, dst);
	}
	return 0;
}
cc_result Platform_Decrypt(const void* data, int len, cc_string* dst) {
	const cc_uint8* src = (const cc_uint8*)data;
	cc_uint32 header[4], key[4];
	cc_result res;
	int dataLen;

	/* Total size must be >= header size */
	if (len < 16) return ERR_END_OF_STREAM;
	if ((res = GetMachineID(key))) return res;

	Mem_Copy(header, src, 16);
	DecipherBlock(header + 0, key);
	DecipherBlock(header + 2, key);

	if (header[0] != ENC1 || header[1] != ENC2 || header[2] != ENC3) return ERR_INVALID_ARGUMENT;
	len -= 16; src += 16;

	if (header[3] > len) return ERR_INVALID_ARGUMENT;
	dataLen = header[3];

	for (; dataLen > 0; len -= ENC_SIZE, src += ENC_SIZE, dataLen -= ENC_SIZE) {
		header[0] = 0; header[1] = 0;
		Mem_Copy(header, src, min(len, ENC_SIZE));

		DecipherBlock(header, key);
		String_AppendAll(dst, header, min(dataLen, ENC_SIZE));
	}
	return 0;
}


/*########################################################################################################################*
*-----------------------------------------------------Configuration-------------------------------------------------------*
*#########################################################################################################################*/
static cc_string Platform_NextArg(STRING_REF cc_string* args) {
	cc_string arg;
	int end;

	/* get rid of leading spaces before arg */
	while (args->length && args->buffer[0] == ' ') {
		*args = String_UNSAFE_SubstringAt(args, 1);
	}

	if (args->length && args->buffer[0] == '"') {
		/* "xy za" is used for arg with spaces */
		*args = String_UNSAFE_SubstringAt(args, 1);
		end = String_IndexOf(args, '"');
	} else {
		end = String_IndexOf(args, ' ');
	}

	if (end == -1) {
		arg   = *args;
		args->length = 0;
	} else {
		arg   = String_UNSAFE_Substring(args, 0, end);
		*args = String_UNSAFE_SubstringAt(args, end + 1);
	}
	return arg;
}

int Platform_GetCommandLineArgs(int argc, STRING_REF char** argv, cc_string* args) {
	cc_string cmdArgs = String_FromReadonly(GetCommandLineA());
	int i;
	Platform_NextArg(&cmdArgs); /* skip exe path */

	for (i = 0; i < GAME_MAX_CMDARGS; i++) {
		args[i] = Platform_NextArg(&cmdArgs);

		if (!args[i].length) break;
	}
	return i;
}

/* Detects if the game is running in Windows directory */
/* This happens when ClassiCube is launched directly from shell process */
/*  (e.g. via clicking a search result in Windows 10 start menu) */
static cc_bool IsProblematicWorkingDirectory(void) {
	cc_string curDir, winDir;
	char curPath[2048] = { 0 };
	char winPath[2048] = { 0 };

	GetCurrentDirectoryA(2048, curPath);
	GetSystemDirectoryA(winPath, 2048);

	curDir = String_FromReadonly(curPath);
	winDir = String_FromReadonly(winPath);
	
	if (String_Equals(&curDir, &winDir)) {
		Platform_LogConst("Working directory is System32! Changing to executable directory..");
		return true;
	}
	return false;
}

cc_result Platform_SetDefaultCurrentDirectory(int argc, char** argv) {
	cc_winstring path;
	int i, len;
	cc_result res;
	if (!IsProblematicWorkingDirectory()) return 0;

	res = Process_RawGetExePath(&path, &len);
	if (res) return res;
	if (!path.uni[0]) return ERR_NOT_SUPPORTED; 
	/* Not implemented on ANSI only systems due to laziness */

	/* Get rid of filename at end of directory */
	for (i = len - 1; i >= 0; i--, len--) 
	{
		if (path.uni[i] == '/' || path.uni[i] == '\\') break;
	}

	path.uni[len] = '\0';
	return SetCurrentDirectoryW(path.uni) ? 0 : GetLastError();
}
#endif
