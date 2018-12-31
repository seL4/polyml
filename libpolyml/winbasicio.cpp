/*
    Title:      Basic IO for Windows.

    Copyright (c) 2000, 2015-2018 David C. J. Matthews

    This was split from the common code for Unix and Windows.

    Portions of this code are derived from the original stream io
    package copyright CUTS 1983-2000.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x) assert(x)
#else
#define ASSERT(x) 0
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_DIRECT_H
#include <direct.h>
#endif
#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#include <limits>

#include <winsock2.h>
#include <tchar.h>

#ifndef INFTIM
#define INFTIM (-1)
#endif

#include <new>

#include "globals.h"
#include "basicio.h"
#include "sys.h"
#include "gc.h"
#include "run_time.h"
#include "machine_dep.h"
#include "arb.h"
#include "processes.h"
#include "diagnostics.h"
#include "io_internal.h"
#include "scanaddrs.h"
#include "polystring.h"
#include "mpoly.h"
#include "save_vec.h"
#include "rts_module.h"
#include "locking.h"
#include "rtsentry.h"
#include "timing.h"

#include "winguiconsole.h"
#define NOMEMORY ERROR_NOT_ENOUGH_MEMORY
#define STREAMCLOSED ERROR_INVALID_HANDLE
#define FILEDOESNOTEXIST ERROR_FILE_NOT_FOUND
#define ERRORNUMBER _doserrno

#ifndef O_ACCMODE
#define O_ACCMODE   (O_RDONLY|O_RDWR|O_WRONLY)
#endif

#define STREAMID(x) (DEREFSTREAMHANDLE(x)->streamNo)

#define SAVE(x) taskData->saveVec.push(x)

#ifdef _MSC_VER
// Don't tell me about ISO C++ changes.
#pragma warning(disable:4996)
#endif

extern "C" {
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyChDir(PolyObject *threadId, PolyWord arg);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyBasicIOGeneral(PolyObject *threadId, PolyWord code, PolyWord strm, PolyWord arg);
}

/*
I've tried various ways of getting asynchronous IO to work in a
consistent manner across different kinds of IO devices in Windows.
It is possible to pass some kinds of handles to WaitForMultipleObjects
but not all.  Anonymous pipes, for example, cannot be used in Windows 95
and don't seem to do what is expected in Windows NT (they return signalled
even when there is no input).  The console is even more of a mess. The
handle is signalled when there are any events (such as mouse movements)
available but these are ignored by ReadFile, which may then block.
Conversely using ReadFile to read less than a line causes the handle
to be unsignalled, supposedly meaning that no input is available, yet
ReadFile will return subsequent characters without blocking.  The eventual
solution was to replace the console completely.
DCJM May 2000
*/

// Standard streams.
static WinStream *standardInput, *standardOutput, *standardError;

int WinStream::fileTypeOfHandle(HANDLE hStream)
{
    switch (GetFileType(hStream))
    {
    case FILE_TYPE_PIPE: return FILEKIND_PIPE;
    case FILE_TYPE_CHAR: return FILEKIND_TTY;// Or a device?
    case FILE_TYPE_DISK: return FILEKIND_FILE;
    default:
        if (GetLastError() == 0)
            return FILEKIND_UNKNOWN; // Error or unknown.
        else return FILEKIND_ERROR;
    }
}

void WinStream::openEntry(TaskData * taskData, TCHAR *name, openMode mode, bool isAppend, bool isBinary)
{
    int oMode = 0;
    switch (mode)
    {
    case OPENREAD: oMode = O_RDONLY; break;
    case OPENWRITE:
        oMode = O_WRONLY | O_CREAT;
        if (isAppend) oMode |= O_APPEND; else oMode |= O_TRUNC;
        break;
        // We don't open for read/write in Windows.
    }
    if (isBinary) oMode |= O_BINARY;

    int stream = _topen(name, oMode);
    if (stream < 0)
        raise_syscall(taskData, "Cannot open", ERRORNUMBER);

    ioDesc = stream;
}

void WinStream::closeEntry(TaskData *taskData)
{
    if (close(ioDesc) < 0)
        raise_syscall(taskData, "Close failed", ERRORNUMBER);
}

int WinStream::fileKind()
{
    return fileTypeOfHandle((HANDLE)_get_osfhandle(ioDesc));
}

size_t WinStream::readStream(TaskData *taskData, byte *base, size_t length)
{
    ssize_t haveRead = read(ioDesc, base, (unsigned int)length);
    if (haveRead < 0)
        raise_syscall(taskData, "Error while reading", ERRORNUMBER);
    return (size_t)haveRead;
}

void WinStream::waitUntilAvailable(TaskData *taskData)
{
    while (!isAvailable(taskData))
    {
        WaitHandle waiter(NULL);
        processes->ThreadPauseForIO(taskData, &waiter);
    }
}

void WinStream::waitUntilOutputPossible(TaskData *taskData)
{
    while (!canOutput(taskData))
    {
        // Use the default waiter for the moment since we don't have
        // one to test for output.
        processes->ThreadPauseForIO(taskData, Waiter::defaultWaiter);
    }
}

void WinStream::unimplemented(TaskData *taskData)
{
    // Called on the random access functions
    raise_syscall(taskData, "Position error", ERROR_NOT_SUPPORTED);
}

size_t WinStream::writeStream(TaskData *taskData, byte *base, size_t length)
{
    ssize_t haveWritten = write(ioDesc, base, (unsigned int)length);
    if (haveWritten < 0) raise_syscall(taskData, "Error while writing", ERRORNUMBER);
    return (size_t)haveWritten;
}

