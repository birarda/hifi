//
//  AssetsBackupHandler.cpp
//  domain-server/src
//
//  Created by Clement Brisset on 1/12/18.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssetsBackupHandler.h"

#include <QJsonDocument>
#include <QDate>
#include <QtCore/QLoggingCategory>

#include <quazip5/quazipfile.h>
#include <quazip5/quazipdir.h>

#include <AssetClient.h>
#include <AssetRequest.h>
#include <AssetUpload.h>
#include <MappingRequest.h>
#include <PathUtils.h>

static const QString ASSETS_DIR = "/assets/";
static const QString MAPPINGS_FILE = "mappings.json";
static const QString ZIP_ASSETS_FOLDER = "files";

using namespace std;

Q_DECLARE_LOGGING_CATEGORY(asset_backup)
Q_LOGGING_CATEGORY(asset_backup, "hifi.asset-backup");

AssetsBackupHandler::AssetsBackupHandler(const QString& backupDirectory) :
    _assetsDirectory(backupDirectory + ASSETS_DIR)
{
    // Make sure the asset directory exists.
    QDir(_assetsDirectory).mkpath(".");

    refreshAssetsOnDisk();

    setupRefreshTimer();
}

void AssetsBackupHandler::setupRefreshTimer() {
    _mappingsRefreshTimer.setTimerType(Qt::CoarseTimer);
    _mappingsRefreshTimer.setSingleShot(true);
    QObject::connect(&_mappingsRefreshTimer, &QTimer::timeout, this, &AssetsBackupHandler::refreshMappings);

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    QObject::connect(nodeList.data(), &LimitedNodeList::nodeAdded, this, [this](SharedNodePointer node) {
        if (node->getType() == NodeType::AssetServer) {
            // run immediately for the first time.
            _mappingsRefreshTimer.start(0);
        }
    });
    QObject::connect(nodeList.data(), &LimitedNodeList::nodeKilled, this, [this](SharedNodePointer node) {
        if (node->getType() == NodeType::AssetServer) {
            _mappingsRefreshTimer.stop();
        }
    });
}

void AssetsBackupHandler::refreshAssetsOnDisk() {
    QDir assetsDir { _assetsDirectory };
    auto assetNames = assetsDir.entryList(QDir::Files);

    // store all valid hashes
    copy_if(begin(assetNames), end(assetNames),
            inserter(_assetsOnDisk, begin(_assetsOnDisk)),
            AssetUtils::isValidHash);

}

void AssetsBackupHandler::refreshAssetsInBackups() {
    _assetsInBackups.clear();
    for (const auto& backup : _backups) {
        for (const auto& mapping : backup.mappings) {
            _assetsInBackups.insert(mapping.second);
        }
    }
}

void AssetsBackupHandler::checkForMissingAssets() {
    vector<AssetUtils::AssetHash> missingAssets;
    set_difference(begin(_assetsInBackups), end(_assetsInBackups),
                   begin(_assetsOnDisk), end(_assetsOnDisk),
                   back_inserter(missingAssets));
    if (missingAssets.size() > 0) {
        qCWarning(asset_backup) << "Found" << missingAssets.size() << "backup assets missing from disk.";
    }
}

void AssetsBackupHandler::checkForAssetsToDelete() {
    vector<AssetUtils::AssetHash> deprecatedAssets;
    set_difference(begin(_assetsOnDisk), end(_assetsOnDisk),
                   begin(_assetsInBackups), end(_assetsInBackups),
                   back_inserter(deprecatedAssets));

    if (deprecatedAssets.size() > 0) {
        qCDebug(asset_backup) << "Found" << deprecatedAssets.size() << "backup assets to delete from disk.";
        if (_allBackupsLoadedSuccessfully) {
            for (const auto& hash : deprecatedAssets) {
                QFile::remove(_assetsDirectory + hash);
            }
        } else {
            qCWarning(asset_backup) << "Some backups did not load properly, aborting delete operation for safety.";
        }
    }
}


