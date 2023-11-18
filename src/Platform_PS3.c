#include "Core.h"
#if defined PLAT_PS3

#include "_PlatformBase.h"
#include "Stream.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Window.h"
#include "Utils.h"
#include "Errors.h"
#include "PackedCol.h"
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/time.h>
#include <net/net.h>
#include <net/poll.h>
#include <ppu-lv2.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <sys/sem.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sys/tty.h>
#include "_PlatformConsole.h"

const cc_result ReturnCode_FileShareViolation = 1000000000; // not used
const cc_result ReturnCode_FileNotFound     = 0x80010006; // ENOENT;
//const cc_result ReturnCode_SocketInProgess  = 0x80010032; // EINPROGRESS
//const cc_result ReturnCode_SocketWouldBlock = 0x80010001; // EWOULDBLOCK;
const cc_result ReturnCode_DirectoryExists  = 0x80010014; // EEXIST

const cc_result ReturnCode_SocketInProgess  = 567456;
const cc_result ReturnCode_SocketWouldBlock = 675364;
const char* Platform_AppNameSuffix = " PS3";


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Log(const char* msg, int len) {
	u32 done = 0;
	sysTtyWrite(STDOUT_FILENO, msg,  len, &done);
	sysTtyWrite(STDOUT_FILENO, "\n", 1,   &done);
}

#define UnixTime_TotalMS(time) ((cc_uint64)time.tv_sec * 1000 + UNIX_EPOCH + (time.tv_usec / 1000))
TimeMS DateTime_CurrentUTC_MS(void) {
	struct timeval cur;
	gettimeofday(&cur, NULL);
	return UnixTime_TotalMS(cur);
}

void DateTime_CurrentLocal(struct DateTime* t) {
	struct timeval cur; 
	struct tm loc_time;
	gettimeofday(&cur, NULL);
	localtime_r(&cur.tv_sec, &loc_time);

	t->year   = loc_time.tm_year + 1900;
	t->month  = loc_time.tm_mon  + 1;
	t->day    = loc_time.tm_mday;
	t->hour   = loc_time.tm_hour;
	t->minute = loc_time.tm_min;
	t->second = loc_time.tm_sec;
}


/*########################################################################################################################*
*--------------------------------------------------------Stopwatch--------------------------------------------------------*
*#########################################################################################################################*/
#define NS_PER_SEC 1000000000ULL

cc_uint64 Stopwatch_Measure(void) { 
	u64 sec, nsec;
	sysGetCurrentTime(&sec, &nsec);
	return sec * NS_PER_SEC + nsec;
}

cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return (end - beg) / 1000;
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
static const cc_string root_path = String_FromConst("/dev_hdd0/ClassiCube/");

static void GetNativePath(char* str, const cc_string* path) {
	Mem_Copy(str, root_path.buffer, root_path.length);
	str += root_path.length;
	String_EncodeUtf8(str, path);
}

