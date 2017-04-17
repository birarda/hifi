//
//  OvenMainWindow.cpp
//  tools/oven/src/ui
//
//  Created by Stephen Birarda on 4/6/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QtWidgets/QStackedWidget>

#include "ModesWidget.h"

#include "OvenMainWindow.h"

OvenMainWindow::OvenMainWindow(QWidget *parent, Qt::WindowFlags flags) :
    QMainWindow(parent, flags)
{
    setWindowTitle("High Fidelity Oven");

    // give the window a fixed width that will never change
    setFixedWidth(FIXED_WINDOW_WIDTH);

    // setup a stacked layout for the main "modes" menu and subseq
    QStackedWidget* stackedWidget = new QStackedWidget(this);
    stackedWidget->addWidget(new ModesWidget);

    setCentralWidget(stackedWidget);
}

OvenMainWindow::~OvenMainWindow() {
    if (_resultsWindow) {
        _resultsWindow->close();
        _resultsWindow->deleteLater();
    }
}

ResultsWindow* OvenMainWindow::showResultsWindow() {
    if (!_resultsWindow) {
        // we don't have a results window right now, so make a new one
        _resultsWindow = new ResultsWindow;
    }

    // show the results window, place it right below our window
    _resultsWindow->show();
    _resultsWindow->move(_resultsWindow->x(), this->frameGeometry().bottom());

    // return a pointer to the results window the caller can use
    return _resultsWindow;
}