std::pair<bool, float> AssetsBackupHandler::isAvailable(QString filePath) {
    auto it = find_if(begin(_backups), end(_backups), [&](const std::vector<AssetServerBackup>::value_type& value) {
        return value.filePath == filePath;
    });
    if (it == end(_backups)) {
        return { true, 1.0f };
    }

    int mappingsMissing = 0;
    for (const auto& mapping : it->mappings) {
        if (_assetsLeftToRequest.find(mapping.second) != end(_assetsLeftToRequest)) {
            ++mappingsMissing;
        }
    }

    if (mappingsMissing == 0) {
        return { true, 1.0f };
    }

    float progress = (float)it->mappings.size();
    progress -= (float)mappingsMissing;
    progress /= it->mappings.size();

    return { false, progress };
}

std::pair<bool, float> AssetsBackupHandler::getRecoveryStatus() {
    if (_assetsLeftToUpload.empty() &&
        _mappingsLeftToSet.empty() &&
        _mappingsLeftToDelete.empty() &&
        _mappingRequestsInFlight == 0) {
        return { false, 1.0f };
    }

    float progress = (float)_numRestoreOperations;
    progress -= (float)_assetsLeftToUpload.size();
    progress -= (float)_mappingRequestsInFlight;
    progress /= (float)_numRestoreOperations;

    return { true, progress };
}

void AssetsBackupHandler::loadBackup(QuaZip& zip) {
    Q_ASSERT(QThread::currentThread() == thread());

    _backups.push_back({ zip.getZipName(), {}, false });
    auto& backup = _backups.back();

    if (!zip.setCurrentFile(MAPPINGS_FILE)) {
        qCCritical(asset_backup) << "Failed to find" << MAPPINGS_FILE << "while loading backup";
        qCCritical(asset_backup) << "    Error:" << zip.getZipError();
        backup.corruptedBackup = true;
        _allBackupsLoadedSuccessfully = false;
        return;
    }

    QuaZipFile zipFile { &zip };
    if (!zipFile.open(QFile::ReadOnly)) {
        qCCritical(asset_backup) << "Could not unzip backup file for load:" << zip.getZipName();
        qCCritical(asset_backup) << "    Error:" << zip.getZipError();
        backup.corruptedBackup = true;
        _allBackupsLoadedSuccessfully = false;
        return;
    }

    QJsonParseError error;
    auto document = QJsonDocument::fromJson(zipFile.readAll(), &error);
    if (document.isNull() || !document.isObject()) {
        qCCritical(asset_backup) << "Could not parse backup file to JSON object for load:" << zip.getZipName();
        qCCritical(asset_backup) << "    Error:" << error.errorString();
        backup.corruptedBackup = true;
        _allBackupsLoadedSuccessfully = false;
        return;
    }

    auto jsonObject = document.object();
    for (auto it = begin(jsonObject); it != end(jsonObject); ++it) {
        const auto& assetPath = it.key();
        const auto& assetHash = it.value().toString();

        if (!AssetUtils::isValidHash(assetHash)) {
            qCCritical(asset_backup) << "Corrupted mapping in loading backup file" << zip.getZipName() << ":" << it.key();
            backup.corruptedBackup = true;
            _allBackupsLoadedSuccessfully = false;
            continue;
        }

        backup.mappings[assetPath] = assetHash;
        _assetsInBackups.insert(assetHash);
    }

    checkForMissingAssets();
    checkForAssetsToDelete();
    return;
}

