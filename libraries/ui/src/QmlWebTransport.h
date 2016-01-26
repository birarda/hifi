//
//  QmlWebTransport.h
//  libraries/ui/src
//
//  Created by Bradley Austin Davis on 2015-12-15
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_QmlWebTransport_h
#define hifi_QmlWebTransport_h

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWebSockets/QWebSocket>
#include <QtWebChannel/QWebChannelAbstractTransport>

class QmlWebTransport : public QWebChannelAbstractTransport {
    Q_OBJECT
public:
    QmlWebTransport(QWebSocket* webSocket) : _webSocket(webSocket) {
        // Translate from the websocket layer to the webchannel layer
        connect(webSocket, &QWebSocket::textMessageReceived, [this](const QString& message) {
            QJsonParseError error;
            QJsonDocument document = QJsonDocument::fromJson(message.toUtf8(), &error);
            if (error.error || !document.isObject()) {
                qWarning() << "Unable to parse incoming JSON message" << message;
                return;
            }
            emit messageReceived(document.object(), this);
        });
    }

    virtual void sendMessage(const QJsonObject &message) override {
        // Translate from the webchannel layer to the websocket layer
        _webSocket->sendTextMessage(QJsonDocument(message).toJson(QJsonDocument::Compact));
    }

private:
    QWebSocket* const _webSocket;
};


#endif // hifi_QmlWebTransport_h