cc_result Directory_Create(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	GetNativePath(str, path);
	/* read/write/search permissions for owner and group, and with read/search permissions for others. */
	/* TODO: Is the default mode in all cases */
	return sysLv2FsMkdir(str, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

int File_Exists(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	sysFSStat sb;
	GetNativePath(str, path);
	return sysLv2FsStat(str, &sb) == 0 && S_ISREG(sb.st_mode);
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	char str[NATIVE_STR_LEN];
	sysFSDirent entry;
	char* src;
	int dir_fd, res;

	GetNativePath(str, dirPath);
	if ((res = sysLv2FsOpenDir(str, &dir_fd))) return res;

	for (;;)
	{
		u64 read = 0;
		if ((res = sysLv2FsReadDir(dir_fd, &entry, &read))) return res;
		if (!read) break; // end of entries

		// ignore . and .. entry
		src = entry.d_name;
		if (src[0] == '.' && src[1] == '\0') continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;
	
		String_InitArray(path, pathBuffer);
		String_Format1(&path, "%s/", dirPath);
		
		int len = String_Length(src);	
		String_AppendUtf8(&path, src, len);

		if (entry.d_type == DT_DIR) {
			res = Directory_Enum(&path, obj, callback);
			if (res) break;
		} else {
			callback(&path, obj);
		}
	}

	sysLv2FsCloseDir(dir_fd);
	return res;
}

static cc_result File_Do(cc_file* file, const cc_string* path, int mode) {
	char str[NATIVE_STR_LEN];
	GetNativePath(str, path);
	int fd = -1;
	
	int access = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	int res    = sysLv2FsOpen(str, mode, &fd, access, NULL, 0);
	
	if (res) {
		*file = -1; return res;
	} else {
		// TODO: is this actually needed?
		if (mode & SYS_O_CREAT) sysLv2FsChmod(str, access);
		*file = fd; return 0;
	}
}

cc_result File_Open(cc_file* file, const cc_string* path) {
	return File_Do(file, path, SYS_O_RDONLY);
}
cc_result File_Create(cc_file* file, const cc_string* path) {
	return File_Do(file, path, SYS_O_RDWR | SYS_O_CREAT | SYS_O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_string* path) {
	return File_Do(file, path, SYS_O_RDWR | SYS_O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	u64 read = 0;
	int res  = sysLv2FsRead(file, data, count, &read);
	
	*bytesRead = read;
	return res;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	u64 wrote = 0;
	int res   = sysLv2FsWrite(file, data, count, &wrote);
	
	*bytesWrote = wrote;
	return res;
}

cc_result File_Close(cc_file file) {
	return sysLv2FsClose(file);
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[] = { SEEK_SET, SEEK_CUR, SEEK_END };
	u64 position = 0;
	return sysLv2FsLSeek64(file, offset, modes[seekType], &position);
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	u64 position = 0;
	int res = sysLv2FsLSeek64(file, 0, SEEK_CUR, &position);
	
	*pos = position;
	return res;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	sysFSStat st;
	int res = sysLv2FsFStat(file, &st);
	
	*len = st.st_size;
	return res;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
void Thread_Sleep(cc_uint32 milliseconds) { 
	sysUsleep(milliseconds * 1000); 
}

static void ExecThread(void* param) {
	((Thread_StartFunc)param)(); 
}
#define STACK_SIZE (128 * 1024)

void* Thread_Create(Thread_StartFunc func) {
	return Mem_Alloc(1, sizeof(sys_ppu_thread_t), "thread");
}

void Thread_Start2(void* handle, Thread_StartFunc func) {
	sys_ppu_thread_t* thread = (sys_ppu_thread_t*)handle;
	int res = sysThreadCreate(thread, ExecThread, (void*)func,
			0, STACK_SIZE, THREAD_JOINABLE, "CC thread");
	if (res) Logger_Abort2(res, "Creating thread");
}

void Thread_Detach(void* handle) {
	sys_ppu_thread_t* thread = (sys_ppu_thread_t*)handle;
	int res = sysThreadDetach(*thread);
	if (res) Logger_Abort2(res, "Detaching thread");
	Mem_Free(thread);
}

void Thread_Join(void* handle) {
	u64 retVal;
	sys_ppu_thread_t* thread = (sys_ppu_thread_t*)handle;
	int res = sysThreadJoin(*thread, &retVal);
	if (res) Logger_Abort2(res, "Joining thread");
	Mem_Free(thread);
}

void* Mutex_Create(void) {
	sys_mutex_attr_t attr;	
	sysMutexAttrInitialize(attr);
	
	sys_mutex_t* mutex = (sys_mutex_t*)Mem_Alloc(1, sizeof(sys_mutex_t), "mutex");
	int res = sysMutexCreate(mutex, &attr);
	if (res) Logger_Abort2(res, "Creating mutex");
	return mutex;
}

void Mutex_Free(void* handle) {
	sys_mutex_t* mutex = (sys_mutex_t*)handle;
	int res = sysMutexDestroy(*mutex);
	if (res) Logger_Abort2(res, "Destroying mutex");
	Mem_Free(mutex);
}

void Mutex_Lock(void* handle) {
	sys_mutex_t* mutex = (sys_mutex_t*)handle;
	int res = sysMutexLock(*mutex, 0);
	if (res) Logger_Abort2(res, "Locking mutex");
}

void Mutex_Unlock(void* handle) {
	sys_mutex_t* mutex = (sys_mutex_t*)handle;
	int res = sysMutexUnlock(*mutex);
	if (res) Logger_Abort2(res, "Unlocking mutex");
}

void* Waitable_Create(void) {
	sys_sem_attr_t attr = { 0 };
	attr.attr_protocol  = SYS_SEM_ATTR_PROTOCOL;
	attr.attr_pshared   = SYS_SEM_ATTR_PSHARED; 
	
	sys_sem_t* sem = (sys_sem_t*)Mem_Alloc(1, sizeof(sys_sem_t), "waitable");
	int res = sysSemCreate(sem, &attr, 0, 1000000);
	if (res) Logger_Abort2(res, "Creating waitable");
	
	return sem;
}

void Waitable_Free(void* handle) {
	sys_sem_t* sem = (sys_sem_t*)handle;

	int res = sysSemDestroy(*sem);
	if (res) Logger_Abort2(res, "Destroying waitable");
	Mem_Free(sem);
}

void Waitable_Signal(void* handle) {
	sys_sem_t* sem = (sys_sem_t*)handle;
	int res = sysSemPost(*sem, 1);
	if (res) Logger_Abort2(res, "Signalling event");
}

void Waitable_Wait(void* handle) {
	sys_sem_t* sem = (sys_sem_t*)handle;
	int res = sysSemWait(*sem, 0);
	if (res) Logger_Abort2(res, "Waitable wait");
}

void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) {
	sys_sem_t* sem = (sys_sem_t*)handle;
	int res = sysSemWait(*sem, milliseconds * 1000);
	if (res) Logger_Abort2(res, "Waitable wait for");
}

/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Init(void) {
	netInitialize();
	// Create root directory
	Directory_Create(&String_Empty);
}

void Platform_Free(void) { }

cc_bool Platform_DescribeError(cc_result res, cc_string* dst) {
	char chars[NATIVE_STR_LEN];
	int len;

	/* For unrecognised error codes, strerror_r might return messages */
	/*  such as 'No error information', which is not very useful */
	/* (could check errno here but quicker just to skip entirely) */
	if (res >= 1000) return false;

	len = strerror_r(res, chars, NATIVE_STR_LEN);
	if (len == -1) return false;

	len = String_CalcLen(chars, NATIVE_STR_LEN);
	String_AppendUtf8(dst, chars, len);
	return true;
}


/*########################################################################################################################*
*-------------------------------------------------------Encryption--------------------------------------------------------*
*#########################################################################################################################*/
static cc_result GetMachineID(cc_uint32* key) {
	return ERR_NOT_SUPPORTED;
}
#endif