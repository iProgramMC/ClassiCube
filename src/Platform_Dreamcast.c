#include "Core.h"
#if defined CC_BUILD_DREAMCAST
#include "_PlatformBase.h"
#include "Stream.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Window.h"
#include "Utils.h"
#include "Errors.h"

#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>
#include <kos.h>
#include "_PlatformConsole.h"
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

const cc_result ReturnCode_FileShareViolation = 1000000000; // not used
const cc_result ReturnCode_FileNotFound     = ENOENT;
const cc_result ReturnCode_SocketInProgess = 567456;
const cc_result ReturnCode_SocketWouldBlock = 675364;
const cc_result ReturnCode_DirectoryExists  = EEXIST;
const char* Platform_AppNameSuffix = " Dreamcast";


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return end - beg;
}

cc_uint64 Stopwatch_Measure(void) {
	return timer_us_gettime64();
}

void Platform_Log(const char* msg, int len) {
	fs_write(STDOUT_FILENO, msg,  len);
	fs_write(STDOUT_FILENO, "\n",   1);
}

TimeMS DateTime_CurrentUTC_MS(void) {
	uint32 secs, ms;
	timer_ms_gettime(&secs, &ms);
	
	// TODO: Can we get away with not adding boot time.. ?
	return (rtc_boot_time() + secs) * 1000 + ms;
}

void DateTime_CurrentLocal(struct DateTime* t) {
	uint32 secs, ms;
	time_t total_secs;
	struct tm loc_time;
	
	timer_ms_gettime(&secs, &ms);
	total_secs = rtc_boot_time() + secs;
	localtime_r(&total_secs, &loc_time);

	t->year   = loc_time.tm_year + 1900;
	t->month  = loc_time.tm_mon  + 1;
	t->day    = loc_time.tm_mday;
	t->hour   = loc_time.tm_hour;
	t->minute = loc_time.tm_min;
	t->second = loc_time.tm_sec;
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
static const cc_string root_path = String_FromConst("/cd/");

static void GetNativePath(char* str, const cc_string* path) {
	Mem_Copy(str, root_path.buffer, root_path.length);
	str += root_path.length;
	String_EncodeUtf8(str, path);
}

cc_result Directory_Create(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	GetNativePath(str, path);
	
	int res = fs_mkdir(str);
	return res == -1 ? errno : 0;
}

int File_Exists(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	struct stat sb;
	GetNativePath(str, path);
	return fs_stat(str, &sb, 0) == 0 && S_ISREG(sb.st_mode);
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	char str[NATIVE_STR_LEN];
	int res;

	GetNativePath(str, dirPath);
	int fd = fs_open(str, O_DIR | O_RDONLY);
	if (fd < 0) return errno;

	String_InitArray(path, pathBuffer);
	dirent_t* entry;
	errno = 0;
	
	while ((entry = fs_readdir(fd))) {
		path.length = 0;
		String_Format1(&path, "%s/", dirPath);

		// ignore . and .. entry (PSP does return them)
		// TODO: Does Dreamcast?
		char* src = entry->name;
		if (src[0] == '.' && src[1] == '\0')                  continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;
		
		int len = String_Length(src);
		String_AppendUtf8(&path, src, len);

		// negative size indicates a directory entry
		if (entry->size < 0) {
			res = Directory_Enum(&path, obj, callback);
			if (res) break;
		} else {
			callback(&path, obj);
		}
	}

	int err = errno; // save error from fs_readdir
	fs_close(fd);
	return err;
}

static cc_result File_Do(cc_file* file, const cc_string* path, int mode) {
	char str[NATIVE_STR_LEN];
	GetNativePath(str, path);
	
	int res = fs_open(str, mode);
	*file   = res;
	return res == -1 ? errno : 0;
}

cc_result File_Open(cc_file* file, const cc_string* path) {
	return File_Do(file, path, O_RDONLY);
}
cc_result File_Create(cc_file* file, const cc_string* path) {
	return File_Do(file, path, O_RDWR | O_CREAT | O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_string* path) {
	return File_Do(file, path, O_RDWR | O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	int res    = fs_read(file, data, count);
	*bytesRead = res;
	return res == -1 ? errno : 0;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	int res     = fs_write(file, data, count);
	*bytesWrote = res;
	return res == -1 ? errno : 0;
}

cc_result File_Close(cc_file file) {
	int res = fs_close(file);
	return res == -1 ? errno : 0;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	
	int res = fs_seek(file, offset, modes[seekType]);
	return res == -1 ? errno : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	int res = fs_seek(file, 0, SEEK_CUR);
	*pos    = res;
	return res == -1 ? errno : 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	int res = fs_total(file);
	*len    = res;
	return res == -1 ? errno : 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
// !!! NOTE: Dreamcast uses cooperative multithreading (not preemptive multithreading) !!!
void Thread_Sleep(cc_uint32 milliseconds) { 
	thd_sleep(milliseconds); 
}

static void* ExecThread(void* param) {
	Thread_StartFunc func = (Thread_StartFunc)param;
	(func)();
	return NULL;
}

void* Thread_Create(Thread_StartFunc func) {
	kthread_attr_t attrs = { 0 };
	attrs.stack_size     = 64 * 1024;
	attrs.label          = "CC thread";
	return thd_create_ex(&attrs, ExecThread, func);
}

void Thread_Start2(void* handle, Thread_StartFunc func) {
}

void Thread_Detach(void* handle) {
	thd_detach((kthread_t*)handle);
}

void Thread_Join(void* handle) {
	thd_join((kthread_t*)handle, NULL);
}

void* Mutex_Create(void) {
	mutex_t* ptr = (mutex_t*)Mem_Alloc(1, sizeof(mutex_t), "mutex");
	int res = mutex_init(ptr, MUTEX_TYPE_NORMAL);
	if (res) Logger_Abort2(errno, "Creating mutex");
	return ptr;
}

void Mutex_Free(void* handle) {
	int res = mutex_destroy((mutex_t*)handle);
	if (res) Logger_Abort2(errno, "Destroying mutex");
	Mem_Free(handle);
}

void Mutex_Lock(void* handle) {
	int res = mutex_lock((mutex_t*)handle);
	if (res) Logger_Abort2(errno, "Locking mutex");
}

void Mutex_Unlock(void* handle) {
	int res = mutex_unlock((mutex_t*)handle);
	if (res) Logger_Abort2(errno, "Unlocking mutex");
}

void* Waitable_Create(void) {
	semaphore_t* ptr = (semaphore_t*)Mem_Alloc(1, sizeof(semaphore_t), "waitable");
	int res = sem_init(ptr, 0);
	if (res) Logger_Abort2(errno, "Creating waitable");
	return ptr;
}

void Waitable_Free(void* handle) {
	int res = sem_destroy((semaphore_t*)handle);
	if (res) Logger_Abort2(errno, "Destroying waitable");
	Mem_Free(handle);
}

void Waitable_Signal(void* handle) {
	int res = sem_signal((semaphore_t*)handle);
	if (res < 0) Logger_Abort2(errno, "Signalling event");
}

void Waitable_Wait(void* handle) {
	int res = sem_wait((semaphore_t*)handle);
	if (res < 0) Logger_Abort2(errno, "Event wait");
}

void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) {
	int res = sem_wait_timed((semaphore_t*)handle, milliseconds);
	if (res >= 0) return;
	
	int err = errno;
	if (err != ETIMEDOUT) Logger_Abort2(err, "Event timed wait");
}



/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Init(void) {
	char cwd[600] = { 0 };
	char* ptr = getcwd(cwd, 600);
	Platform_Log1("WORKING DIR: %c", ptr);
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