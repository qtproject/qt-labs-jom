/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of jom.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#define _CRT_RAND_S
#include <cstdlib>

#include "jomprocess.h"
#include "helperfunctions.h"
#include "iocompletionport.h"

#include <QByteArray>
#include <QEventLoop>
#include <QDir>
#include <QElapsedTimer>
#include <QMap>
#include <QMetaType>
#include <QMutex>
#include <QSysInfo>
#include <QTimer>
#include <QWinEventNotifier>

#include <qt_windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x08
#endif

namespace NMakeFile {

Q_GLOBAL_STATIC(QElapsedTimer, runtime)

struct Pipe
{
    Pipe()
        : hWrite(INVALID_HANDLE_VALUE)
        , hRead(INVALID_HANDLE_VALUE)
    {
        ZeroMemory(&overlapped, sizeof(overlapped));
    }

    ~Pipe()
    {
        if (hWrite != INVALID_HANDLE_VALUE)
            CloseHandle(hWrite);
        if (hRead != INVALID_HANDLE_VALUE)
            CloseHandle(hRead);
    }

    HANDLE hWrite;
    HANDLE hRead;
    OVERLAPPED overlapped;
};

static void safelyCloseHandle(HANDLE &h)
{
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
}

struct TimeStampedBuffer
{
    TimeStampedBuffer(const qint64 t, const QByteArray &b)
        : timestamp(t), buffer(b)
    {
    }

    qint64 timestamp;
    QByteArray buffer;
};

class ProcessPrivate;

class OutputChannel : public IoCompletionPortObserver
{
public:
    bool startRead();
    void completionPortNotified(DWORD numberOfBytes, DWORD errorCode);

    ProcessPrivate *d;
    Pipe *pipe;
    FILE *stream;
    QByteArray intermediateOutputBuffer;
    QList<TimeStampedBuffer> buffers;
    QMutex outputBufferLock;
};

class ProcessPrivate
{
public:
    ProcessPrivate(Process *process)
        : q(process),
          hProcess(INVALID_HANDLE_VALUE),
          hProcessThread(INVALID_HANDLE_VALUE),
          exitCode(STILL_ACTIVE)
    {
        stdoutChannel.d = this;
        stdoutChannel.pipe = &stdoutPipe;
        stdoutChannel.stream = stdout;
        stderrChannel.d = this;
        stderrChannel.pipe = &stderrPipe;
        stderrChannel.stream = stderr;
    }

    bool startRead();