void WinCopyInStream::closeEntry(TaskData *taskData)
{
    WinStream::closeEntry(taskData);
    CloseHandle(hInputAvailable);
}

bool WinCopyInStream::isAvailable(TaskData *taskData)
{
    HANDLE hFile = (HANDLE)_get_osfhandle(ioDesc);
    DWORD dwAvail;
    // hInputAvailable is set by the copy thread when it adds data.
    // We may not have read everything yet.  Reset the event first and
    // then set it if there is still data to read.  That way we avoid
    // a race condition if the copy thread is just adding data.
    ResetEvent(hInputAvailable);
    if (PeekNamedPipe(hFile, NULL, 0, NULL, &dwAvail, NULL) && dwAvail == 0)
        return false; // Succeeded and there really is nothing there.
                      // Something there or an error including "pipe-closed".
    SetEvent(hInputAvailable);
    return true;
}

void WinCopyInStream::waitUntilAvailable(TaskData *taskData)
{
    while (!isAvailable(taskData))
    {
        WaitHandle waiter(hInputAvailable);
        processes->ThreadPauseForIO(taskData, &waiter);
    }
}

/* Open a file in the required mode. */
static Handle openWinFile(TaskData *taskData, Handle filename, openMode mode, bool isAppend, bool isBinary)
{
    TempString cFileName(filename->Word()); // Get file name
    if (cFileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    try {
        if (mode == OPENREAD)
        {
            WinInStream *stream = new WinInStream();
            stream->openEntry(taskData, cFileName, !isBinary);
            return MakeVolatileWord(taskData, stream);
        }
        else
        {
            WinStream *stream = new WinStream();
            stream->openEntry(taskData, cFileName, mode, isAppend, isBinary);
            return MakeVolatileWord(taskData, stream);
        }
    }
    catch (std::bad_alloc&) {
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    }
}

WinInStream::WinInStream()
{
    hStream = hEvent = INVALID_HANDLE_VALUE;
    buffer = 0;
    currentInBuffer = currentPtr = 0;
    endOfStream = false;
    buffSize = 4096; // Seems like a good number
    ZeroMemory(&overlap, sizeof(overlap));
    isText = false;
}

WinInStream::~WinInStream()
{
    free(buffer);
}

void WinInStream::openEntry(TaskData * taskData, TCHAR *name, bool isT)
{
    isText = isT;
    ASSERT(hStream == INVALID_HANDLE_VALUE); // We should never reuse an object.
    buffer = (byte*)malloc(buffSize);
    if (buffer == 0)
        raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    // Create a manual reset event with state=signalled.  This means
    // that no operation is in progress.
    hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    overlap.hEvent = hEvent;
    hStream = CreateFile(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hStream == INVALID_HANDLE_VALUE)
        raise_syscall(taskData, "CreateFile failed", GetLastError());
    // Start a read immediately so that there is something in the buffer.
    beginReading(taskData);
}

// Start reading.  This may complete immediately.
void WinInStream::beginReading(TaskData *taskData)
{
    if (!ReadFile(hStream, buffer, buffSize, NULL, &overlap))
    {
        switch (GetLastError())
        {
        case ERROR_HANDLE_EOF:
            endOfStream = true;
        case ERROR_IO_PENDING:
            return;
        default:
            raise_syscall(taskData, "ReadFile failed", GetLastError());
        }
    }
}

void WinInStream::closeEntry(TaskData *taskData)
{
    PLocker locker(&lock);
    DWORD dwWait = WaitForSingleObject(hEvent, 0);
    if (dwWait == WAIT_FAILED)
        raise_syscall(taskData, "WaitForSingleObject failed", GetLastError());
    if (dwWait == WAIT_TIMEOUT)
    {
        // Something is in progress.
        CancelIoEx(hStream, &overlap);
    }
    CloseHandle(hStream);
    hStream = INVALID_HANDLE_VALUE;
    CloseHandle(hEvent);
    hEvent = INVALID_HANDLE_VALUE;
}

size_t WinInStream::readStream(TaskData *taskData, byte *base, size_t length)
{
    PLocker locker(&lock);
    if (endOfStream) return 0;
    size_t copied = 0;
    // Copy as much as we can from the buffer.
    while (currentPtr < currentInBuffer && copied < length)
    {
        byte b = buffer[currentPtr++];
        // In text mode we want to return NL for CRNL.  Assume that this is
        // properly formatted and simply skip CRs.  It's not clear what to return
        // if it isn't properly formatted and the user can always open it as binary
        // and do the conversion.
        if (!isText || b != '\r')
            base[copied++] = b;
    }
    // If we have exhausted the buffer we start a new read.
    while (isText && currentPtr < currentInBuffer && buffer[currentPtr] == '\r')
        currentPtr++;
    if (currentInBuffer == currentPtr)
    {
        // We need to start a new read
        currentInBuffer = currentPtr = 0;
        beginReading(taskData);
    }
    return copied;
}

// This actually does most of the work.  In particular for text streams we may have a
// block that consists only of CRs.
bool WinInStream::isAvailable(TaskData *taskData)
{
    while (1)
    {
        {
            PLocker locker(&lock);
            // It is available if we have something in the buffer or we're at EOF
            if (currentInBuffer < currentPtr || endOfStream)
                return true;
            // We should have had a read in progress.
            DWORD bytesRead = 0;
            if (!GetOverlappedResult(hStream, &overlap, &bytesRead, FALSE))
            {
                DWORD err = GetLastError();
                switch (err)
                {
                case ERROR_HANDLE_EOF:
                    // We've had EOF - That result is available
                    endOfStream = true;
                    return true;
                case ERROR_IO_INCOMPLETE:
                    // It's still in progress.
                    return false;
                default:
                    raise_syscall(taskData, "GetOverlappedResult failed", err);
                }
            }
            // The next read must be after this.
            setOverlappedPos(getOverlappedPos() + bytesRead);
            currentInBuffer = bytesRead;
            // If this is a text stream skip CRs.
            while (isText && currentPtr < currentInBuffer && buffer[currentPtr] == '\r')
                currentPtr++;
            // If we have some real data it can be read now
            if (currentPtr < currentInBuffer)
                return true;
        }
        // Try again.
        beginReading(taskData); // And loop
    }
}

void WinInStream::waitUntilAvailable(TaskData *taskData)
{
    while (!isAvailable(taskData))
    {
        WaitHandle waiter(hEvent);
        processes->ThreadPauseForIO(taskData, &waiter);
    }
}

// Random access functions
uint64_t WinInStream::getPos(TaskData *taskData)
{
    if (GetFileType(hStream) != FILE_TYPE_DISK)
        raise_syscall(taskData, "Stream is not a file", ERROR_SEEK_ON_DEVICE);
    PLocker locker(&lock);
    return getOverlappedPos() - currentInBuffer + currentPtr;
}

void WinInStream::setPos(TaskData *taskData, uint64_t pos)
{
    if (GetFileType(hStream) != FILE_TYPE_DISK)
        raise_syscall(taskData, "Stream is not a file", ERROR_SEEK_ON_DEVICE);
    PLocker locker(&lock);
    // Need to wait until any pending operation is complete.
    while (WaitForSingleObject(hEvent, 0) == WAIT_TIMEOUT)
    {
        WaitHandle waiter(hEvent);
        processes->ThreadPauseForIO(taskData, &waiter);
    }
    setOverlappedPos(pos);
    // Discard any unread data and start reading at the new position.
    currentInBuffer = currentPtr = 0;
    endOfStream = false;
    beginReading(taskData);
}

uint64_t WinInStream::fileSize(TaskData *taskData)
{
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hStream, &fileSize))
        raise_syscall(taskData, "Stream is not a file", GetLastError());
    return fileSize.QuadPart;
}


