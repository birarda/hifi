//
//  Socket.cpp
//  libraries/networking/src/udt
//
//  Created by Stephen Birarda on 2015-07-20.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Socket.h"

#ifdef Q_OS_ANDROID
#include <sys/socket.h>
#endif

#include <QtCore/QThread>
#include <QtCore/QCoreApplication>

#include <shared/QtHelpers.h>
#include <LogHandler.h>
#include <Trace.h>
#include <ThreadHelpers.h>

#include "../NetworkLogging.h"
#include "../NLPacket.h"
#include "../NLPacketList.h"

#include "Connection.h"
#include "ControlPacket.h"
#include "Packet.h"
#include "PacketList.h"

using namespace udt;

PacketReciever::PacketReciever(tbb::concurrent_queue<Datagram>& incomingDatagrams)
    : _incomingDatagrams(incomingDatagrams)
{
}

void PacketReciever::run(int fd) {
    while (!thread()->isInterruptionRequested()) {
        static const int MAX_SIZE = 2048;
        // setup a buffer to read the packet into
        auto buffer = std::unique_ptr<char[]>(new char[MAX_SIZE]);

        struct sockaddr_in src_addr;

        struct iovec iov[1];
        iov[0].iov_base = buffer.get();
        iov[0].iov_len = MAX_SIZE;

        struct msghdr message;
        message.msg_name = &src_addr;
        message.msg_namelen = sizeof(src_addr);
        message.msg_iov = iov;
        message.msg_iovlen = 1;
        message.msg_control = 0;
        message.msg_controllen = 0;

        ssize_t size = recvmsg(fd, &message, 0);
        if (size < 0) {
            if (thread()->isInterruptionRequested()) {
                break;
            }
            qCCritical(networking) << "Failed to recv msg:" << strerror(errno);
        } else if (message.msg_flags & MSG_TRUNC) {
            qCCritical(networking) << "datagram too large for buffer: truncated";
        } else {
            // grab a time point we can mark as the receive time of this packet
            auto receiveTime = p_high_resolution_clock::now();

            QHostAddress senderAddress((sockaddr*)&src_addr);
            quint16 senderPort = ntohs(src_addr.sin_port);

            _incomingDatagrams.push({ senderAddress, senderPort, (int)size,
                std::move(buffer), receiveTime });

            emit pendingDatagrams(1);
        }

        QCoreApplication::processEvents();
    }

    thread()->exit();
}

Socket::Socket(QObject* parent, bool shouldChangeSocketOptions) :
    QObject(parent),
    _shouldChangeSocketOptions(shouldChangeSocketOptions),
    _packetReciever(_incomingDatagrams)
{
    _sockFD = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (_sockFD < 0) {
        qCCritical(networking) << "Cannot create socket";
        assert(false);
    }
    moveToNewNamedThread(&_packetReciever, "PacketReciever", [this] {
        _packetReciever.run(_sockFD);
    }, QThread::TimeCriticalPriority);

    connect(&_packetReciever, &PacketReciever::pendingDatagrams, this, &Socket::processPendingDatagrams);

    // make sure we hear about errors and state changes from the underlying socket
//    connect(&_udpSocket, SIGNAL(error(QAbstractSocket::SocketError)),
//            this, SLOT(handleSocketError(QAbstractSocket::SocketError)));
//    connect(&_udpSocket, &QAbstractSocket::stateChanged, this, &Socket::handleStateChanged);
}

Socket::~Socket() {
    _packetReciever.thread()->requestInterruption();
    ::close(_sockFD);
}

