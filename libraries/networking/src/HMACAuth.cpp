//
//  HMACAuth.cpp
//  libraries/networking/src
//
//  Created by Simon Walton on 3/19/2018.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HMACAuth.h"

#include <cassert>

#include <openssl/opensslv.h>
#include <openssl/hmac.h>

#include "NetworkLogging.h"

#include <QUuid>

#if OPENSSL_VERSION_NUMBER >= 0x10100000
HMACAuth::HMACAuth(AuthMethod authMethod)
    : _hmacContext(HMAC_CTX_new())
    , _authMethod(authMethod) { }

HMACAuth::~HMACAuth()
{
    HMAC_CTX_free(_hmacContext);
}

#else

HMACAuth::HMACAuth(AuthMethod authMethod)
    : _hmacContext(new HMAC_CTX())
    , _authMethod(authMethod) {
    HMAC_CTX_init(_hmacContext);
}

HMACAuth::~HMACAuth() {
    HMAC_CTX_cleanup(_hmacContext);
    delete _hmacContext;
}
#endif

const EVP_MD* hashFunctionForAuthMethod(HMACAuth::AuthMethod authMethod) {

    switch (authMethod) {
        case HMACAuth::MD5:
            return EVP_md5();

        case HMACAuth::SHA1:
            return EVP_sha1();

        case HMACAuth::SHA224:
            return EVP_sha224();

        case HMACAuth::SHA256:
            return EVP_sha256();

        case HMACAuth::RIPEMD160:
            return EVP_ripemd160();

        default:
            return nullptr;
    }
}

bool HMACAuth::setKey(const char* keyValue, int keyLen) {
    const EVP_MD* sslStruct = hashFunctionForAuthMethod(_authMethod);
    if (sslStruct) {
        QMutexLocker lock(&_lock);
        return (bool) HMAC_Init_ex(_hmacContext, keyValue, keyLen, sslStruct, nullptr);
    } else {
        return false;
    }
}

bool HMACAuth::setKey(const QUuid& uidKey) {
    const QByteArray rfcBytes(uidKey.toRfc4122());
    return setKey(rfcBytes.constData(), rfcBytes.length());
}

bool HMACAuth::reset() {
    const EVP_MD* sslStruct = hashFunctionForAuthMethod(_authMethod);
    if (sslStruct) {
        QMutexLocker lock(&_lock);
        // call HMAC_Init_ex with a null key (keeping it unchanged)
        // and our current EVP_MD, forcing an internal state reset
        return (bool) HMAC_Init_ex(_hmacContext, nullptr, 0, sslStruct, nullptr);
    } else {
        return false;
    }
}

bool HMACAuth::addData(const char* data, int dataLen) {
    QMutexLocker lock(&_lock);
    return (bool) HMAC_Update(_hmacContext, reinterpret_cast<const unsigned char*>(data), dataLen);
}

HMACAuth::HMACHash HMACAuth::result() {
    HMACHash hashValue(EVP_MAX_MD_SIZE);
    unsigned int  hashLen;
    QMutexLocker lock(&_lock);
    auto hmacResult = HMAC_Final(_hmacContext, &hashValue[0], &hashLen);

    if (hmacResult) {
        hashValue.resize((size_t) hashLen);
    } else {
        // the HMAC_FINAL call failed - should not be possible to get into this state
        qCWarning(networking) << "Error occured calling HMAC_Final";
        assert(hmacResult);
    }

    // Clear state for possible reuse.
    reset();
    return hashValue;
}