    Process *q;
    HANDLE hProcess;
    HANDLE hProcessThread;
    Pipe stdoutPipe;
    Pipe stderrPipe;
    Pipe stdinPipe;     // we don't use it but some processes demand it (e.g. xcopy)
    OutputChannel stdoutChannel;
    OutputChannel stderrChannel;
    QMutex bufferedOutputModeSwitchMutex;
    DWORD exitCode;
    QWinEventNotifier deathNotifier;
};

Process::Process(QObject *parent)
    : QObject(parent),
      d(new ProcessPrivate(this)),
      m_state(NotRunning),
      m_exitCode(0),
      m_exitStatus(NormalExit),
      m_bufferedOutput(true)
{
    static bool staticsInitialized = false;
    if (!staticsInitialized) {
        staticsInitialized = true;
        qRegisterMetaType<ExitStatus>("Process::ExitStatus");
        qRegisterMetaType<ProcessError>("Process::ProcessError");
        qRegisterMetaType<ProcessState>("Process::ProcessState");
        runtime()->start();
    }
    connect(&d->deathNotifier, &QWinEventNotifier::activated,
            this, &Process::tryToRetrieveExitCode);
}

Process::~Process()
{
    IoCompletionPort::instance()->unregisterObserver(&d->stdoutChannel);
    IoCompletionPort::instance()->unregisterObserver(&d->stderrChannel);

    if (m_state == Running)
        qWarning("Process: destroyed while process still running.");
    printBufferedOutput();
    delete d;
}

void Process::setBufferedOutput(bool b)
{
    if (m_bufferedOutput == b)
        return;

    d->bufferedOutputModeSwitchMutex.lock();

    m_bufferedOutput = b;
    if (!m_bufferedOutput)
        printBufferedOutput();

    d->bufferedOutputModeSwitchMutex.unlock();
}

void Process::writeToStdOutBuffer(const QByteArray &output)
{
    d->stdoutChannel.outputBufferLock.lock();
    d->stdoutChannel.buffers.append(TimeStampedBuffer(runtime()->elapsed(), output));
    d->stdoutChannel.outputBufferLock.unlock();
}

void Process::writeToStdErrBuffer(const QByteArray &output)
{
    d->stderrChannel.outputBufferLock.lock();
    d->stderrChannel.buffers.append(TimeStampedBuffer(runtime()->elapsed(), output));
    d->stderrChannel.outputBufferLock.unlock();
}

void Process::setWorkingDirectory(const QString &path)
{
    m_workingDirectory = path;
}

static QByteArray createEnvBlock(const ProcessEnvironment &environment)
{
    QByteArray envlist;
    if (!environment.isEmpty()) {
        ProcessEnvironment copy = environment;

        const QString pathKey(QLatin1String("Path"));
        if (copy.contains(pathKey)) {
            // PATH has been altered.
            // It must be set in this environment to start the correct executable.
            // ### Note that this doesn't work if a batch file is supposed to shadow an exe or com.
            if (!qSetEnvironmentVariable(pathKey, environment.value(pathKey)))
                qWarning("jom: setting PATH failed");
        } else {
            // add PATH (for DLL loading)
            QString path = qGetEnvironmentVariable(L"PATH");
            if (!path.isEmpty())
                copy.insert(pathKey, path);
        }

        // add systemroot if needed
        const ProcessEnvironmentKey rootKey(QLatin1String("SystemRoot"));
        if (!copy.contains(rootKey)) {
            QString systemRoot = qGetEnvironmentVariable(L"SystemRoot");
            if (!systemRoot.isEmpty())
                copy.insert(rootKey, systemRoot);
        }

        int pos = 0;
        ProcessEnvironment::const_iterator it = copy.constBegin();
        const ProcessEnvironment::const_iterator end = copy.constEnd();

        static const wchar_t equal = L'=';
        static const wchar_t nul = L'\0';

        for ( ; it != end; ++it) {
            const QString &keystr = it.key().toQString();
            uint tmpSize = sizeof(wchar_t) * (keystr.length() + it.value().length() + 2);
            // ignore empty strings
            if (tmpSize == sizeof(wchar_t) * 2)
                continue;
            envlist.resize(envlist.size() + tmpSize);

            tmpSize = keystr.length() * sizeof(wchar_t);
            memcpy(envlist.data() + pos, keystr.utf16(), tmpSize);
            pos += tmpSize;

            memcpy(envlist.data()+pos, &equal, sizeof(wchar_t));
            pos += sizeof(wchar_t);

            tmpSize = it.value().length() * sizeof(wchar_t);
            memcpy(envlist.data()+pos, it.value().utf16(), tmpSize);
            pos += tmpSize;

            memcpy(envlist.data()+pos, &nul, sizeof(wchar_t));
            pos += sizeof(wchar_t);
        }
        // add the 2 terminating 0 (actually 4, just to be on the safe side)
        envlist.resize( envlist.size()+4 );
        envlist[pos++] = 0;
        envlist[pos++] = 0;
        envlist[pos++] = 0;
        envlist[pos++] = 0;
    }
    return envlist;
}

void Process::setEnvironment(const ProcessEnvironment &environment)
{
    m_environment = environment;
    m_envBlock = createEnvBlock(environment);
}

enum PipeType { InputPipe, OutputPipe };

static bool setupPipe(Pipe &pipe, SECURITY_ATTRIBUTES *sa, PipeType pt)
{
    BOOL oldInheritHandle = sa->bInheritHandle;

    HANDLE hRead;
    const size_t maxPipeNameLen = 256;
    wchar_t pipeName[maxPipeNameLen];
    unsigned int attempts = 1000;
    do {
        unsigned int randomValue;
        if (rand_s(&randomValue) != 0)
            randomValue = rand();
        swprintf_s(pipeName, maxPipeNameLen, L"\\\\.\\pipe\\jom-%X", randomValue);

        sa->bInheritHandle = (pt == InputPipe);
        const DWORD dwPipeBufferSize = 1024 * 1024;
        const DWORD dwPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                | PIPE_REJECT_REMOTE_CLIENTS;
        hRead = CreateNamedPipe(pipeName,
                                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                dwPipeMode,
                                1,                      // only one pipe instance
                                0,                      // output buffer size
                                dwPipeBufferSize,       // input buffer size
                                0,
                                sa);
        if (hRead != INVALID_HANDLE_VALUE) {
            // Rejoice! The pipe was created!
            break;
        } else {
            DWORD dwError = GetLastError();
            if (dwError != ERROR_PIPE_BUSY) {
                qErrnoWarning(dwError, "Process: CreateNamedPipe failed.");
                return false;
            }
        }
    } while (--attempts > 0);

    sa->bInheritHandle = (pt == OutputPipe);
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    hWrite = CreateFile(pipeName,
                        GENERIC_WRITE,
                        0,
                        sa,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);
    if (hWrite == INVALID_HANDLE_VALUE) {
        qErrnoWarning("Process: CreateFile failed.");
        CloseHandle(hRead);
        return false;
    }

    // Wait until connection is in place.
    ConnectNamedPipe(hRead, NULL);

    pipe.hRead = hRead;
    pipe.hWrite = hWrite;
    sa->bInheritHandle = oldInheritHandle;
    return true;
}

void Process::start(const QString &commandLine)
{
    m_state = Starting;

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!setupPipe(d->stdinPipe, &sa, InputPipe))
        qFatal("Cannot setup pipe for stdin.");
    if (!setupPipe(d->stdoutPipe, &sa, OutputPipe))
        qFatal("Cannot setup pipe for stdout.");
    if (!setupPipe(d->stderrPipe, &sa, OutputPipe))
        qFatal("Cannot setup pipe for stderr.");

