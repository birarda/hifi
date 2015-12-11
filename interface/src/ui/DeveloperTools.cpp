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

using namespace DeveloperTools;

// currently because it's the only view available the developer tools window goes right to the log
const QString DEV_TOOLS_INDEX_PATH = "html/dev-tools/log.html";

Window::Window() {

#ifndef Q_NO_DEBUG
    // in debug, allow the web inspector on QWebView
    QWebSettings::globalSettings()->setAttribute(QWebSettings::DeveloperExtrasEnabled, true);
#endif

    // set the window title
    setWindowTitle("Log");

    // delete the dialog on close
    setAttribute(Qt::WA_DeleteOnClose);

    // set the URL of the window to show the log
    setUrl(QUrl::fromLocalFile(PathUtils::resourcesPath() + DEV_TOOLS_INDEX_PATH));
}

void ScriptingInterface::handleLogLine(QtMsgType type, const QString& message) {
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
    if (!_window) {
        _window = new Window;

        // immediately add the tools object to the window and hook up to have it re-added if the window object is cleared
        addToolsObjectToWindow();
        connect(_window->page()->mainFrame(), &QWebFrame::javaScriptWindowObjectCleared,
                this, &WindowManager::addToolsObjectToWindow);
    }
    
    _window->setVisible(true);
}

void WindowManager::addToolsObjectToWindow() {
    if (_window) {
        _window->page()->mainFrame()->addToJavaScriptWindowObject("Developer", &_scriptInterface);
    }
}


