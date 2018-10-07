#ifndef CC_PLATFORM_H
#define CC_PLATFORM_H
#include "Utils.h"
#include "PackedCol.h"
/* Abstracts platform specific memory management, I/O, etc.
   Copyright 2017 ClassicalSharp | Licensed under BSD-3
*/
struct DrawTextArgs;
struct AsyncRequest;

enum SOCKET_SELECT { SOCKET_SELECT_READ, SOCKET_SELECT_WRITE };
#ifdef CC_BUILD_WIN
typedef uintptr_t SocketPtr;
#else
typedef int SocketPtr;
#endif

extern char* Platform_NewLine; /* Newline for text */
extern char  Directory_Separator;
extern char* Font_DefaultName;
extern ReturnCode ReturnCode_FileShareViolation;
extern ReturnCode ReturnCode_FileNotFound;
extern ReturnCode ReturnCode_NotSupported;
extern ReturnCode ReturnCode_SocketInProgess;
extern ReturnCode ReturnCode_SocketWouldBlock;
extern ReturnCode ReturnCode_InvalidArg;

void Platform_ConvertString(void* dstPtr, const String* src);
void Platform_Init(void);
void Platform_Free(void);
void Platform_SetWorkingDir(void);
void Platform_Exit(ReturnCode code);
int  Platform_GetCommandLineArgs(int argc, STRING_REF const char** argv, String* args);
ReturnCode Platform_StartShell(const String* args);

NOINLINE_ void* Mem_Alloc(uint32_t numElems, uint32_t elemsSize, const char* place);
NOINLINE_ void* Mem_AllocCleared(uint32_t numElems, uint32_t elemsSize, const char* place);
NOINLINE_ void* Mem_Realloc(void* mem, uint32_t numElems, uint32_t elemsSize, const char* place);
NOINLINE_ void  Mem_Free(void* mem);
void Mem_Set(void* dst, uint8_t value, uint32_t numBytes);
void Mem_Copy(void* dst, void* src, uint32_t numBytes);

void Platform_Log(const String* message);
void Platform_LogConst(const char* message);
void Platform_Log1(const char* format, const void* a1);
void Platform_Log2(const char* format, const void* a1, const void* a2);
void Platform_Log3(const char* format, const void* a1, const void* a2, const void* a3);
void Platform_Log4(const char* format, const void* a1, const void* a2, const void* a3, const void* a4);

TimeMS DateTime_CurrentUTC_MS(void);
void DateTime_CurrentUTC(DateTime* time);
void DateTime_CurrentLocal(DateTime* time);
void Stopwatch_Measure(uint64_t* timer);
int  Stopwatch_ElapsedMicroseconds(uint64_t* timer);

bool Directory_Exists(const String* path);
ReturnCode Directory_Create(const String* path);
typedef void Directory_EnumCallback(const String* filename, void* obj);
ReturnCode Directory_Enum(const String* path, void* obj, Directory_EnumCallback callback);
bool File_Exists(const String* path);
ReturnCode File_GetModifiedTime_MS(const String* path, TimeMS* ms);

ReturnCode File_Create(void** file, const String* path);
ReturnCode File_Open(void** file, const String* path);
ReturnCode File_Append(void** file, const String* path);
ReturnCode File_Read(void* file, uint8_t* buffer, uint32_t count, uint32_t* bytesRead);
ReturnCode File_Write(void* file, uint8_t* buffer, uint32_t count, uint32_t* bytesWrote);
ReturnCode File_Close(void* file);
ReturnCode File_Seek(void* file, int offset, int seekType);
ReturnCode File_Position(void* file, uint32_t* position);
ReturnCode File_Length(void* file, uint32_t* length);

void Thread_Sleep(uint32_t milliseconds);
typedef void Thread_StartFunc(void);
void* Thread_Start(Thread_StartFunc* func, bool detach);
void Thread_Detach(void* handle);
void Thread_Join(void* handle);

void* Mutex_Create(void);
void  Mutex_Free(void* handle);
void  Mutex_Lock(void* handle);
void  Mutex_Unlock(void* handle);

void* Waitable_Create(void);
void  Waitable_Free(void* handle);
void  Waitable_Signal(void* handle);
void  Waitable_Wait(void* handle); 
void  Waitable_WaitFor(void* handle, uint32_t milliseconds);

NOINLINE_ void Font_GetNames(StringsBuffer* buffer);
NOINLINE_ void Font_Make(FontDesc* desc, const String* fontName, int size, int style);
NOINLINE_ void Font_Free(FontDesc* desc);
NOINLINE_ Size2D Platform_TextMeasure(struct DrawTextArgs* args);
NOINLINE_ Size2D Platform_TextDraw(struct DrawTextArgs* args, Bitmap* bmp, int x, int y, PackedCol col);

void Socket_Create(SocketPtr* socket);
ReturnCode Socket_Available(SocketPtr socket, uint32_t* available);
ReturnCode Socket_SetBlocking(SocketPtr socket, bool blocking);
ReturnCode Socket_GetError(SocketPtr socket, ReturnCode* result);

ReturnCode Socket_Connect(SocketPtr socket, const String* ip, int port);
ReturnCode Socket_Read(SocketPtr socket, uint8_t* buffer, uint32_t count, uint32_t* modified);
ReturnCode Socket_Write(SocketPtr socket, uint8_t* buffer, uint32_t count, uint32_t* modified);
ReturnCode Socket_Close(SocketPtr socket);
ReturnCode Socket_Select(SocketPtr socket, int selectMode, bool* success);

void Http_Init(void);
ReturnCode Http_Do(struct AsyncRequest* req, volatile int* progress);
ReturnCode Http_Free(void);

#define AUDIO_MAX_CHUNKS 4
struct AudioFormat { uint16_t Channels, BitsPerSample; int SampleRate; };
#define AudioFormat_Eq(a, b) ((a)->Channels == (b)->Channels && (a)->BitsPerSample == (b)->BitsPerSample && (a)->SampleRate == (b)->SampleRate)
typedef int AudioHandle;

void Audio_Init(AudioHandle* handle, int buffers);
ReturnCode Audio_Free(AudioHandle handle);
ReturnCode Audio_StopAndFree(AudioHandle handle);
struct AudioFormat* Audio_GetFormat(AudioHandle handle);
ReturnCode Audio_SetFormat(AudioHandle handle, struct AudioFormat* format);
ReturnCode Audio_BufferData(AudioHandle handle, int idx, void* data, uint32_t dataSize);
ReturnCode Audio_Play(AudioHandle handle);
ReturnCode Audio_Stop(AudioHandle handle);
ReturnCode Audio_IsCompleted(AudioHandle handle, int idx, bool* completed);
ReturnCode Audio_IsFinished(AudioHandle handle, bool* finished);
#endif