/* Read into an array. */
// We can't combine readArray and readString because we mustn't compute the
// destination of the data in readArray until after any GC.
static Handle readArray(TaskData *taskData, Handle stream, Handle args, bool/*isText*/)
{
    WinStream *strm = *(WinStream**)(stream->WordP());
    if (strm == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
    /* The isText argument is ignored in both Unix and Windows but
    is provided for future use.  Windows remembers the mode used
    when the file was opened to determine whether to translate
    CRLF into LF. */
    // We should check for interrupts even if we're not going to block.
    processes->TestAnyEvents(taskData);

    // First test to see if we have input available.
    // These tests may result in a GC if another thread is running.
    // First test to see if we have input available.
    // These tests may result in a GC if another thread is running.
    strm->waitUntilAvailable(taskData);

    // We can now try to read without blocking.
    // Actually there's a race here in the unlikely situation that there
    // are multiple threads sharing the same low-level reader.  They could
    // both detect that input is available but only one may succeed in
    // reading without blocking.  This doesn't apply where the threads use
    // the higher-level IO interfaces in ML which have their own mutexes.
    byte *base = DEREFHANDLE(args)->Get(0).AsObjPtr()->AsBytePtr();
    POLYUNSIGNED offset = getPolyUnsigned(taskData, DEREFWORDHANDLE(args)->Get(1));
    size_t length = getPolyUnsigned(taskData, DEREFWORDHANDLE(args)->Get(2));
    size_t haveRead = strm->readStream(taskData, base + offset, length);
    return Make_fixed_precision(taskData, haveRead); // Success.
}

/* Return input as a string. We don't actually need both readArray and
readString but it's useful to have both to reduce unnecessary garbage.
The IO library will construct one from the other but the higher levels
choose the appropriate function depending on need. */
static Handle readString(TaskData *taskData, Handle stream, Handle args, bool/*isText*/)
{
    size_t length = getPolyUnsigned(taskData, DEREFWORD(args));
    WinStream *strm;
    // Legacy: during the bootstrap we will have old format references.
    if (stream->Word().IsTagged() && stream->Word().UnTagged() == 0)
        strm = standardInput;
    else strm = *(WinStream**)(stream->WordP());
    if (strm == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);

    // We should check for interrupts even if we're not going to block.
    processes->TestAnyEvents(taskData);

    // First test to see if we have input available.
    // These tests may result in a GC if another thread is running.
    strm->waitUntilAvailable(taskData);

    // We can now try to read without blocking.
    // We previously allocated the buffer on the stack but that caused
    // problems with multi-threading at least on Mac OS X because of
    // stack exhaustion.  We limit the space to 100k. */
    if (length > 102400) length = 102400;
    byte *buff = (byte*)malloc(length);
    if (buff == 0) raise_syscall(taskData, "Unable to allocate buffer", NOMEMORY);

    try {
        size_t haveRead = strm->readStream(taskData, buff, length);
        Handle result = SAVE(C_string_to_Poly(taskData, (char*)buff, haveRead));
        free(buff);
        return result;
    }
    catch (...) {
        free(buff);
        throw;
    }
}

static Handle writeArray(TaskData *taskData, Handle stream, Handle args, bool/*isText*/)
{
    /* The isText argument is ignored in both Unix and Windows but
    is provided for future use.  Windows remembers the mode used
    when the file was opened to determine whether to translate
    LF into CRLF. */
    PolyWord base = DEREFWORDHANDLE(args)->Get(0);
    POLYUNSIGNED    offset = getPolyUnsigned(taskData, DEREFWORDHANDLE(args)->Get(1));
    size_t length = getPolyUnsigned(taskData, DEREFWORDHANDLE(args)->Get(2));
    WinStream *strm;
    // Legacy: We may have this during the bootstrap.
    if (stream->Word().IsTagged() && stream->Word().UnTagged() == 1)
        strm = standardOutput;
    else strm = *(WinStream**)(stream->WordP());
    if (strm == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
    /* We don't actually handle cases of blocking on output. */
    byte *toWrite = base.AsObjPtr()->AsBytePtr();
    size_t haveWritten = strm->writeStream(taskData, toWrite + offset, length);
    return Make_fixed_precision(taskData, haveWritten);
}

Handle pollTest(TaskData *taskData, Handle stream)
{
    WinStream *strm = *(WinStream**)(stream->WordP());
    return Make_fixed_precision(taskData, strm->pollTest());
}

// Do the polling.  Takes a vector of io descriptors, a vector of bits to test
// and a time to wait and returns a vector of results.

// Windows: This is messy because "select" only works on sockets.
// Do the best we can.
static Handle pollDescriptors(TaskData *taskData, Handle args, int blockType)
{
    Handle hSave = taskData->saveVec.mark();
TryAgain:
    PolyObject  *strmVec = DEREFHANDLE(args)->Get(0).AsObjPtr();
    PolyObject  *bitVec = DEREFHANDLE(args)->Get(1).AsObjPtr();
    POLYUNSIGNED nDesc = strmVec->Length();
    ASSERT(nDesc == bitVec->Length());
    // We should check for interrupts even if we're not going to block.
    processes->TestAnyEvents(taskData);

    /* Simply do a non-blocking poll. */
    /* Record the results in this vector. */
    char *results = 0;
    bool haveResult = false;
    Handle  resVec;
    if (nDesc > 0)
    {
        results = (char*)alloca(nDesc);
        memset(results, 0, nDesc);
    }

    for (POLYUNSIGNED i = 0; i < nDesc; i++)
    {
        WinStream *strm = *(WinStream**)(strmVec->Get(i).AsObjPtr());
        if (strm == NULL) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        int bits = get_C_int(taskData, bitVec->Get(i));
        int res = strm->poll(bits);
        if (res != 0)
            haveResult = true;
    }
    if (haveResult == 0)
    {
        /* Poll failed - treat as time out. */
        switch (blockType)
        {
        case 0: /* Check the time out. */
        {
            Handle hSave = taskData->saveVec.mark();
            /* The time argument is an absolute time. */
            FILETIME ftTime, ftNow;
            /* Get the file time. */
            getFileTimeFromArb(taskData, taskData->saveVec.push(DEREFHANDLE(args)->Get(2)), &ftTime);
            GetSystemTimeAsFileTime(&ftNow);
            taskData->saveVec.reset(hSave);
            /* If the timeout time is earlier than the current time
            we must return, otherwise we block. */
            if (CompareFileTime(&ftTime, &ftNow) <= 0)
                break; /* Return the empty set. */
                        /* else drop through and block. */
        }
        case 1: /* Block until one of the descriptors is ready. */
            processes->ThreadPause(taskData);
            taskData->saveVec.reset(hSave);
            goto TryAgain;
            /*NOTREACHED*/
        case 2: /* Just a simple poll - drop through. */
            break;
        }
    }
    /* Copy the results to a result vector. */
    resVec = alloc_and_save(taskData, nDesc);
    for (POLYUNSIGNED j = 0; j < nDesc; j++)
        (DEREFWORDHANDLE(resVec))->Set(j, TAGGED(results[j]));
    return resVec;
}

// Directory functions.
class WinDirData
{
public:
    HANDLE  hFind; /* FindFirstFile handle */
    WIN32_FIND_DATA lastFind;
    int fFindSucceeded;
};

static Handle openDirectory(TaskData *taskData, Handle dirname)
{
    // Get the directory name but add on two characters for the \* plus one for the NULL.
    POLYUNSIGNED length = PolyStringLength(dirname->Word());
    TempString dirName((TCHAR*)malloc((length + 3) * sizeof(TCHAR)));
    if (dirName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    Poly_string_to_C(dirname->Word(), dirName, length + 2);
    // Tack on \* to the end so that we find all files in the directory.
    lstrcat(dirName, _T("\\*"));
    WinDirData *pData = new WinDirData; // TODO: Handle failue
    HANDLE hFind = FindFirstFile(dirName, &pData->lastFind);
    if (hFind == INVALID_HANDLE_VALUE)
        raise_syscall(taskData, "FindFirstFile failed", GetLastError());
    pData->hFind = hFind;
    /* There must be at least one file which matched. */
    pData->fFindSucceeded = 1;
    return MakeVolatileWord(taskData, pData);
}


/* Return the next entry from the directory, ignoring current and
parent arcs ("." and ".." in Windows and Unix) */
Handle readDirectory(TaskData *taskData, Handle stream)
{
    WinDirData *pData = *(WinDirData**)(stream->WordP()); // In a Volatile
    if (pData == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
    Handle result = NULL;
    // The next entry to read is already in the buffer. FindFirstFile
    // both opens the directory and returns the first entry. If
    // fFindSucceeded is false we have already reached the end.
    if (!pData->fFindSucceeded)
        return SAVE(EmptyString(taskData));
    while (result == NULL)
    {
        WIN32_FIND_DATA *pFind = &(pData->lastFind);
        if (!((pFind->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            (lstrcmp(pFind->cFileName, _T(".")) == 0 ||
                lstrcmp(pFind->cFileName, _T("..")) == 0)))
        {
            result = SAVE(C_string_to_Poly(taskData, pFind->cFileName));
        }
        /* Get the next entry. */
        if (!FindNextFile(pData->hFind, pFind))
        {
            DWORD dwErr = GetLastError();
            if (dwErr == ERROR_NO_MORE_FILES)
            {
                pData->fFindSucceeded = 0;
                if (result == NULL) return SAVE(EmptyString(taskData));
            }
        }
    }
    return result;
}

Handle rewindDirectory(TaskData *taskData, Handle stream, Handle dirname)
{
    WinDirData *pData = *(WinDirData**)(stream->WordP()); // In a SysWord
    if (pData == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
    // There's no rewind - close and reopen.
    FindClose(pData->hFind);
    POLYUNSIGNED length = PolyStringLength(dirname->Word());
    TempString dirName((TCHAR*)malloc((length + 3) * sizeof(TCHAR)));
    if (dirName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    Poly_string_to_C(dirname->Word(), dirName, length + 2);
    // Tack on \* to the end so that we find all files in the directory.
    lstrcat(dirName, _T("\\*"));
    HANDLE hFind = FindFirstFile(dirName, &(pData->lastFind));
    if (hFind == INVALID_HANDLE_VALUE)
        raise_syscall(taskData, "FindFirstFile failed", GetLastError());
    pData->hFind = hFind;
    /* There must be at least one file which matched. */
    pData->fFindSucceeded = 1;
    return Make_fixed_precision(taskData, 0);
}

static Handle closeDirectory(TaskData *taskData, Handle stream)
{
    WinDirData *pData = *(WinDirData**)(stream->WordP()); // In a SysWord
    if (pData != 0)
    {
        FindClose(pData->hFind);
        delete(pData);
        *((WinDirData**)stream->WordP()) = 0; // Clear this - no longer valid
    }
    return Make_fixed_precision(taskData, 0);
}

/* change_dirc - this is called directly and not via the dispatch
   function. */
static Handle change_dirc(TaskData *taskData, Handle name)
/* Change working directory. */
{
    TempString cDirName(name->Word());
    if (cDirName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    if (SetCurrentDirectory(cDirName) == FALSE)
       raise_syscall(taskData, "SetCurrentDirectory failed", GetLastError());
    return SAVE(TAGGED(0));
}

// External call
POLYUNSIGNED PolyChDir(PolyObject *threadId, PolyWord arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedArg = taskData->saveVec.push(arg);

    try {
        (void)change_dirc(taskData, pushedArg);
    } catch (...) { } // If an ML exception is raised

    taskData->saveVec.reset(reset); // Ensure the save vec is reset
    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned(); // Result is unit
}


/* Test for a directory. */
Handle isDir(TaskData *taskData, Handle name)
{
    TempString cDirName(name->Word());
    if (cDirName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    DWORD dwRes = GetFileAttributes(cDirName);
    if (dwRes == 0xFFFFFFFF)
        raise_syscall(taskData, "GetFileAttributes failed", GetLastError());
    if (dwRes & FILE_ATTRIBUTE_DIRECTORY)
        return Make_fixed_precision(taskData, 1);
    else return Make_fixed_precision(taskData, 0);
}

/* Get absolute canonical path name. */
Handle fullPath(TaskData *taskData, Handle filename)
{
    TempString cFileName;

    /* Special case of an empty string. */
    if (PolyStringLength(filename->Word()) == 0) cFileName = _tcsdup(_T("."));
    else cFileName = Poly_string_to_T_alloc(filename->Word());
    if (cFileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    // Get the length
    DWORD dwRes = GetFullPathName(cFileName, 0, NULL, NULL);
    if (dwRes == 0)
        raise_syscall(taskData, "GetFullPathName failed", GetLastError());
    TempString resBuf((TCHAR*)malloc(dwRes * sizeof(TCHAR)));
    if (resBuf == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    // When the length is enough the result is the length excluding the null
    DWORD dwRes1 = GetFullPathName(cFileName, dwRes, resBuf, NULL);
    if (dwRes1 == 0 || dwRes1 >= dwRes)
        raise_syscall(taskData, "GetFullPathName failed", GetLastError());
    /* Check that the file exists.  GetFullPathName doesn't do that. */
    dwRes = GetFileAttributes(resBuf);
    if (dwRes == 0xffffffff)
        raise_syscall(taskData, "File does not exist", FILEDOESNOTEXIST);
    return(SAVE(C_string_to_Poly(taskData, resBuf)));
}

/* Get file modification time.  This returns the value in the
   time units and from the base date used by timing.c. c.f. filedatec */
Handle modTime(TaskData *taskData, Handle filename)
{
    TempString cFileName(filename->Word());
    if (cFileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    /* There are two ways to get this information.
        We can either use GetFileTime if we are able
        to open the file for reading but if it is locked
        we won't be able to.  FindFirstFile is the other
        alternative.  We have to check that the file name
        does not contain '*' or '?' otherwise it will try
        to "glob" this, which isn't what we want here. */
    WIN32_FIND_DATA wFind;
    HANDLE hFind;
    const TCHAR *p;
    for(p = cFileName; *p; p++)
        if (*p == '*' || *p == '?')
            raise_syscall(taskData, "Invalid filename", STREAMCLOSED);
    hFind = FindFirstFile(cFileName, &wFind);
    if (hFind == INVALID_HANDLE_VALUE)
        raise_syscall(taskData, "FindFirstFile failed", GetLastError());
    FindClose(hFind);
    return Make_arb_from_Filetime(taskData, wFind.ftLastWriteTime);
}

/* Get file size. */
Handle fileSize(TaskData *taskData, Handle filename)
{
    TempString cFileName(filename->Word());
    if (cFileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    /* Similar to modTime*/
    WIN32_FIND_DATA wFind;
    HANDLE hFind;
    const TCHAR *p;
    for(p = cFileName; *p; p++)
        if (*p == '*' || *p == '?')
            raise_syscall(taskData, "Invalid filename", STREAMCLOSED);
    hFind = FindFirstFile(cFileName, &wFind);
    if (hFind == INVALID_HANDLE_VALUE)
        raise_syscall(taskData, "FindFirstFile failed", GetLastError());
    FindClose(hFind);
    return Make_arb_from_32bit_pair(taskData, wFind.nFileSizeHigh, wFind.nFileSizeLow);
}

/* Set file modification and access times. */
Handle setTime(TaskData *taskData, Handle fileName, Handle fileTime)
{
    TempString cFileName(fileName->Word());
    if (cFileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);

    // The only way to set the time is to open the file and use SetFileTime.
    FILETIME ft;
    /* Get the file time. */
    getFileTimeFromArb(taskData, fileTime, &ft);
    /* Open an existing file with write access. We need that
        for SetFileTime. */
    HANDLE hFile = CreateFile(cFileName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        raise_syscall(taskData, "CreateFile failed", GetLastError());
    /* Set the file time. */
    if (!SetFileTime(hFile, NULL, &ft, &ft))
    {
        int nErr = GetLastError();
        CloseHandle(hFile);
        raise_syscall(taskData, "SetFileTime failed", nErr);
    }
    CloseHandle(hFile);
    return Make_fixed_precision(taskData, 0);
}

/* Rename a file. */
Handle renameFile(TaskData *taskData, Handle oldFileName, Handle newFileName)
{
    TempString oldName(oldFileName->Word()), newName(newFileName->Word());
    if (oldName == 0 || newName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    if (! MoveFileEx(oldName, newName, MOVEFILE_REPLACE_EXISTING))
        raise_syscall(taskData, "MoveFileEx failed", GetLastError());
    return Make_fixed_precision(taskData, 0);
}

/* Access right requests passed in from ML. */
#define FILE_ACCESS_READ    1
#define FILE_ACCESS_WRITE   2
#define FILE_ACCESS_EXECUTE 4

/* Get access rights to a file. */
Handle fileAccess(TaskData *taskData, Handle name, Handle rights)
{
    TempString fileName(name->Word());
    if (fileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
    int rts = get_C_int(taskData, DEREFWORD(rights));

    /* Test whether the file is read-only.  This is, of course,
        not what was asked but getting anything more is really
        quite complicated.  I don't see how we can find out if
        a file is executable (maybe check if the extension is
        .exe, .com or .bat?).  It would be possible, in NT, to
        examine the access structures but that seems far too
        complicated.  Leave it for the moment. */
    DWORD dwRes = GetFileAttributes(fileName);
    if (dwRes == 0xffffffff)
        return Make_fixed_precision(taskData, 0);
    /* If we asked for write access but it is read-only we
        return false. */
    if ((dwRes & FILE_ATTRIBUTE_READONLY) &&
        (rts & FILE_ACCESS_WRITE))
        return Make_fixed_precision(taskData, 0);
    else return Make_fixed_precision(taskData, 1);
}



/* IO_dispatchc.  Called from assembly code module. */
static Handle IO_dispatch_c(TaskData *taskData, Handle args, Handle strm, Handle code)
{
    unsigned c = get_C_unsigned(taskData, DEREFWORD(code));
    switch (c)
    {
    case 0: // Return standard input. 
            // N.B.  If these next functions are called again we will have multiple references.
        return MakeVolatileWord(taskData, standardInput);
    case 1: /* Return standard output */
        return MakeVolatileWord(taskData, standardOutput);
    case 2: /* Return standard error */
        return MakeVolatileWord(taskData, standardError);
    case 3: /* Open file for text input. */
        return openWinFile(taskData, args, OPENREAD, false, false);
    case 4: /* Open file for binary input. */
        return openWinFile(taskData, args, OPENREAD, false, true);
    case 5: /* Open file for text output. */
        return openWinFile(taskData, args, OPENWRITE, false, false);
    case 6: /* Open file for binary output. */
        return openWinFile(taskData, args, OPENWRITE, false, true);
    case 13: /* Open text file for appending. */
             /* The IO library definition leaves it open whether this
             should use "append mode" or not.  */
        return openWinFile(taskData, args, OPENWRITE, true, false);
    case 14: /* Open binary file for appending. */
        return openWinFile(taskData, args, OPENWRITE, true, true);
    case 7: /* Close file */
    {
        // During the bootstrap we will have old format references.
        if (strm->Word().IsTagged())
            return Make_fixed_precision(taskData, 0);
        WinStream *stream = *(WinStream **)(strm->WordP());
        // Mustn't delete the standard streams.  At least during bootstrapping we can return
        // multiple references to them.
        if (stream != 0 && stream != standardInput &&
            stream != standardOutput && stream != standardError)
        {
            stream->closeEntry(taskData);
            // TODO: If there was an error it could have raised an exception.
            delete(stream);
            *(WinStream **)(strm->WordP()) = 0;
        }
        return Make_fixed_precision(taskData, 0);
    }
    case 8: /* Read text into an array. */
        return readArray(taskData, strm, args, true);
    case 9: /* Read binary into an array. */
        return readArray(taskData, strm, args, false);
    case 10: /* Get text as a string. */
        return readString(taskData, strm, args, true);
    case 11: /* Write from memory into a text file. */
        return writeArray(taskData, strm, args, true);
    case 12: /* Write from memory into a binary file. */
        return writeArray(taskData, strm, args, false);
    case 15: /* Return recommended buffer size. */
             // This is a guess but 4k seems reasonable.
        return Make_fixed_precision(taskData, 4096);

    case 16: /* See if we can get some input. */
    {
        WinStream *stream = *(WinStream **)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        return Make_fixed_precision(taskData, stream->isAvailable(taskData) ? 1 : 0);
    }

    case 17: // Return the number of bytes available. PrimIO.avail.
    {
        WinStream *stream = *(WinStream**)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        uint64_t endOfStream = stream->fileSize(taskData); // May raise an exception if this isn't a file.
        uint64_t current = stream->getPos(taskData);
        return Make_fixed_precision(taskData, endOfStream - current);
    }

    case 18: // Get position on stream.  PrimIO.getPos
    {
        WinStream *stream = *(WinStream**)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        // Get the current position in the stream.  This is used to test
        // for the availability of random access so it should raise an
        // exception if setFilePos or endFilePos would fail. 
        return Make_arbitrary_precision(taskData, stream->getPos(taskData));
    }

    case 19: // Seek to position on stream.  PrimIO.setPos
    {
        WinStream *stream = *(WinStream**)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        // TODO: This doesn't necessarily return a 64-bit value.
        uint64_t position = (uint64_t)getPolyUnsigned(taskData, DEREFWORD(args));
        stream->setPos(taskData, position);
        return Make_arbitrary_precision(taskData, 0);
    }

    case 20: // Return position at end of stream.  PrimIO.endPos.
    {
        WinStream *stream = *(WinStream**)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        return Make_arbitrary_precision(taskData, stream->fileSize(taskData));
    }

    case 21: /* Get the kind of device underlying the stream. */
    {
        WinStream *stream = *(WinStream**)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        return Make_fixed_precision(taskData, stream->fileKind());
    }
    case 22: /* Return the polling options allowed on this descriptor. */
        return pollTest(taskData, strm);
    case 23: /* Poll the descriptor, waiting forever. */
        return pollDescriptors(taskData, args, 1);
    case 24: /* Poll the descriptor, waiting for the time requested. */
        return pollDescriptors(taskData, args, 0);
    case 25: /* Poll the descriptor, returning immediately.*/
        return pollDescriptors(taskData, args, 2);
    case 26: /* Get binary as a vector. */
        return readString(taskData, strm, args, false);

    case 27: /* Block until input is available. */
    {
        WinStream *stream = *(WinStream **)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        // We should check for interrupts even if we're not going to block.
        processes->TestAnyEvents(taskData);
        stream->waitUntilAvailable(taskData);
        return Make_fixed_precision(taskData, 0);
    }

    case 28: /* Test whether output is possible. */
    {
        WinStream *stream = *(WinStream **)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        return Make_fixed_precision(taskData, stream->canOutput(taskData) ? 1 : 0);
    }

    case 29: /* Block until output is possible. */
    {
        WinStream *stream = *(WinStream **)(strm->WordP());
        if (stream == 0) raise_syscall(taskData, "Stream is closed", STREAMCLOSED);
        // We should check for interrupts even if we're not going to block.
        processes->TestAnyEvents(taskData);
        // This doesn't actually do anything in Windows.
        stream->waitUntilOutputPossible(taskData);
        return Make_fixed_precision(taskData, 0);
    }

    /* Functions added for Posix structure. */
    case 30: /* Return underlying file descriptor. */
    {
        // Legacy:  This was previously used LibrarySupport.wrapInFileDescr
        // to see if a stream was one of the standard streams.
        if (strm->Word().IsTagged())
            return strm;
        else
        {
            WinStream *stream = *(WinStream**)(strm->WordP());
            if (stream == standardInput)
                return Make_fixed_precision(taskData, 0);
            else if (stream == standardOutput)
                return Make_fixed_precision(taskData, 1);
            else if (stream == standardError)
                return Make_fixed_precision(taskData, 2);
            else return Make_fixed_precision(taskData, 3 /* > 2 */);
        }
    }

    /* Directory functions. */
    case 50: /* Open a directory. */
        return openDirectory(taskData, args);

    case 51: /* Read a directory entry. */
        return readDirectory(taskData, strm);

    case 52: /* Close the directory */
        return closeDirectory(taskData, strm);

    case 53: /* Rewind the directory. */
        return rewindDirectory(taskData, strm, args);

    case 54: /* Get current working directory. */
    {
        DWORD space = GetCurrentDirectory(0, NULL);
        if (space == 0)
            raise_syscall(taskData, "GetCurrentDirectory failed", GetLastError());
        TempString buff((TCHAR*)malloc(space * sizeof(TCHAR)));
        if (buff == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
        if (GetCurrentDirectory(space, buff) == 0)
            raise_syscall(taskData, "GetCurrentDirectory failed", GetLastError());
        return SAVE(C_string_to_Poly(taskData, buff));
    }

    case 55: /* Create a new directory. */
    {
        TempString dirName(args->Word());
        if (dirName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
        if (!CreateDirectory(dirName, NULL))
            raise_syscall(taskData, "CreateDirectory failed", GetLastError());
        return Make_fixed_precision(taskData, 0);
    }

    case 56: /* Delete a directory. */
    {
        TempString dirName(args->Word());
        if (dirName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
        if (!RemoveDirectory(dirName))
            raise_syscall(taskData, "RemoveDirectory failed", GetLastError());
        return Make_fixed_precision(taskData, 0);
    }

    case 57: /* Test for directory. */
        return isDir(taskData, args);

    case 58: /* Test for symbolic link. */
    {
        TempString fileName(args->Word());
        if (fileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
        DWORD dwRes = GetFileAttributes(fileName);
        if (dwRes == 0xFFFFFFFF)
            raise_syscall(taskData, "GetFileAttributes failed", GetLastError());
        return Make_fixed_precision(taskData, (dwRes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0);
    }

    case 59: /* Read a symbolic link. */
    {
        // Windows has added symbolic links but reading the target is far from
        // straightforward.   It's probably not worth trying to implement this.
        raise_syscall(taskData, "Symbolic links are not implemented", 0);
        return taskData->saveVec.push(TAGGED(0)); /* To keep compiler happy. */
    }

    case 60: /* Return the full absolute path name. */
        return fullPath(taskData, args);

    case 61: /* Modification time. */
        return modTime(taskData, args);

    case 62: /* File size. */
        return fileSize(taskData, args);

    case 63: /* Set file time. */
        return setTime(taskData, strm, args);

    case 64: /* Delete a file. */
    {
        TempString fileName(args->Word());
        if (fileName == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
        if (!DeleteFile(fileName))
            raise_syscall(taskData, "DeleteFile failed", GetLastError());
        return Make_fixed_precision(taskData, 0);
    }

    case 65: /* rename a file. */
        return renameFile(taskData, strm, args);

    case 66: /* Get access rights. */
        return fileAccess(taskData, strm, args);

    case 67: /* Return a temporary file name. */
    {
        DWORD dwSpace = GetTempPath(0, NULL);
        if (dwSpace == 0)
            raise_syscall(taskData, "GetTempPath failed", GetLastError());
        TempString buff((TCHAR*)malloc((dwSpace + 12) * sizeof(TCHAR)));
        if (buff == 0) raise_syscall(taskData, "Insufficient memory", NOMEMORY);
        if (GetTempPath(dwSpace, buff) == 0)
            raise_syscall(taskData, "GetTempPath failed", GetLastError());
        lstrcat(buff, _T("MLTEMPXXXXXX"));
#if (defined(HAVE_MKSTEMP) && ! defined(UNICODE))
        // mkstemp is present in the Mingw64 headers but only as ANSI not Unicode.
        // Set the umask to mask out access by anyone else.
        // mkstemp generally does this anyway.
        mode_t oldMask = umask(0077);
        int fd = mkstemp(buff);
        int wasError = ERRORNUMBER;
        (void)umask(oldMask);
        if (fd != -1) close(fd);
        else raise_syscall(taskData, "mkstemp failed", wasError);
#else
        if (_tmktemp(buff) == 0)
            raise_syscall(taskData, "mktemp failed", ERRORNUMBER);
        int fd = _topen(buff, O_RDWR | O_CREAT | O_EXCL, 00600);
        if (fd != -1) close(fd);
        else raise_syscall(taskData, "Temporary file creation failed", ERRORNUMBER);
#endif
        Handle res = SAVE(C_string_to_Poly(taskData, buff));
        return res;
    }

    case 68: /* Get the file id. */
    {
        /* This concept does not exist in Windows. */
        /* Return a negative number. This is interpreted
        as "not implemented". */
        return Make_fixed_precision(taskData, -1);
    }

    case 69:
        // Return an index for a token.  It is used in OS.IO.hash.
        return Make_fixed_precision(taskData, STREAMID(strm));

    default:
    {
        char msg[100];
        sprintf(msg, "Unknown io function: %d", c);
        raise_exception_string(taskData, EXC_Fail, msg);
        return 0;
    }
    }
}

// General interface to IO.  Ideally the various cases will be made into
// separate functions.
POLYUNSIGNED PolyBasicIOGeneral(PolyObject *threadId, PolyWord code, PolyWord strm, PolyWord arg)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();
    Handle reset = taskData->saveVec.mark();
    Handle pushedCode = taskData->saveVec.push(code);
    Handle pushedStrm = taskData->saveVec.push(strm);
    Handle pushedArg = taskData->saveVec.push(arg);
    Handle result = 0;

    try {
        result = IO_dispatch_c(taskData, pushedArg, pushedStrm, pushedCode);
    }
    catch (KillException &) {
        processes->ThreadExit(taskData); // TestAnyEvents may test for kill
    }
    catch (...) {} // If an ML exception is raised

    taskData->saveVec.reset(reset);
    taskData->PostRTSCall();
    if (result == 0) return TAGGED(0).AsUnsigned();
    else return result->Word().AsUnsigned();
}

struct _entrypts basicIOEPT[] =
{
    { "PolyChDir",                      (polyRTSFunction)&PolyChDir },
    { "PolyBasicIOGeneral",             (polyRTSFunction)&PolyBasicIOGeneral },

    { NULL, NULL } // End of list.
};

class BasicIO : public RtsModule
{
public:
    virtual void Start(void);
};

// Declare this.  It will be automatically added to the table.
static BasicIO basicIOModule;

void BasicIO::Start(void)
{
    standardInput = stdInStream; // Created in Console
    standardOutput = new WinStream(1);
    standardError = new WinStream(2);
}
