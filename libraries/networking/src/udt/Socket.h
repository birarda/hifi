//
//  Socket.h
//  libraries/networking/src/udt
//
//  Created by Stephen Birarda on 2015-07-20.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_Socket_h
#define hifi_Socket_h

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <functional>
#include <unordered_map>
#include <mutex>

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <tbb/concurrent_queue.h>

#include "../HifiSockAddr.h"
#include "TCPVegasCC.h"
#include "Connection.h"

//#define UDT_CONNECTION_DEBUG

class UDTTest;

namespace udt {

class BasePacket;
class Packet;
class PacketList;
class SequenceNumber;
class PacketReciever;

using PacketFilterOperator = std::function<bool(const Packet&)>;
using ConnectionCreationFilterOperator = std::function<bool(const HifiSockAddr&)>;

using BasePacketHandler = std::function<void(std::unique_ptr<BasePacket>)>;
using PacketHandler = std::function<void(std::unique_ptr<Packet>)>;
using MessageHandler = std::function<void(std::unique_ptr<Packet>)>;
using MessageFailureHandler = std::function<void(HifiSockAddr, udt::Packet::MessageNumber)>;

struct Datagram {
    QHostAddress _senderAddress;
    int _senderPort;
    int _datagramLength;
    std::unique_ptr<char[]> _datagram;
    p_high_resolution_clock::time_point _receiveTime;
};

class PacketReciever : public QObject {
    Q_OBJECT

public:
    PacketReciever(tbb::concurrent_queue<Datagram>& incomingDatagrams);

    void run(int fd);

signals:
    void pendingDatagrams(int datagramCount);

private:
    tbb::concurrent_queue<Datagram>& _incomingDatagrams;
};

class Socket : public QObject {
    Q_OBJECT

    using Mutex = std::mutex;
    using Lock = std::unique_lock<Mutex>;

public:
    using StatsVector = std::vector<std::pair<HifiSockAddr, ConnectionStats::Stats>>;

    Socket(QObject* object = 0, bool shouldChangeSocketOptions = true);
    ~Socket();
    
    quint16 localPort() const { return _localPort; }
    
    // Simple functions writing to the socket with no processing
    qint64 writeBasePacket(const BasePacket& packet, const HifiSockAddr& sockAddr);
    qint64 writePacket(const Packet& packet, const HifiSockAddr& sockAddr);
    qint64 writePacket(std::unique_ptr<Packet> packet, const HifiSockAddr& sockAddr);
    qint64 writePacketList(std::unique_ptr<PacketList> packetList, const HifiSockAddr& sockAddr);
    qint64 writeDatagram(const char* data, qint64 size, const HifiSockAddr& sockAddr);
    qint64 writeDatagram(const QByteArray& datagram, const HifiSockAddr& sockAddr);
    
    void bind(const QHostAddress& address, quint16 port = 0);
    void rebind(quint16 port);
    void rebind();

    void setPacketFilterOperator(PacketFilterOperator filterOperator) { _packetFilterOperator = filterOperator; }
    void setPacketHandler(PacketHandler handler) { _packetHandler = handler; }
    void setMessageHandler(MessageHandler handler) { _messageHandler = handler; }
    void setMessageFailureHandler(MessageFailureHandler handler) { _messageFailureHandler = handler; }
    void setConnectionCreationFilterOperator(ConnectionCreationFilterOperator filterOperator)
        { _connectionCreationFilterOperator = filterOperator; }
    
    void addUnfilteredHandler(const HifiSockAddr& senderSockAddr, BasePacketHandler handler)
        { _unfilteredHandlers[senderSockAddr] = handler; }
    
    void setCongestionControlFactory(std::unique_ptr<CongestionControlVirtualFactory> ccFactory);
    void setConnectionMaxBandwidth(int maxBandwidth);

    void messageReceived(std::unique_ptr<Packet> packet);
    void messageFailed(Connection* connection, Packet::MessageNumber messageNumber);
    
    StatsVector sampleStatsForAllConnections();

#if (PR_BUILD || DEV_BUILD)
    void sendFakedHandshakeRequest(const HifiSockAddr& sockAddr);
#endif

signals:
    void clientHandshakeRequestComplete(const HifiSockAddr& sockAddr);

public slots:
    void cleanupConnection(HifiSockAddr sockAddr);
    void clearConnections();
    
private slots:
    void processPendingDatagrams(int datagramCount);

    void handleSocketError(QAbstractSocket::SocketError socketError);
    void handleStateChanged(QAbstractSocket::SocketState socketState);

private:
    void setSystemBufferSizes();
    Connection* findOrCreateConnection(const HifiSockAddr& sockAddr);
    bool socketMatchesNodeOrDomain(const HifiSockAddr& sockAddr);
   
    // privatized methods used by UDTTest - they are private since they must be called on the Socket thread
    ConnectionStats::Stats sampleStatsForConnection(const HifiSockAddr& destination);
    
    std::vector<HifiSockAddr> getConnectionSockAddrs();
    void connectToSendSignal(const HifiSockAddr& destinationAddr, QObject* receiver, const char* slot);
    
    Q_INVOKABLE void writeReliablePacket(Packet* packet, const HifiSockAddr& sockAddr);
    Q_INVOKABLE void writeReliablePacketList(PacketList* packetList, const HifiSockAddr& sockAddr);

    int _sockFD;
    uint16_t _localPort;

    PacketFilterOperator _packetFilterOperator;
    PacketHandler _packetHandler;
    MessageHandler _messageHandler;
    MessageFailureHandler _messageFailureHandler;
    ConnectionCreationFilterOperator _connectionCreationFilterOperator;

    Mutex _unreliableSequenceNumbersMutex;

    std::unordered_map<HifiSockAddr, BasePacketHandler> _unfilteredHandlers;
    std::unordered_map<HifiSockAddr, SequenceNumber> _unreliableSequenceNumbers;
    std::unordered_map<HifiSockAddr, std::unique_ptr<Connection>> _connectionsHash;

    int _maxBandwidth { -1 };

    std::unique_ptr<CongestionControlVirtualFactory> _ccFactory { new CongestionControlFactory<TCPVegasCC>() };

    bool _shouldChangeSocketOptions { true };

    int _lastPacketSizeRead { 0 };
    SequenceNumber _lastReceivedSequenceNumber;
    HifiSockAddr _lastPacketSockAddr;

    tbb::concurrent_queue<Datagram> _incomingDatagrams;

    PacketReciever _packetReciever;
    
    friend UDTTest;
};

} // namespace udt

#endif // hifi_Socket_h