void AssetsBackupHandler::createBackup(QuaZip& zip) {
    Q_ASSERT(QThread::currentThread() == thread());

    if (operationInProgress()) {
        qCWarning(asset_backup) << "There is already an operation in progress.";
        return;
    }

    if (_lastMappingsRefresh == 0) {
        qCWarning(asset_backup) << "Current mappings not yet loaded.";
        return;
    }

    static constexpr quint64 MAX_REFRESH_TIME = 15 * 60 * 1000 * 1000;
    if (usecTimestampNow() - _lastMappingsRefresh > MAX_REFRESH_TIME) {
        qCWarning(asset_backup) << "Backing up asset mappings that might be stale.";
    }

    AssetServerBackup backup;
    backup.filePath = zip.getZipName();

    QJsonObject jsonObject;
    for (const auto& mapping : _currentMappings) {
        backup.mappings[mapping.first] = mapping.second;
        _assetsInBackups.insert(mapping.second);
        jsonObject.insert(mapping.first, mapping.second);
    }
    QJsonDocument document(jsonObject);

    QuaZipFile zipFile { &zip };
    if (!zipFile.open(QIODevice::WriteOnly, QuaZipNewInfo(MAPPINGS_FILE))) {
        qCDebug(asset_backup) << "Could not open zip file:" << zipFile.getZipError();
        return;
    }
    zipFile.write(document.toJson());
    zipFile.close();
    if (zipFile.getZipError() != UNZ_OK) {
        qCDebug(asset_backup) << "Could not close zip file: " << zipFile.getZipError();
        return;
    }
    _backups.push_back(backup);
}

void AssetsBackupHandler::recoverBackup(QuaZip& zip) {
    Q_ASSERT(QThread::currentThread() == thread());

    if (operationInProgress()) {
        qCWarning(asset_backup) << "There is already a backup/restore in progress.";
        return;
    }

    if (_lastMappingsRefresh == 0) {
        qCWarning(asset_backup) << "Current mappings not yet loaded.";
        return;
    }

    static constexpr quint64 MAX_REFRESH_TIME = 15 * 60 * 1000 * 1000;
    if (usecTimestampNow() - _lastMappingsRefresh > MAX_REFRESH_TIME) {
        qCWarning(asset_backup) << "Current asset mappings that might be stale.";
    }

    auto it = find_if(begin(_backups), end(_backups), [&](const std::vector<AssetServerBackup>::value_type& value) {
        return value.filePath == zip.getZipName();
    });
    if (it == end(_backups)) {
        qCDebug(asset_backup) << "Could not find backup" << zip.getZipName() << "to restore.";

        loadBackup(zip);

        QuaZipDir zipDir { &zip, ZIP_ASSETS_FOLDER };

        auto assetNames = zipDir.entryList(QDir::Files);
        for (const auto& asset : assetNames) {
            if (AssetUtils::isValidHash(asset)) {
                if (!zip.setCurrentFile(zipDir.filePath(asset))) {
                    qCCritical(asset_backup) << "Failed to find" << asset << "while recovering backup";
                    qCCritical(asset_backup) << "    Error:" << zip.getZipError();
                    continue;
                }

                QuaZipFile zipFile { &zip };
                if (!zipFile.open(QFile::ReadOnly)) {
                    qCCritical(asset_backup) << "Could not unzip asset file:" << asset;
                    qCCritical(asset_backup) << "    Error:" << zip.getZipError();
                    continue;
                }

                writeAssetFile(asset, zipFile.readAll());
            }
        }
    }

    const auto& newMappings = it->mappings;
    computeServerStateDifference(_currentMappings, newMappings);

    restoreAllAssets();
}

void AssetsBackupHandler::deleteBackup(QuaZip& zip) {
    Q_ASSERT(QThread::currentThread() == thread());

    if (operationInProgress()) {
        qCWarning(asset_backup) << "There is a backup/restore in progress.";
        return;
    }

    auto it = find_if(begin(_backups), end(_backups), [&](const std::vector<AssetServerBackup>::value_type& value) {
        return value.filePath == zip.getZipName();
    });
    if (it == end(_backups)) {
        qCDebug(asset_backup) << "Could not find backup" << zip.getZipName() << "to delete.";
        return;
    }

    refreshAssetsInBackups();
    checkForAssetsToDelete();
}