    IoCompletionPort::instance()->registerObserver(&d->stdoutChannel, d->stdoutPipe.hRead);
    IoCompletionPort::instance()->registerObserver(&d->stderrChannel, d->stderrPipe.hRead);
    if (!d->startRead()) {
        m_state = NotRunning;
        emit error(FailedToStart);
        qWarning("Can't read output channels.");
        return;
    }

    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    si.hStdInput = d->stdinPipe.hRead;
    si.hStdOutput = d->stdoutPipe.hWrite;
    si.hStdError = d->stderrPipe.hWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    DWORD dwCreationFlags = CREATE_UNICODE_ENVIRONMENT;
    PROCESS_INFORMATION pi;
    wchar_t *strCommandLine = _wcsdup((const wchar_t*)commandLine.utf16());     // CreateProcess can modify this string
    const wchar_t *strWorkingDir = 0;
    if (!m_workingDirectory.isEmpty()) {
        m_workingDirectory = QDir::toNativeSeparators(m_workingDirectory);
        strWorkingDir = (const wchar_t*)m_workingDirectory.utf16();
    }
    void *envBlock = (m_envBlock.isEmpty() ? 0 : m_envBlock.data());
    BOOL bResult = CreateProcess(NULL, strCommandLine,
                                 0, 0, TRUE, dwCreationFlags, envBlock,
                                 strWorkingDir, &si, &pi);
    free(strCommandLine);
    strCommandLine = 0;
    if (!bResult) {
        m_state = NotRunning;
        emit error(FailedToStart);
        return;
    }

    // Close the pipe handles. This process doesn't need them anymore.
    safelyCloseHandle(d->stdinPipe.hRead);
    safelyCloseHandle(d->stdinPipe.hWrite);
    safelyCloseHandle(d->stdoutPipe.hWrite);
    safelyCloseHandle(d->stderrPipe.hWrite);

    d->deathNotifier.setHandle(pi.hProcess);
    d->deathNotifier.setEnabled(true);
    d->hProcess = pi.hProcess;
    d->hProcessThread = pi.hThread;
    m_state = Running;
}

void Process::tryToRetrieveExitCode()
{
    if (d->exitCode == STILL_ACTIVE)
        if (!GetExitCodeProcess(d->hProcess, &d->exitCode))
            d->exitCode = STILL_ACTIVE;

    if (d->exitCode == STILL_ACTIVE)
        QTimer::singleShot(250, this, SLOT(tryToRetrieveExitCode()));
    else
        onProcessFinished();
}

void Process::onProcessFinished()
{
    if (m_state != Running)
        return;

    d->deathNotifier.setEnabled(false);
    IoCompletionPort::instance()->unregisterObserver(&d->stdoutChannel);
    IoCompletionPort::instance()->unregisterObserver(&d->stderrChannel);
    safelyCloseHandle(d->stdoutPipe.hRead);
    safelyCloseHandle(d->stderrPipe.hRead);
    safelyCloseHandle(d->hProcess);
    safelyCloseHandle(d->hProcessThread);
    printBufferedOutput();
    m_state = NotRunning;
    m_exitCode = d->exitCode;
    d->exitCode = STILL_ACTIVE;

    //### for now we assume a crash if exit code is less than -1 or the magic number
    const bool crashed = (m_exitCode == 0xf291 || m_exitCode < 0);
    ExitStatus exitStatus = crashed ? Process::CrashExit : Process::NormalExit;
    emit finished(m_exitCode, exitStatus);
}

