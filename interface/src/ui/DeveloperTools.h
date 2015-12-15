//
//  DeveloperTools.h
//  interface/src/ui
//
//  Created by Stephen Birarda on 2015-10-09.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_DeveloperTools_h
#define hifi_DeveloperTools_h

#include <QtCore/QPointer>
#include <QtWebChannel/QWebChannel>
#include <QtWebEngineWidgets/QWebEngineView>
#include <QtWebSockets/QWebSocketServer>

#include "WebSocketClientWrapper.h"

namespace DeveloperTools {
    class ScriptingInterface : public QObject {
        Q_OBJECT
        Q_PROPERTY(QStringList log READ getLogLines)

    public slots:
        void revealLogFile();

    signals:
        void newLogLine(int index, const QString& message);

    private:
        void handleLogLine(const QString& message);

        const QStringList& getLogLines() const { return _logLines; }
        QStringList _logLines;

        friend class WindowManager;
    };

    class WindowManager : public QObject {
        Q_OBJECT
    public:
        static WindowManager& getInstance();
        void setWindowParent(QWidget* parent) { _parent = parent; }
    public slots:
        void showWindow();
        void handleLogLine(const QString& message) { _scriptInterface.handleLogLine(message); }
        
    private:
        WindowManager() {};
        Q_DISABLE_COPY(WindowManager);

        void setupWebSocketServer();

        QWidget* _parent { nullptr };

        QPointer<QWebEngineView> _window { nullptr };
        ScriptingInterface _scriptInterface;

        QWebSocketServer _server { "Developer Tools Server", QWebSocketServer::NonSecureMode };
        WebSocketClientWrapper _clientWrapper { &_server }; // wraps WebSocket clients in QWebChannelAbstractTransport objects
        QWebChannel _channel;
    };
}




#endif // hifi_DeveloperTools_h