void AssetsBackupHandler::consolidateBackup(QuaZip& zip) {
    Q_ASSERT(QThread::currentThread() == thread());

    if (operationInProgress()) {
        qCWarning(asset_backup) << "There is a backup/restore in progress.";
        return;
    }
    QFileInfo zipInfo(zip.getZipName());

    auto it = find_if(begin(_backups), end(_backups), [&](const std::vector<AssetServerBackup>::value_type& value) {
        QFileInfo info(value.filePath);
        return info.fileName() == zipInfo.fileName();
    });
    if (it == end(_backups)) {
        qCDebug(asset_backup) << "Could not find backup" << zip.getZipName() << "to consolidate.";
        return;
    }

    for (const auto& mapping : it->mappings) {
        const auto& hash = mapping.second;

        QDir assetsDir { _assetsDirectory };
        QFile file { assetsDir.filePath(hash) };
        if (!file.open(QFile::ReadOnly)) {
            qCCritical(asset_backup) << "Could not open asset file" << file.fileName();
            continue;
        }

        QuaZipFile zipFile { &zip };
        if (!zipFile.open(QIODevice::WriteOnly, QuaZipNewInfo(ZIP_ASSETS_FOLDER + "/" + hash))) {
            qCDebug(asset_backup) << "Could not open zip file:" << zipFile.getZipError();
            continue;
        }
        zipFile.write(file.readAll());
        zipFile.close();
        if (zipFile.getZipError() != UNZ_OK) {
            qCDebug(asset_backup) << "Could not close zip file: " << zipFile.getZipError();
            continue;
        }
    }

}

void AssetsBackupHandler::refreshMappings() {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetAllMappingsRequest();

    QObject::connect(request, &GetAllMappingsRequest::finished, this, [this](GetAllMappingsRequest* request) {
        if (request->getError() == MappingRequest::NoError) {
            const auto& mappings = request->getMappings();
            _currentMappings.clear();
            for (const auto& mapping : mappings) {
                _currentMappings.insert({ mapping.first, mapping.second.hash });
            }
            _lastMappingsRefresh = usecTimestampNow();

            downloadMissingFiles(_currentMappings);
        } else {
            qCCritical(asset_backup) << "Could not refresh asset server mappings.";
            qCCritical(asset_backup) << "    Error:" << request->getErrorString();
        }

        request->deleteLater();

        // Launch next mappings request
        static constexpr int MAPPINGS_REFRESH_INTERVAL = 30 * 1000;
        _mappingsRefreshTimer.start(MAPPINGS_REFRESH_INTERVAL);
    });

    request->start();
}

void AssetsBackupHandler::downloadMissingFiles(const AssetUtils::Mappings& mappings) {
    auto wasEmpty = _assetsLeftToRequest.empty();

    for (const auto& mapping : mappings) {
        const auto& hash = mapping.second;
        if (_assetsOnDisk.find(hash) == end(_assetsOnDisk)) {
            _assetsLeftToRequest.insert(hash);
        }
    }

    // If we were empty, that means no download chain was already going, start one.
    if (wasEmpty) {
        downloadNextMissingFile();
    }
}

void AssetsBackupHandler::downloadNextMissingFile() {
    if (_assetsLeftToRequest.empty()) {
        return;
    }
    auto hash = *begin(_assetsLeftToRequest);

    auto assetClient = DependencyManager::get<AssetClient>();
    auto assetRequest = assetClient->createRequest(hash);

    QObject::connect(assetRequest, &AssetRequest::finished, this, [this](AssetRequest* request) {
        if (request->getError() == AssetRequest::NoError) {
            qCDebug(asset_backup) << "Backing up asset" << request->getHash();

            bool success = writeAssetFile(request->getHash(), request->getData());
            if (!success) {
                qCCritical(asset_backup) << "Failed to write asset file" << request->getHash();
            }
        } else {
            qCCritical(asset_backup) << "Failed to backup asset" << request->getHash();
        }

        _assetsLeftToRequest.erase(request->getHash());
        downloadNextMissingFile();

        request->deleteLater();
    });

    assetRequest->start();
}

bool AssetsBackupHandler::writeAssetFile(const AssetUtils::AssetHash& hash, const QByteArray& data) {
    QDir assetsDir { _assetsDirectory };
    QFile file { assetsDir.filePath(hash) };
    if (!file.open(QFile::WriteOnly)) {
        qCCritical(asset_backup) << "Could not open asset file for write:" << file.fileName();
        return false;
    }

    auto bytesWritten = file.write(data);
    if (bytesWritten != data.size()) {
        qCCritical(asset_backup) << "Could not write data to file" << file.fileName();
        file.remove();
        return false;
    }

    _assetsOnDisk.insert(hash);

    return true;
}