void Socket::bind(const QHostAddress& address, quint16 port) {
    // TODO: ignoring address right now
    struct sockaddr_in sockAddr;
    memset((char *)&sockAddr, 0, sizeof(sockAddr));
    sockAddr.sin_family = AF_INET;
    sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    sockAddr.sin_port = htons(port);

    if (::bind(_sockFD, (struct sockaddr*)&sockAddr, sizeof(sockAddr)) < 0) {
        qCCritical(networking) << "Bind failed";
        assert(false);
    }

    struct sockaddr_in localAddress;
    socklen_t addressLength = sizeof(localAddress);;
    ::getsockname(_sockFD, (struct sockaddr*)&localAddress, &addressLength);
    _localPort = ntohs(localAddress.sin_port);

    if (_shouldChangeSocketOptions) {
        setSystemBufferSizes();

#if defined(Q_OS_LINUX)
        int val = IP_PMTUDISC_DONT;
        setsockopt(_sockFD, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
#elif defined(Q_OS_WINDOWS)
        int val = 0; // false
        setsockopt(_sockFD, IPPROTO_IP, IP_DONTFRAGMENT, &val, sizeof(val));
#endif
    }
}

void Socket::rebind() {
    rebind(localPort());
}

void Socket::rebind(quint16 localPort) {
    _packetReciever.thread()->requestInterruption();
    ::close(_sockFD);
    bind(QHostAddress::AnyIPv4, localPort);
    moveToNewNamedThread(&_packetReciever, "PacketReciever", [this] {
        _packetReciever.run(_sockFD);
    }, QThread::TimeCriticalPriority);
}

void Socket::setSystemBufferSizes() {
    uint32_t recvBufferSize = 0;
    uint32_t sendBufferSize = 0;
    uint32_t optLen = sizeof(int);

    ::getsockopt(_sockFD, SOL_SOCKET, SO_RCVBUF, &recvBufferSize, &optLen);

    if (recvBufferSize < udt::UDP_RECEIVE_BUFFER_SIZE_BYTES) {
        ::setsockopt(_sockFD, SOL_SOCKET, SO_RCVBUF, &udt::UDP_RECEIVE_BUFFER_SIZE_BYTES, optLen);
        qCDebug(networking) << "Changed socket receive buffer size from" << recvBufferSize << "to"
        << udt::UDP_RECEIVE_BUFFER_SIZE_BYTES << "bytes";
    }


    ::getsockopt(_sockFD, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &optLen);
    if (sendBufferSize < udt::UDP_SEND_BUFFER_SIZE_BYTES) {
        ::setsockopt(_sockFD, SOL_SOCKET, SO_SNDBUF, &udt::UDP_SEND_BUFFER_SIZE_BYTES, optLen);
        qCDebug(networking) << "Changed socket send buffer size from" << sendBufferSize << "to"
        << udt::UDP_SEND_BUFFER_SIZE_BYTES << "bytes";
    }

}

qint64 Socket::writeBasePacket(const udt::BasePacket& packet, const HifiSockAddr &sockAddr) {
    // Since this is a base packet we have no way to know if this is reliable or not - we just fire it off

    // this should not be called with an instance of Packet
    Q_ASSERT_X(!dynamic_cast<const Packet*>(&packet),
               "Socket::writeBasePacket", "Cannot send a Packet/NLPacket via writeBasePacket");

    return writeDatagram(packet.getData(), packet.getDataSize(), sockAddr);
}

qint64 Socket::writePacket(const Packet& packet, const HifiSockAddr& sockAddr) {
    Q_ASSERT_X(!packet.isReliable(), "Socket::writePacket", "Cannot send a reliable packet unreliably");

    SequenceNumber sequenceNumber;
    {
        Lock lock(_unreliableSequenceNumbersMutex);
        sequenceNumber = ++_unreliableSequenceNumbers[sockAddr];
    }

    // write the correct sequence number to the Packet here
    packet.writeSequenceNumber(sequenceNumber);

    return writeDatagram(packet.getData(), packet.getDataSize(), sockAddr);
}

qint64 Socket::writePacket(std::unique_ptr<Packet> packet, const HifiSockAddr& sockAddr) {

    if (packet->isReliable()) {
        // hand this packet off to writeReliablePacket
        // because Qt can't invoke with the unique_ptr we have to release it here and re-construct in writeReliablePacket

        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, "writeReliablePacket", Qt::QueuedConnection,
                                      Q_ARG(Packet*, packet.release()),
                                      Q_ARG(HifiSockAddr, sockAddr));
        } else {
            writeReliablePacket(packet.release(), sockAddr);
        }

        return 0;
    }

    return writePacket(*packet, sockAddr);
}

