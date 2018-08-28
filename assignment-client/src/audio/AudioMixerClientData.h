//
//  AudioMixerClientData.h
//  assignment-client/src/audio
//
//  Created by Stephen Birarda on 10/18/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AudioMixerClientData_h
#define hifi_AudioMixerClientData_h

#include <queue>

#include <tbb/concurrent_vector.h>

#include <QtCore/QJsonObject>

#include <AABox.h>
#include <AudioHRTF.h>
#include <AudioLimiter.h>
#include <UUIDHasher.h>

#include <plugins/Forward.h>
#include <plugins/CodecPlugin.h>

#include "PositionalAudioStream.h"
#include "AvatarAudioStream.h"

class AudioMixerClientData : public NodeData {
    Q_OBJECT
public:
    struct AddedStream {
        NodeIDStreamID nodeIDStreamID;
        PositionalAudioStream* positionalStream;

        AddedStream(QUuid nodeID, Node::LocalID localNodeID,
                    StreamID streamID, PositionalAudioStream* positionalStream) :
            nodeIDStreamID(nodeID, localNodeID, streamID), positionalStream(positionalStream) {};
    };

    using ConcurrentAddedStreams = tbb::concurrent_vector<AddedStream>;

    AudioMixerClientData(const QUuid& nodeID, Node::LocalID nodeLocalID);
    ~AudioMixerClientData();

    using SharedStreamPointer = std::shared_ptr<PositionalAudioStream>;
    using AudioStreamVector = std::vector<SharedStreamPointer>;

    void queuePacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer node);
    void processPackets(ConcurrentAddedStreams& addedStreams);

    AudioStreamVector& getAudioStreams() { return _audioStreams; }
    AvatarAudioStream* getAvatarAudioStream();

    void removeAgentAvatarAudioStream();

    // packet parsers
    int parseData(ReceivedMessage& message) override;
    void processStreamPacket(ReceivedMessage& message, ConcurrentAddedStreams& addedStreams);
    void negotiateAudioFormat(ReceivedMessage& message, const SharedNodePointer& node);
    void parseRequestsDomainListData(ReceivedMessage& message);
    void parsePerAvatarGainSet(ReceivedMessage& message, const SharedNodePointer& node);
    void parseNodeIgnoreRequest(QSharedPointer<ReceivedMessage> message, const SharedNodePointer& node);
    void parseRadiusIgnoreRequest(QSharedPointer<ReceivedMessage> message, const SharedNodePointer& node);

    // attempt to pop a frame from each audio stream, and return the number of streams from this client
    int checkBuffersBeforeFrameSend();

    void removeDeadInjectedStreams();

    QJsonObject getAudioStreamStats();

    void sendAudioStreamStatsPackets(const SharedNodePointer& destinationNode);

    void incrementOutgoingMixedAudioSequenceNumber() { _outgoingMixedAudioSequenceNumber++; }
    quint16 getOutgoingSequenceNumber() const { return _outgoingMixedAudioSequenceNumber; }

    // uses randomization to have the AudioMixer send a stats packet to this node around every second
    bool shouldSendStats(int frameNumber);

    float getMasterAvatarGain() const { return _masterAvatarGain; }
    void setMasterAvatarGain(float gain) { _masterAvatarGain = gain; }

    AudioLimiter audioLimiter;

    void setupCodec(CodecPluginPointer codec, const QString& codecName);
    void cleanupCodec();
    void encode(const QByteArray& decodedBuffer, QByteArray& encodedBuffer) {
        if (_encoder) {
            _encoder->encode(decodedBuffer, encodedBuffer);
        } else {
            encodedBuffer = decodedBuffer;
        }
        // once you have encoded, you need to flush eventually.
        _shouldFlushEncoder = true;
    }
    void encodeFrameOfZeros(QByteArray& encodedZeros);
    bool shouldFlushEncoder() { return _shouldFlushEncoder; }

    QString getCodecName() { return _selectedCodecName; }

    bool shouldMuteClient() { return _shouldMuteClient; }
    void setShouldMuteClient(bool shouldMuteClient) { _shouldMuteClient = shouldMuteClient; }
    glm::vec3 getPosition() { return getAvatarAudioStream() ? getAvatarAudioStream()->getPosition() : glm::vec3(0); }
    bool getRequestsDomainListData() { return _requestsDomainListData; }
    void setRequestsDomainListData(bool requesting) { _requestsDomainListData = requesting; }

    void setupCodecForReplicatedAgent(QSharedPointer<ReceivedMessage> message);

    struct MixableStream {
        float approximateVolume { 0.0f };
        NodeIDStreamID nodeStreamID;
        std::unique_ptr<AudioHRTF> hrtf;
        PositionalAudioStream* positionalStream;
        bool ignoredByListener { false };
        bool ignoringListener { false };
        bool completedSilentRender { false };

        MixableStream(NodeIDStreamID nodeIDStreamID, PositionalAudioStream* positionalStream) :
            nodeStreamID(nodeIDStreamID), hrtf(new AudioHRTF), positionalStream(positionalStream) {};
        MixableStream(QUuid nodeID, Node::LocalID localNodeID, StreamID streamID, PositionalAudioStream* positionalStream) :
            nodeStreamID(nodeID, localNodeID, streamID), hrtf(new AudioHRTF), positionalStream(positionalStream) {};
    };

    using MixableStreamsVector = std::vector<MixableStream>;

    MixableStreamsVector& getMixableStreams() { return _mixableStreams; }

    // thread-safe, called from AudioMixerSlave(s) while processing ignore packets for other nodes
    void ignoredByNode(QUuid nodeID);
    void unignoredByNode(QUuid nodeID);

    // start of methods called non-concurrently from single AudioMixerSlave mixing for the owning node

    const Node::IgnoredNodeIDs& getNewIgnoredNodeIDs() const { return _newIgnoredNodeIDs; }
    const Node::IgnoredNodeIDs& getNewUnignoredNodeIDs() const { return _newUnignoredNodeIDs; }

    using ConcurrentIgnoreNodeIDs = tbb::concurrent_vector<QUuid>;
    const ConcurrentIgnoreNodeIDs& getNewIgnoringNodeIDs() const { return _newIgnoringNodeIDs; }
    const ConcurrentIgnoreNodeIDs& getNewUnignoringNodeIDs() const { return _newUnignoringNodeIDs; }

    void clearStagedIgnoreChanges();

    const Node::IgnoredNodeIDs& getIgnoringNodeIDs() const { return _ignoringNodeIDs; }

    bool getHasReceivedFirstMix() const { return _hasReceivedFirstMix; }
    void setHasReceivedFirstMix(bool hasReceivedFirstMix) { _hasReceivedFirstMix = hasReceivedFirstMix; }

    // end of methods called non-concurrently from single AudioMixerSlave

