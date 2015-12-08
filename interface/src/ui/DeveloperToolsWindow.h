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
};

class DeveloperToolsWindowManager : public QObject {
    Q_OBJECT
public:
    static DeveloperToolsWindowManager& getInstance();
    
public slots:
    void showWindow();

private:
    DeveloperToolsWindow* _window { nullptr };
};


#endif // hifi_DeveloperToolsWindow_h