qint64 Socket::writePacketList(std::unique_ptr<PacketList> packetList, const HifiSockAddr& sockAddr) {
    if (packetList->isReliable()) {
        // hand this packetList off to writeReliablePacketList
        // because Qt can't invoke with the unique_ptr we have to release it here and re-construct in writeReliablePacketList

        if (packetList->getNumPackets() == 0) {
            qCWarning(networking) << "Trying to send packet list with 0 packets, bailing.";
            return 0;
        }


        if (QThread::currentThread() != thread()) {
            auto ptr = packetList.release();
            QMetaObject::invokeMethod(this, "writeReliablePacketList", Qt::AutoConnection,
                                      Q_ARG(PacketList*, ptr),
                                      Q_ARG(HifiSockAddr, sockAddr));
        } else {
            writeReliablePacketList(packetList.release(), sockAddr);
        }

        return 0;
    }

    // Unerliable and Unordered
    qint64 totalBytesSent = 0;
    while (!packetList->_packets.empty()) {
        totalBytesSent += writePacket(packetList->takeFront<Packet>(), sockAddr);
    }

    return totalBytesSent;
}

void Socket::writeReliablePacket(Packet* packet, const HifiSockAddr& sockAddr) {
    auto connection = findOrCreateConnection(sockAddr);
    if (connection) {
        connection->sendReliablePacket(std::unique_ptr<Packet>(packet));
    }
#ifdef UDT_CONNECTION_DEBUG
    else {
        qCDebug(networking) << "Socket::writeReliablePacket refusing to send packet - no connection was created";
    }
#endif

}

void Socket::writeReliablePacketList(PacketList* packetList, const HifiSockAddr& sockAddr) {
    auto connection = findOrCreateConnection(sockAddr);
    if (connection) {
        connection->sendReliablePacketList(std::unique_ptr<PacketList>(packetList));
    }
#ifdef UDT_CONNECTION_DEBUG
    else {
        qCDebug(networking) << "Socket::writeReliablePacketList refusing to send packet list - no connection was created";
    }
#endif
}

qint64 Socket::writeDatagram(const char* data, qint64 size, const HifiSockAddr& sockAddr) {
    return writeDatagram(QByteArray::fromRawData(data, size), sockAddr);
}

