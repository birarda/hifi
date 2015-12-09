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
#include <QtWebKitWidgets/QWebView>

namespace DeveloperTools {
    class Window : public QWebView {
        Q_OBJECT
    public:
        Window();
    private:

    };

    class ScriptingInterface : public QObject {
        Q_OBJECT
        Q_PROPERTY(QStringList log READ getLogLines)
    public slots:
        void handleLogLine(QtMsgType type, const QString& message) { _logLines << message; }
    private:
        const QStringList& getLogLines() const { return _logLines; }
        QStringList _logLines;
    };

    class WindowManager : public QObject {
        Q_OBJECT
    public:
        static WindowManager& getInstance();

    public slots:
        void showWindow();
        void handleLogLine(QtMsgType type, const QString& message) { _scriptInterface.handleLogLine(type, message); }

    private slots:
        void addToolsObjectToWindow();
        
    private:
        QPointer<Window> _window { nullptr };
        ScriptingInterface _scriptInterface;
    };
}




#endif // hifi_DeveloperTools_h
