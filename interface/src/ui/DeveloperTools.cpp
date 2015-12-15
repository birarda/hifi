//
//  DeveloperTools.cpp
//  interface/src/ui
//
//  Created by Stephen Birarda on 2015-10-09.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DeveloperTools.h"

#include <QtWebKitWidgets/QWebFrame>

#include <PathUtils.h>

#include "../Application.h"
#include "WebSocketTransport.h"

using namespace DeveloperTools;

// currently because it's the only view available the developer tools window goes right to the log
const QString DEV_TOOLS_INDEX_PATH = "html/dev-tools/log.html";

void ScriptingInterface::handleLogLine(const QString& message) {
    // add the log line to our in-memory QStringList
    _logLines << message;

    // emit our signal to say that a new log line has been added
    emit newLogLine(_logLines.size() - 1, message);
}

void ScriptingInterface::revealLogFile() {
    auto logger = qApp->getLogger();
    if (logger) {
        logger->locateLog();
    }
}

WindowManager& WindowManager::getInstance() {
    static WindowManager staticInstance;
    return staticInstance;
}

void WindowManager::showWindow() {
    // is the web socket server ready to go?
    if (!_server.isListening()) {
        setupWebSocketServer();
    }

    if (!_window) {
        _window = new QWebEngineView;

        // set the window title
        _window->setWindowTitle("Log");

        // delete the dialog on close
        _window->setAttribute(Qt::WA_DeleteOnClose);

        // set the URL of the window to show the log, add a query to pass the url to the web channel server
        QUrl devToolsURL = QUrl::fromLocalFile(PathUtils::resourcesPath() + DEV_TOOLS_INDEX_PATH);
        devToolsURL.setQuery("webChannelURL=" + _server.serverUrl().toString());

        qDebug().noquote() << "Opening the Developer Tools QWebEngineView at" << devToolsURL.toString();

        _window->setUrl(devToolsURL);
    }
    
    _window->show();
}

void WindowManager::setupWebSocketServer() {
    // NOTE: Should we end up using QWebChannel for multiple views, it's likely we should centralize this and just
    // have all registered objects (behind safe scripting interfaces) exposed to the QWebEngineViews.

    if (!_server.listen(QHostAddress::LocalHost)) {
        qWarning() << "Failed to open Developer Tools web socket server. Developer Tools will not be available.";
    } else {
        qDebug() << "Developer Tools QWebSocketServer listening at" << _server.serverUrl().toString();

        // setup the QWebChannel
        connect(&_clientWrapper, &WebSocketClientWrapper::clientConnected, &_channel, &QWebChannel::connectTo);

        // register the scripting interface with the web channel
        _channel.registerObject("developer", &_scriptInterface);
    }
}

