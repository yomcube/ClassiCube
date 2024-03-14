#include "Core.h"
#if defined CC_BUILD_WIIU

#include "_PlatformBase.h"
#include "Stream.h"
#include "ExtMath.h"
#include "SystemFonts.h"
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
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <signal.h>
#include <stdio.h>
#include <poll.h>
#include <netdb.h>
#include <coreinit/event.h>
#include <coreinit/fastmutex.h>
#include <coreinit/thread.h>
#include <coreinit/systeminfo.h>
#include <coreinit/time.h>
#include "_PlatformConsole.h"

const cc_result ReturnCode_FileShareViolation = 1000000000; /* TODO: not used apparently */
const cc_result ReturnCode_FileNotFound     = ENOENT;
const cc_result ReturnCode_SocketInProgess  = EINPROGRESS;
const cc_result ReturnCode_SocketWouldBlock = EWOULDBLOCK;
const cc_result ReturnCode_DirectoryExists  = EEXIST;

const char* Platform_AppNameSuffix = " Wii U";


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Log(const char* msg, int len) {
	char tmp[256 + 1];
	len = min(len, 256);
	Mem_Copy(tmp, msg, len); tmp[len] = '\0';
	
	OSReport("%s\n", tmp);
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
cc_uint64 Stopwatch_Measure(void) {
	return OSGetSystemTime(); // TODO OSGetTick ??
}

cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return OSTicksToMicroseconds(end - beg);
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
static void GetNativePath(char* str, const cc_string* path) {
	//const char* sd_root = WHBGetSdCardMountPath();
	//int sd_length = String_Length(sd_root);
	//Mem_Copy(str, sd_root, sd_length);
	//str   += sd_length;
	
	//*str++ = '/';
	String_EncodeUtf8(str, path);
}

cc_result Directory_Create(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	GetNativePath(str, path);
	/* read/write/search permissions for owner and group, and with read/search permissions for others. */
	/* TODO: Is the default mode in all cases */
	return mkdir(str, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1 ? errno : 0;
}

int File_Exists(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	struct stat sb;
	GetNativePath(str, path);
	return stat(str, &sb) == 0 && S_ISREG(sb.st_mode);
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	char str[NATIVE_STR_LEN];
	DIR* dirPtr;
	struct dirent* entry;
	char* src;
	int len, res, is_dir;

	GetNativePath(str, dirPath);
	dirPtr = opendir(str);
	if (!dirPtr) return errno;

	/* POSIX docs: "When the end of the directory is encountered, a null pointer is returned and errno is not changed." */
	/* errno is sometimes leftover from previous calls, so always reset it before readdir gets called */
	errno = 0;
	String_InitArray(path, pathBuffer);

	while ((entry = readdir(dirPtr))) {
		path.length = 0;
		String_Format1(&path, "%s/", dirPath);

		/* ignore . and .. entry */
		src = entry->d_name;
		if (src[0] == '.' && src[1] == '\0') continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;

		len = String_Length(src);
		String_AppendUtf8(&path, src, len);

		is_dir = entry->d_type == DT_DIR;
		/* TODO: fallback to stat when this fails */

		if (is_dir) {
			res = Directory_Enum(&path, obj, callback);
			if (res) { closedir(dirPtr); return res; }
		} else {
			callback(&path, obj);
		}
		errno = 0;
	}

	res = errno; /* return code from readdir */
	closedir(dirPtr);
	return res;
}

static cc_result File_Do(cc_file* file, const cc_string* path, int mode) {
	char str[NATIVE_STR_LEN];
	GetNativePath(str, path);
	*file = open(str, mode, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	return *file == -1 ? errno : 0;
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
	*bytesRead = read(file, data, count);
	return *bytesRead == -1 ? errno : 0;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	*bytesWrote = write(file, data, count);
	return *bytesWrote == -1 ? errno : 0;
}

cc_result File_Close(cc_file file) {
	return close(file) == -1 ? errno : 0;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	return lseek(file, offset, modes[seekType]) == -1 ? errno : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	*pos = lseek(file, 0, SEEK_CUR);
	return *pos == -1 ? errno : 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	struct stat st;
	if (fstat(file, &st) == -1) { *len = -1; return errno; }
	*len = st.st_size; return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
void Thread_Sleep(cc_uint32 milliseconds) { 
	OSSleepTicks(OSMillisecondsToTicks(milliseconds));
}

static int ExecThread(int argc, const char **argv) {
	((Thread_StartFunc)argv)(); 
	return 0;
}

#define STACK_SIZE 128 * 1024
void* Thread_Create(Thread_StartFunc func) {
	OSThread* thread = (OSThread*)Mem_Alloc(1, sizeof(OSThread), "thread");
	void* stack = memalign(16, STACK_SIZE);
	
	OSCreateThread(thread, ExecThread,
                       1, (Thread_StartFunc)func,
                       stack + STACK_SIZE, STACK_SIZE,
                       16, OS_THREAD_ATTRIB_AFFINITY_ANY);

	// TODO revisit this
	OSSetThreadRunQuantum(thread, 1000); // force yield after 1 millisecond
	return thread;
}

void Thread_Start2(void* handle, Thread_StartFunc func) {
	OSResumeThread((OSThread*)handle);
}

void Thread_Detach(void* handle) {
	OSDetachThread((OSThread*)handle);
}

void Thread_Join(void* handle) {
	int result;
	OSJoinThread((OSThread*)handle, &result);
}

void* Mutex_Create(void) {
	OSFastMutex* mutex = (OSFastMutex*)Mem_Alloc(1, sizeof(OSFastMutex), "mutex");
	
	OSFastMutex_Init(mutex, "CC mutex");
	return mutex;
}

void Mutex_Free(void* handle) {
	Mem_Free(handle);
}

void Mutex_Lock(void* handle) {
	OSFastMutex_Lock((OSFastMutex*)handle);
}

void Mutex_Unlock(void* handle) {
	OSFastMutex_Unlock((OSFastMutex*)handle);
}

void* Waitable_Create(void) {
	OSEvent* event = (OSEvent*)Mem_Alloc(1, sizeof(OSEvent), "waitable");

	OSInitEvent(event, false, OS_EVENT_MODE_AUTO);
	return event;
}

void Waitable_Free(void* handle) {
	OSResetEvent((OSEvent*)handle);
	Mem_Free(handle);
}

void Waitable_Signal(void* handle) {
	OSSignalEvent((OSEvent*)handle);
}

void Waitable_Wait(void* handle) {
	OSWaitEvent((OSEvent*)handle);
}

void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) {
	OSTime timeout = OSMillisecondsToTicks(milliseconds);
	OSWaitEventWithTimeout((OSEvent*)handle, timeout);
}


/*########################################################################################################################*
*---------------------------------------------------------Socket----------------------------------------------------------*
*#########################################################################################################################*/
union SocketAddress {
	struct sockaddr raw;
	struct sockaddr_in  v4;
	#ifdef AF_INET6
	struct sockaddr_in6 v6;
	struct sockaddr_storage total;
	#endif
};
/* Sanity check to ensure cc_sockaddr struct is large enough to contain all socket addresses supported by this platform */
static char sockaddr_size_check[sizeof(union SocketAddress) < CC_SOCKETADDR_MAXSIZE ? 1 : -1];

static cc_result ParseHost(const char* host, int port, cc_sockaddr* addrs, int* numValidAddrs) {
	char portRaw[32]; cc_string portStr;
	struct addrinfo hints = { 0 };
	struct addrinfo* result;
	struct addrinfo* cur;
	int res, i = 0;

	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	
	String_InitArray(portStr,  portRaw);
	String_AppendInt(&portStr, port);
	portRaw[portStr.length] = '\0';

	res = getaddrinfo(host, portRaw, &hints, &result);
	if (res == EAI_AGAIN) return SOCK_ERR_UNKNOWN_HOST;
	if (res) return res;

	/* Prefer IPv4 addresses first */
	for (cur = result; cur && i < SOCKET_MAX_ADDRS; cur = cur->ai_next) 
	{
		if (cur->ai_family != AF_INET) continue;
		Mem_Copy(addrs[i].data, cur->ai_addr, cur->ai_addrlen);
		addrs[i].size = cur->ai_addrlen; i++;
	}
	
	for (cur = result; cur && i < SOCKET_MAX_ADDRS; cur = cur->ai_next) 
	{
		if (cur->ai_family == AF_INET) continue;
		Mem_Copy(addrs[i].data, cur->ai_addr, cur->ai_addrlen);
		addrs[i].size = cur->ai_addrlen; i++;
	}

	freeaddrinfo(result);
	*numValidAddrs = i;
	return i == 0 ? ERR_INVALID_ARGUMENT : 0;
}

cc_result Socket_ParseAddress(const cc_string* address, int port, cc_sockaddr* addrs, int* numValidAddrs) {
	union SocketAddress* addr = (union SocketAddress*)addrs[0].data;
	char str[NATIVE_STR_LEN];

	String_EncodeUtf8(str, address);
	*numValidAddrs = 0;

	if (inet_pton(AF_INET,  str, &addr->v4.sin_addr)  > 0) {
		addr->v4.sin_family = AF_INET;
		addr->v4.sin_port   = htons(port);
		
		addrs[0].size  = sizeof(addr->v4);
		*numValidAddrs = 1;
		return 0;
	}
	
	#ifdef AF_INET6
	if (inet_pton(AF_INET6, str, &addr->v6.sin6_addr) > 0) {
		addr->v6.sin6_family = AF_INET6;
		addr->v6.sin6_port   = htons(port);
		
		addrs[0].size  = sizeof(addr->v6);
		*numValidAddrs = 1;
		return 0;
	}
	#endif
	
	return ParseHost(str, port, addrs, numValidAddrs);
}

cc_result Socket_Connect(cc_socket* s, cc_sockaddr* addr, cc_bool nonblocking) {
	struct sockaddr* raw = (struct sockaddr*)addr->data;
	cc_result res;

	*s = socket(raw->sa_family, SOCK_STREAM, IPPROTO_TCP);
	if (*s == -1) return errno;

	if (nonblocking) {
		int blocking_raw = -1; /* non-blocking mode */
		ioctl(*s, FIONBIO, &blocking_raw);
	}

	res = connect(*s, raw, addr->size);
	return res == -1 ? errno : 0;
}

cc_result Socket_Read(cc_socket s, cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	int recvCount = recv(s, data, count, 0);
	if (recvCount != -1) { *modified = recvCount; return 0; }
	*modified = 0; return errno;
}

cc_result Socket_Write(cc_socket s, const cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	int sentCount = send(s, data, count, 0);
	if (sentCount != -1) { *modified = sentCount; return 0; }
	*modified = 0; return errno;
}

void Socket_Close(cc_socket s) {
	shutdown(s, SHUT_RDWR);
	close(s);
}

static cc_result Socket_Poll(cc_socket s, int mode, cc_bool* success) {
	struct pollfd pfd;
	int flags;

	pfd.fd     = s;
	pfd.events = mode == SOCKET_POLL_READ ? POLLIN : POLLOUT;
	if (poll(&pfd, 1, 0) == -1) { *success = false; return errno; }
	
	/* to match select, closed socket still counts as readable */
	flags    = mode == SOCKET_POLL_READ ? (POLLIN | POLLHUP) : POLLOUT;
	*success = (pfd.revents & flags) != 0;
	return 0;
}

cc_result Socket_CheckReadable(cc_socket s, cc_bool* readable) {
	return Socket_Poll(s, SOCKET_POLL_READ, readable);
}

cc_result Socket_CheckWritable(cc_socket s, cc_bool* writable) {
	socklen_t resultSize = sizeof(socklen_t);
	cc_result res = Socket_Poll(s, SOCKET_POLL_WRITE, writable);
	if (res || *writable) return res;

	/* https://stackoverflow.com/questions/29479953/so-error-value-after-successful-socket-operation */
	getsockopt(s, SOL_SOCKET, SO_ERROR, &res, &resultSize);
	return res;
}


/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
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

void Platform_Init(void) {
	WHBProcInit();	
}


/*########################################################################################################################*
*-------------------------------------------------------Encryption--------------------------------------------------------*
*#########################################################################################################################*/
static cc_result GetMachineID(cc_uint32* key) { return ERR_NOT_SUPPORTED; }
#endif