/*
  This file is part of the SpeedCrunch project
  Copyright (C) 2009, 2010 @heldercorreia

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to
  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301, USA.
*/

#include "gui/application.h"

#include <QByteArray>
#include <QDir>

#include <QSharedMemory>
#ifndef Q_OS_WIN
#include <QLocalServer>
#include <QLocalSocket>
#endif  /* Q_OS_WIN */

#ifdef Q_OS_WIN
#include <windows.h>
#endif  /* Q_OS_WIN */

#define GUI_APPLICATION_SHARED_MEMORY_KEY QStringLiteral("speedcrunch")
#define GUI_APPLICATION_LOCAL_SOCKET_TIMEOUT 1000

#ifdef Q_OS_WIN

typedef struct {
    /** ID of the process to look for (IN) */
    DWORD processId;
    /** Handle of the main window of that process (OUT) */
    HWND hwnd;
} EnumWindowParam;

static HWND FindProcessWindow(DWORD processId);
static BOOL CALLBACK FindProcessWindowCallback(HWND hwnd, LPARAM lParam);

#endif  /* Q_OS_WIN */

static QString GetSessionId(void);

class ApplicationPrivate {
public:
    bool isRunning;
    QSharedMemory sharedMemory;
    QString instanceKey;
#ifndef Q_OS_WIN
    QLocalServer localServer;
#endif  /* Q_OS_WIN */
};

typedef struct {
#ifdef Q_OS_WIN

    /** Process ID of the already running instance */
    DWORD processId;

#else   /* Q_OS_WIN */

    char dummy;  // Make sure the size of the data is not 0

#endif  /* Q_OS_WIN */
} ApplicationShared;

Application::Application(int &argc, char *argv[])
    : QApplication(argc, argv)
    , d_ptr(new ApplicationPrivate)
{
    Q_D(Application);

    // Make instance key specific to user sessions
    d->instanceKey = GUI_APPLICATION_SHARED_MEMORY_KEY + QLatin1Char('@') + GetSessionId();

    d->isRunning = false;
    d->sharedMemory.setKey(d->instanceKey);

    if (d->sharedMemory.attach()) {
        d->isRunning = true;
    } else {
        if (!d->sharedMemory.create(sizeof(ApplicationShared)))
            return;

#ifdef Q_OS_WIN

        // Share our own process ID so that other instances can raise our window
        d->sharedMemory.lock();
        ApplicationShared *sharedData = (ApplicationShared*) d->sharedMemory.data();
        sharedData->processId = GetCurrentProcessId();
        d->sharedMemory.unlock();

#else   /* Q_OS_WIN */
        // Was not running, cleanup in case the application crashed
        QString localServerAddress = QDir::tempPath() + QLatin1Char('/') + d->instanceKey;
        QFile localSocketFile(localServerAddress);
        if (localSocketFile.exists())
            localSocketFile.remove();
        connect(&d->localServer, SIGNAL(newConnection()), SLOT(receiveMessage()));
        d->localServer.listen(localServerAddress);

#endif  /* Q_OS_WIN */

    }
}

Application::~Application()
{
}

#ifndef Q_OS_WIN

void Application::receiveMessage()
{
    Q_D(Application);

    QLocalSocket *localSocket = d->localServer.nextPendingConnection();

    if (!localSocket->waitForReadyRead(GUI_APPLICATION_LOCAL_SOCKET_TIMEOUT))
        return;

    QByteArray byteArray = localSocket->readAll();
    const QString message = QString::fromUtf8(byteArray.constData());

    if (message == "raise")
        emit raiseRequested();

    localSocket->disconnectFromServer();
}

#endif  /* Q_OS_WIN */

bool Application::isRunning()
{
    Q_D(Application);

    return d->isRunning;
}

bool Application::sendRaiseRequest()
{
    Q_D(Application);

    if (!d->isRunning)
        return false;

#ifdef Q_OS_WIN

    // Read other instance process ID (and copy it locally to prevent leaking the lock on crash)
    d->sharedMemory.lock();
    ApplicationShared *sharedData = (ApplicationShared*) d->sharedMemory.data();
    DWORD processId = sharedData->processId;
    d->sharedMemory.unlock();

    HWND hwnd = FindProcessWindow(processId);
    if (hwnd == NULL)
        return false;

    // Restore window if it is minimized
    WINDOWPLACEMENT placement = { sizeof(placement) };
    GetWindowPlacement(hwnd, &placement);

    if (placement.showCmd == SW_SHOWMINIMIZED)
        ShowWindowAsync(hwnd, SW_RESTORE);

    // Raise window up
    SetForegroundWindow(hwnd);

    return true;

#else   /* Q_OS_WIN */

    QLocalSocket localSocket;
    localSocket.connectToServer(d->instanceKey, QIODevice::WriteOnly);

    if (!localSocket.waitForConnected(GUI_APPLICATION_LOCAL_SOCKET_TIMEOUT))
        return false;

    localSocket.write(QString("raise").toUtf8());

    if (!localSocket.waitForBytesWritten(GUI_APPLICATION_LOCAL_SOCKET_TIMEOUT))
        return false;

    localSocket.disconnectFromServer();
    return true;

#endif  /* Q_OS_WIN */

}

/** Return system session ID (QGuiApplication::sessionId() returns something unrelated) */
QString GetSessionId(void) {

#ifdef Q_OS_WIN

    DWORD sessionId;
    if(ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) == TRUE)
        return QString::number(sessionId);

    // Fallback to user name
    return qgetenv("USERNAME");

#else   /* Q_OS_WIN */

    // Fallback to user name
    QString user = qgetenv("USER");  // Linux/MacOS
    if (user.isEmpty())
        user = qgetenv("USERNAME");  // Windows

    return user;

#endif  /* Q_OS_WIN */

}

#ifdef Q_OS_WIN

/** Find window handle of given process. */
HWND FindProcessWindow(DWORD processId) {
    EnumWindowParam param;
    param.hwnd = NULL;
    param.processId = processId;

    // Enumerate all windows and return the one that is the main window of given process
    EnumWindows(FindProcessWindowCallback, (LPARAM) &param);

    return param.hwnd;
}

/** EnumWindows() callback used by FindProcessWindow(). */
BOOL CALLBACK FindProcessWindowCallback(HWND hwnd, LPARAM lParam) {
    EnumWindowParam *param = (EnumWindowParam*) lParam;

    // Check if the window belongs to the requested process
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == param->processId) {
        // Check if this is the main window
        if (GetWindow(hwnd, GW_OWNER) == (HWND)0 && IsWindowVisible(hwnd)) {
            param->hwnd = hwnd;
            return FALSE;
        }
    }

    return TRUE;
}

#endif  /* Q_OS_WIN */