bool Process::waitForFinished()
{
    if (m_state != Running)
        return true;
    //if (WaitForSingleObject(d->hProcess, INFINITE) == WAIT_TIMEOUT)
    //    return false;

    QEventLoop eventLoop;
    connect(this, SIGNAL(finished(int, Process::ExitStatus)), &eventLoop, SLOT(quit()));
    eventLoop.exec();

    m_state = NotRunning;
    return true;
}

bool ProcessPrivate::startRead()
{
    return stdoutChannel.startRead() && stderrChannel.startRead();
}

/**
 * Starts the asynchronous read operation.
 * Returns true, if initiating the read operation was successful.
 */
bool OutputChannel::startRead()
{
    DWORD dwRead;
    BOOL bSuccess;

    const DWORD minReadBufferSize = 4096;
    bSuccess = PeekNamedPipe(pipe->hRead, NULL, 0, NULL, &dwRead, NULL);
    if (!bSuccess || dwRead < minReadBufferSize)
        dwRead = minReadBufferSize;

    intermediateOutputBuffer.resize(dwRead);
    bSuccess = ReadFile(pipe->hRead,
                        intermediateOutputBuffer.data(),
                        intermediateOutputBuffer.size(),
                        NULL,
                        &pipe->overlapped);
    if (!bSuccess) {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
            return false;
    }
    return true;
}

static void fwrite_binary(FILE *stream, const char *str, size_t count)
{
    const int fd = _fileno(stream);
    const int origMode = _setmode(fd, _O_BINARY);
    if (fwrite(str, sizeof(char), count, stream)) {
        // Write operation was successful.
        fflush(stream);
    } else if (errno == ENOMEM) {
        // The buffer was too big for writing. Write it in chunks.
        const size_t chunkSize = 4096;
        for (;;) {
            size_t k = qMin(chunkSize, count);
            fwrite(str, sizeof(char), k, stream);
            fflush(stream);
            if (k >= count)
                break;
            str += k;
            count -= k;
        }
    }
    if (origMode != -1)
        _setmode(fd, origMode);
}

/**
 * Is called whenever we receive the result of an asynchronous I/O operation.
 * Note: This function is running in the IOCP thread!
 */
void OutputChannel::completionPortNotified(DWORD numberOfBytes, DWORD errorCode)
{
    if (numberOfBytes)  {
        d->bufferedOutputModeSwitchMutex.lock();

        if (d->q->isBufferedOutputSet()) {
            outputBufferLock.lock();
            QByteArray data(intermediateOutputBuffer.data(), numberOfBytes);
            buffers.append(TimeStampedBuffer(runtime()->elapsed(), data));
            outputBufferLock.unlock();
        } else {
            fwrite_binary(stream, intermediateOutputBuffer.data(), numberOfBytes);
        }

        d->bufferedOutputModeSwitchMutex.unlock();
    }

    if (errorCode == ERROR_SUCCESS)
        if (startRead())
            return;

    QMetaObject::invokeMethod(d->q, "tryToRetrieveExitCode", Qt::QueuedConnection);
}

void Process::printBufferedOutput()
{
    while (!d->stdoutChannel.buffers.isEmpty()
        || !d->stderrChannel.buffers.isEmpty())
    {
        OutputChannel *channels[2] = { &d->stdoutChannel, &d->stderrChannel };

        size_t i = 0;
        if (channels[0]->buffers.isEmpty()
            || (!channels[1]->buffers.isEmpty()
                && channels[0]->buffers.first().timestamp > channels[1]->buffers.first().timestamp))
        {
            i = 1;
        }

        OutputChannel *const channel = channels[i];
        channel->outputBufferLock.lock();
        const QByteArray &ba = channel->buffers.first().buffer;
        fwrite_binary(channel->stream, ba.data(), ba.count());
        channel->buffers.removeFirst();
        channel->outputBufferLock.unlock();
    }
}

} // namespace NMakeFile

QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(NMakeFile::TimeStampedBuffer, Q_MOVABLE_TYPE);
QT_END_NAMESPACE