signals:
    void injectorStreamFinished(const QUuid& streamIdentifier);

public slots:
    void handleMismatchAudioFormat(SharedNodePointer node, const QString& currentCodec, const QString& recievedCodec);
    void sendSelectAudioFormat(SharedNodePointer node, const QString& selectedCodecName);

private:
    struct PacketQueue : public std::queue<QSharedPointer<ReceivedMessage>> {
        QWeakPointer<Node> node;
    };
    PacketQueue _packetQueue;

    QReadWriteLock _streamsLock;
    AudioStreamVector _audioStreams; // microphone stream from avatar is stored under key of null UUID

    void optionallyReplicatePacket(ReceivedMessage& packet, const Node& node);

    void setGainForAvatar(QUuid nodeID, uint8_t gain);

    MixableStreamsVector _mixableStreams;

    quint16 _outgoingMixedAudioSequenceNumber;

    AudioStreamStats _downstreamAudioStreamStats;

    int _frameToSendStats { 0 };

    float _masterAvatarGain { 1.0f };   // per-listener mixing gain, applied only to avatars

    CodecPluginPointer _codec;
    QString _selectedCodecName;
    Encoder* _encoder{ nullptr }; // for outbound mixed stream
    Decoder* _decoder{ nullptr }; // for mic stream

    bool _shouldFlushEncoder { false };

    bool _shouldMuteClient { false };
    bool _requestsDomainListData { false };

    std::vector<AddedStream> _newAddedStreams;

    Node::IgnoredNodeIDs _newIgnoredNodeIDs;
    Node::IgnoredNodeIDs _newUnignoredNodeIDs;

    tbb::concurrent_vector<QUuid> _newIgnoringNodeIDs;
    tbb::concurrent_vector<QUuid> _newUnignoringNodeIDs;

    std::mutex _ignoringNodeIDsMutex;
    Node::IgnoredNodeIDs _ignoringNodeIDs;

    std::atomic_bool _isIgnoreRadiusEnabled { false };

    bool _hasReceivedFirstMix { false };
};

#endif // hifi_AudioMixerClientData_h
