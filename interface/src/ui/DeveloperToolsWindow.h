//
//  DeveloperToolsWindow.h
//  interface/src/ui
//
//  Created by Stephen Birarda on 2015-10-09.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_DeveloperToolsWindow_h
#define hifi_DeveloperToolsWindow_h

#include "../scripting/WebWindowClass.h"

class DeveloperToolsWindow : public WebWindowClass {
    Q_OBJECT
public:
    DeveloperToolsWindow();
private:

};

class DeveloperToolsWindowManager : public QObject {
    Q_OBJECT
public:
    static DeveloperToolsWindowManager& getInstance();
    
public slots:
    void showWindow();
    void handleLogLine(QtMsgType type, const QString& message) { _logLines << message; }

private:
    DeveloperToolsWindow* _window { nullptr };
    QStringList _logLines;
};


#endif // hifi_DeveloperToolsWindow_h
