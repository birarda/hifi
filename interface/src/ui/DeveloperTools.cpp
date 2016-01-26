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
#include <OffscreenUi.h>

#include <QQmlContext>

#include <QmlWebTransport.h>

#include "../Application.h"

using namespace DeveloperTools;

// currently because it's the only view available the developer tools window goes right to the log
const QString DEV_TOOLS_INDEX_PATH = "html/dev-tools/log.html";

void ScriptingInterface::handleLogLine(const QString& message) {
    // add the log line to our in-memory QStringList
    _logLines << message;

    // emit our signal to say that a new log line has been added
    QMetaObject::invokeMethod(this, "newLogLine", Qt::QueuedConnection,
                              Q_ARG(int, _logLines.size() - 1), Q_ARG(QString, message));
}

void ScriptingInterface::revealLogFile() {
    auto logger = qApp->getLogger();
    if (logger) {
        logger->locateLog();
    }
}

Window::Window(QObject* qmlWindow) : QmlWebWindowClass(qmlWindow) {

}

WindowManager& WindowManager::getInstance() {
    static WindowManager staticInstance;
    return staticInstance;
}

void WindowManager::showWindow() {
    // is the web socket server ready to go?
    if (!_server) {
        setupWebSocketServer();
    }

    if (!_window) {
        // set the URL of the window to show the log, add a query to pass the url to the web channel server
        QUrl devToolsURL = QUrl::fromLocalFile(PathUtils::resourcesPath() + DEV_TOOLS_INDEX_PATH);
        devToolsURL.setQuery("webChannelURL=" + _server->serverUrl().toString());

        qDebug().noquote() << "Opening the Developer Tools QWebEngineView at" << devToolsURL.toString();

        auto offscreenUi = DependencyManager::get<OffscreenUi>();
        const QUrl DEVELOPER_TOOLS_QML = QUrl("DeveloperTools.qml");
        offscreenUi->show(DEVELOPER_TOOLS_QML, "DeveloperTools", [&devToolsURL](QQmlContext* context, QObject* newObject){
            QQuickItem* item = dynamic_cast<QQuickItem*>(newObject);
            item->setWidth(720);
            item->setHeight(720);
            item->setProperty("source", devToolsURL.toString());
        });
    }
    
}

void WindowManager::setupWebSocketServer() {
    // NOTE: We may want to avoid using multiple QWebSocketServers for QtWebEngine to C++ bridge, a single server
    // could accomodate all.

    if (!_server) {
        _server = std::unique_ptr<QWebSocketServer> { new QWebSocketServer("Developer Tools Server", QWebSocketServer::NonSecureMode) };

        if (!_server->listen(QHostAddress::LocalHost, 0)) {
            qWarning() << "Failed to open Developer Tools web socket server. Developer Tools will not be available.";
        } else {
            qDebug() << "Developer Tools QWebSocketServer listening at" << _server->serverUrl().toString();

            // setup the QWebChannel when client connects
            QObject::connect(_server.get(), &QWebSocketServer::newConnection, [this] {
                _channel.connectTo(new QmlWebTransport(_server->nextPendingConnection()));
            });

            // register the scripting interface with the web channel
            _channel.registerObject("developer", &_scriptInterface);
        }
    }
}