qint64 Socket::writeDatagram(const QByteArray& datagram, const HifiSockAddr& sockAddr) {

    struct sockaddr_in servaddr;
    memset((char*)&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(sockAddr.getAddress().toIPv4Address());
    servaddr.sin_port = htons(sockAddr.getPort());

    struct iovec iov[1];
    iov[0].iov_base = (void*)datagram.data();
    iov[0].iov_len = datagram.size();

    struct msghdr message;
    memset((char*)&message, 0, sizeof(message));
    message.msg_name = &servaddr;
    message.msg_namelen = sizeof(servaddr);
    message.msg_iov = iov;
    message.msg_iovlen = 1;
    message.msg_control = 0;
    message.msg_controllen = 0;

    qint64 bytesWritten = ::sendmsg(_sockFD, &message, 0);
    if (bytesWritten < 0) {
        qDebug() << sockAddr.getAddress() << sockAddr.getPort();
        qCCritical(networking) << "Failed to send msg:" << bytesWritten << strerror(errno);
    }

    return bytesWritten;
}

Connection* Socket::findOrCreateConnection(const HifiSockAddr& sockAddr) {
    auto it = _connectionsHash.find(sockAddr);

    if (it == _connectionsHash.end()) {
        // we did not have a matching connection, time to see if we should make one

        if (_connectionCreationFilterOperator && !_connectionCreationFilterOperator(sockAddr)) {
            // the connection creation filter did not allow us to create a new connection
#ifdef UDT_CONNECTION_DEBUG
            qCDebug(networking) << "Socket::findOrCreateConnection refusing to create connection for" << sockAddr
                << "due to connection creation filter";
#endif
            return nullptr;
        } else {
            auto congestionControl = _ccFactory->create();
            congestionControl->setMaxBandwidth(_maxBandwidth);
            auto connection = std::unique_ptr<Connection>(new Connection(this, sockAddr, std::move(congestionControl)));

            // allow higher-level classes to find out when connections have completed a handshake
            QObject::connect(connection.get(), &Connection::receiverHandshakeRequestComplete,
                             this, &Socket::clientHandshakeRequestComplete);

#ifdef UDT_CONNECTION_DEBUG
            qCDebug(networking) << "Creating new connection to" << sockAddr;
#endif

            it = _connectionsHash.insert(it, std::make_pair(sockAddr, std::move(connection)));
        }
    }

    return it->second.get();
}

void Socket::clearConnections() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "clearConnections");
        return;
    }

    if (_connectionsHash.size() > 0) {
        // clear all of the current connections in the socket
        qCDebug(networking) << "Clearing all remaining connections in Socket.";
        _connectionsHash.clear();
    }
}

void Socket::cleanupConnection(HifiSockAddr sockAddr) {
    auto numErased = _connectionsHash.erase(sockAddr);

    if (numErased > 0) {
#ifdef UDT_CONNECTION_DEBUG
        qCDebug(networking) << "Socket::cleanupConnection called for UDT connection to" << sockAddr;
#endif
    }
}

void Socket::messageReceived(std::unique_ptr<Packet> packet) {
    if (_messageHandler) {
        _messageHandler(std::move(packet));
    }
}

void Socket::messageFailed(Connection* connection, Packet::MessageNumber messageNumber) {
    if (_messageFailureHandler) {
        _messageFailureHandler(connection->getDestination(), messageNumber);
    }
}

void Socket::processPendingDatagrams(int) {
    Datagram datagram;
    while (_incomingDatagrams.try_pop(datagram)) {
        int datagramSize = datagram._datagramLength;
        auto receiveTime = datagram._receiveTime;
        HifiSockAddr senderSockAddr(datagram._senderAddress,
                                    datagram._senderPort);

        auto it = _unfilteredHandlers.find(senderSockAddr);
        if (it != _unfilteredHandlers.end()) {
            // we have a registered unfiltered handler for this HifiSockAddr (eg. STUN packet) - call that and return
            if (it->second) {
                auto basePacket = BasePacket::fromReceivedPacket(std::move(datagram._datagram),
                                                                 datagramSize, senderSockAddr);
                basePacket->setReceiveTime(receiveTime);
                it->second(std::move(basePacket));
            }
            continue;
        }

        // save information for this packet, in case it is the one that sticks readyRead
        _lastPacketSizeRead = datagramSize;
        _lastPacketSockAddr = senderSockAddr;

        // check if this was a control packet or a data packet
        bool isControlPacket = *reinterpret_cast<uint32_t*>(datagram._datagram.get()) & CONTROL_BIT_MASK;

        if (isControlPacket) {
            // setup a control packet from the data we just read
            auto controlPacket = ControlPacket::fromReceivedPacket(std::move(datagram._datagram), datagramSize, senderSockAddr);
            controlPacket->setReceiveTime(receiveTime);

            // move this control packet to the matching connection, if there is one
            auto connection = findOrCreateConnection(senderSockAddr);

            if (connection) {
                connection->processControl(move(controlPacket));
            }

        } else {
            // setup a Packet from the data we just read
            auto packet = Packet::fromReceivedPacket(std::move(datagram._datagram), datagramSize, senderSockAddr);
            packet->setReceiveTime(receiveTime);

            // save the sequence number in case this is the packet that sticks readyRead
            _lastReceivedSequenceNumber = packet->getSequenceNumber();

            // call our hash verification operator to see if this packet is verified
            if (!_packetFilterOperator || _packetFilterOperator(*packet)) {
                if (packet->isReliable()) {
                    // if this was a reliable packet then signal the matching connection with the sequence number
                    auto connection = findOrCreateConnection(senderSockAddr);

                    if (!connection || !connection->processReceivedSequenceNumber(packet->getSequenceNumber(),
                                                                                  packet->getDataSize(),
                                                                                  packet->getPayloadSize())) {
                        // the connection could not be created or indicated that we should not continue processing this packet
#ifdef UDT_CONNECTION_DEBUG
                        qCDebug(networking) << "Can't process packet: version" << (unsigned int)NLPacket::versionInHeader(*packet)
                            << ", type" << NLPacket::typeInHeader(*packet);
#endif
                        continue;
                    }
                }

                if (packet->isPartOfMessage()) {
                    auto connection = findOrCreateConnection(senderSockAddr);
                    if (connection) {
                        connection->queueReceivedMessagePacket(std::move(packet));
                    }
                } else if (_packetHandler) {
                    // call the verified packet callback to let it handle this packet
                    _packetHandler(std::move(packet));
                }
            }
        }
    }
}