void AssetsBackupHandler::computeServerStateDifference(const AssetUtils::Mappings& currentMappings,
                                                    const AssetUtils::Mappings& newMappings) {
    _mappingsLeftToSet.reserve((int)newMappings.size());
    _assetsLeftToUpload.reserve((int)newMappings.size());
    _mappingsLeftToDelete.reserve((int)currentMappings.size());

    set<AssetUtils::AssetHash> currentAssets;
    for (const auto& currentMapping : currentMappings) {
        const auto& currentPath = currentMapping.first;
        const auto& currentHash = currentMapping.second;

        if (newMappings.find(currentPath) == end(newMappings)) {
            _mappingsLeftToDelete.push_back(currentPath);
        }
        currentAssets.insert(currentHash);
    }

    for (const auto& newMapping : newMappings) {
        const auto& newPath = newMapping.first;
        const auto& newHash = newMapping.second;

        auto it = currentMappings.find(newPath);
        if (it == end(currentMappings) || it->second != newHash) {
            _mappingsLeftToSet.push_back({ newPath, newHash });
        }
        if (currentAssets.find(newHash) == end(currentAssets)) {
            _assetsLeftToUpload.push_back(newHash);
        }
    }

    _numRestoreOperations = (int)_assetsLeftToUpload.size() + (int)_mappingsLeftToSet.size();
    if (!_mappingsLeftToDelete.empty()) {
        ++_numRestoreOperations;
    }

    qCDebug(asset_backup) << "Mappings to set:" << _mappingsLeftToSet.size();
    qCDebug(asset_backup) << "Mappings to del:" << _mappingsLeftToDelete.size();
    qCDebug(asset_backup) << "Assets to upload:" << _assetsLeftToUpload.size();
}

void AssetsBackupHandler::restoreAllAssets() {
    restoreNextAsset();
}

void AssetsBackupHandler::restoreNextAsset() {
    if (_assetsLeftToUpload.empty()) {
        updateMappings();
        return;
    }

    auto hash = _assetsLeftToUpload.back();
    _assetsLeftToUpload.pop_back();

    auto assetFilename = _assetsDirectory + hash;

    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createUpload(assetFilename);

    QObject::connect(request, &AssetUpload::finished, this, [this](AssetUpload* request) {
        if (request->getError() != AssetUpload::NoError) {
            qCCritical(asset_backup) << "Failed to restore asset:" << request->getFilename();
            qCCritical(asset_backup) << "    Error:" << request->getErrorString();
        }

        restoreNextAsset();

        request->deleteLater();
    });

    request->start();
}

void AssetsBackupHandler::updateMappings() {
    auto assetClient = DependencyManager::get<AssetClient>();
    for (const auto& mapping : _mappingsLeftToSet) {
        auto request = assetClient->createSetMappingRequest(mapping.first, mapping.second);
        QObject::connect(request, &SetMappingRequest::finished, this, [this](SetMappingRequest* request) {
            if (request->getError() != MappingRequest::NoError) {
                qCCritical(asset_backup) << "Failed to set mapping:" << request->getPath();
                qCCritical(asset_backup) << "    Error:" << request->getErrorString();
            }

            --_mappingRequestsInFlight;

            request->deleteLater();
        });

        request->start();
        ++_mappingRequestsInFlight;
    }
    _mappingsLeftToSet.clear();

    auto request = assetClient->createDeleteMappingsRequest(_mappingsLeftToDelete);
    QObject::connect(request, &DeleteMappingsRequest::finished, this, [this](DeleteMappingsRequest* request) {
        if (request->getError() != MappingRequest::NoError) {
            qCCritical(asset_backup) << "Failed to delete mappings";
            qCCritical(asset_backup) << "    Error:" << request->getErrorString();
        }

        --_mappingRequestsInFlight;

        request->deleteLater();
    });
    _mappingsLeftToDelete.clear();

    request->start();
    ++_mappingRequestsInFlight;
}