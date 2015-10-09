//
//  DeveloperToolsWindow.cpp
//  interface/src/ui
//
//  Created by Stephen Birarda on 2015-10-09.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DeveloperToolsWindow.h"

#include <PathUtils.h>

DeveloperToolsWindowManager& DeveloperToolsWindowManager::getInstance() {
    static DeveloperToolsWindowManager staticInstance;
    return staticInstance;
}

void DeveloperToolsWindowManager::showWindow() {
    if (!_window) {
        _window = new DeveloperToolsWindow;
    }
    
    _window->setVisible(true);
}

const QString DEV_TOOLS_INDEX_PATH = "html/dev-tools/index.html";

DeveloperToolsWindow::DeveloperToolsWindow() :
    WebWindowClass("Developer Tools", PathUtils::resourcesPath() + DEV_TOOLS_INDEX_PATH, 400, 400)
{
    
}