void Socket::connectToSendSignal(const HifiSockAddr& destinationAddr, QObject* receiver, const char* slot) {
    auto it = _connectionsHash.find(destinationAddr);
    if (it != _connectionsHash.end()) {
        connect(it->second.get(), SIGNAL(packetSent()), receiver, slot);
    }
}

void Socket::setCongestionControlFactory(std::unique_ptr<CongestionControlVirtualFactory> ccFactory) {
    // swap the current unique_ptr for the new factory
    _ccFactory.swap(ccFactory);
}


void Socket::setConnectionMaxBandwidth(int maxBandwidth) {
    qInfo() << "Setting socket's maximum bandwith to" << maxBandwidth << "bps. ("
            << _connectionsHash.size() << "live connections)";
    _maxBandwidth = maxBandwidth;
    for (auto& pair : _connectionsHash) {
        auto& connection = pair.second;
        connection->setMaxBandwidth(_maxBandwidth);
    }
}

ConnectionStats::Stats Socket::sampleStatsForConnection(const HifiSockAddr& destination) {
    auto it = _connectionsHash.find(destination);
    if (it != _connectionsHash.end()) {
        return it->second->sampleStats();
    } else {
        return ConnectionStats::Stats();
    }
}

Socket::StatsVector Socket::sampleStatsForAllConnections() {
    StatsVector result;
    result.reserve(_connectionsHash.size());
    for (const auto& connectionPair : _connectionsHash) {
        result.emplace_back(connectionPair.first, connectionPair.second->sampleStats());
    }
    return result;
}


std::vector<HifiSockAddr> Socket::getConnectionSockAddrs() {
    std::vector<HifiSockAddr> addr;
    addr.reserve(_connectionsHash.size());

    for (const auto& connectionPair : _connectionsHash) {
        addr.push_back(connectionPair.first);
    }
    return addr;
}

void Socket::handleSocketError(QAbstractSocket::SocketError socketError) {
    HIFI_FCDEBUG(networking(), "udt::Socket error - " << socketError);
}

void Socket::handleStateChanged(QAbstractSocket::SocketState socketState) {
    if (socketState != QAbstractSocket::BoundState) {
        qCDebug(networking) << "udt::Socket state changed - state is now" << socketState;
    }
}

#if (PR_BUILD || DEV_BUILD)

void Socket::sendFakedHandshakeRequest(const HifiSockAddr& sockAddr) {
    auto connection = findOrCreateConnection(sockAddr);
    if (connection) {
        connection->sendHandshakeRequest();
    }
}

#endif
