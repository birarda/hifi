//
//  FBXBaker.cpp
//  tools/oven/src
//
//  Created by Stephen Birarda on 3/30/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cmath> // need this include so we don't get an error looking for std::isnan

#include <fbxsdk.h>

#include <QtConcurrent>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <mutex>

#include <NetworkAccessManager.h>
#include <SharedUtil.h>

#include "ModelBakingLoggingCategory.h"
#include "TextureBaker.h"

#include "FBXBaker.h"
#include "draco\mesh\mesh.h"
#include "draco\io\obj_encoder.h"
#include "draco\core\draco_types.h"
#include "draco\mesh\triangle_soup_mesh_builder.h"
#include "draco\compression\encode.h"
#include <fstream>
#include "draco\io\obj_decoder.h"
#include "draco\compression\decode.h"
#include "fbxsdk\core\base\fbxarray.h"


#  ifndef foreach
#    define foreach Q_FOREACH
#  endif

using namespace::draco;
std::once_flag onceFlag;
FBXSDKManagerUniquePointer FBXBaker::_sdkManager{ nullptr };

FBXBaker::FBXBaker(const QUrl& fbxURL, const QString& baseOutputPath,
                   TextureBakerThreadGetter textureThreadGetter, bool copyOriginals) :
    _fbxURL(fbxURL),
    _baseOutputPath(baseOutputPath),
    _textureThreadGetter(textureThreadGetter),
    _copyOriginals(copyOriginals) {
    std::call_once(onceFlag, []() {
        // create the static FBX SDK manager
        _sdkManager = FBXSDKManagerUniquePointer(FbxManager::Create(), [](FbxManager* manager) {
            manager->Destroy();
        });
    });

    // grab the name of the FBX from the URL, this is used for folder output names
    auto fileName = fbxURL.fileName();
    _fbxName = fileName.left(fileName.lastIndexOf('.'));
}

static const QString BAKED_OUTPUT_SUBFOLDER = "baked/";
static const QString ORIGINAL_OUTPUT_SUBFOLDER = "original/";

QString FBXBaker::pathToCopyOfOriginal() const {
    return _uniqueOutputPath + ORIGINAL_OUTPUT_SUBFOLDER + _fbxURL.fileName();
}

void FBXBaker::bake() {
    qCDebug(model_baking) << "Baking" << _fbxURL;

    // setup the output folder for the results of this bake
    setupOutputFolder();

    if (hasErrors()) {
        return;
    }

    connect(this, &FBXBaker::sourceCopyReadyToLoad, this, &FBXBaker::bakeSourceCopy);

    // make a local copy of the FBX file
    loadSourceFBX();
}

void FBXBaker::bakeSourceCopy() {
    // load the scene from the FBX file
    importScene();

    if (hasErrors()) {
        return;
    }

    // Perforrm mesh compression using Draco
    compressMesh();
    //Test();
    if (hasErrors()) {
        return;
    }

    // enumerate the textures found in the scene and start a bake for them
    rewriteAndBakeSceneTextures();

    if (hasErrors()) {
        return;
    }

    // export the FBX with re-written texture references
    exportScene();

    if (hasErrors()) {
        return;
    }

    // check if we're already done with textures (in case we had none to re-write)
    checkIfTexturesFinished();
}

void FBXBaker::setupOutputFolder() {
    // construct the output path using the name of the fbx and the base output path
    _uniqueOutputPath = _baseOutputPath + "/" + _fbxName + "/";

    // make sure there isn't already an output directory using the same name
    int iteration = 0;

    while (QDir(_uniqueOutputPath).exists()) {
        _uniqueOutputPath = _baseOutputPath + "/" + _fbxName + "-" + QString::number(++iteration) + "/";
    }

    qCDebug(model_baking) << "Creating FBX output folder " << _uniqueOutputPath;

    // attempt to make the output folder
    if (!QDir().mkdir(_uniqueOutputPath)) {
        handleError("Failed to create FBX output folder " + _uniqueOutputPath);
        return;
    }

    // make the baked and original sub-folders used during export
    QDir uniqueOutputDir = _uniqueOutputPath;
    if (!uniqueOutputDir.mkdir(BAKED_OUTPUT_SUBFOLDER) || !uniqueOutputDir.mkdir(ORIGINAL_OUTPUT_SUBFOLDER)) {
        handleError("Failed to create baked/original subfolders in " + _uniqueOutputPath);
        return;
    }
}

void FBXBaker::loadSourceFBX() {
    // check if the FBX is local or first needs to be downloaded
    if (_fbxURL.isLocalFile()) {
        // load up the local file
        QFile localFBX{ _fbxURL.toLocalFile() };

        // make a copy in the output folder
        localFBX.copy(pathToCopyOfOriginal());

        // emit our signal to start the import of the FBX source copy
        emit sourceCopyReadyToLoad();
    } else {
        // remote file, kick off a download
        auto& networkAccessManager = NetworkAccessManager::getInstance();

        QNetworkRequest networkRequest;

        // setup the request to follow re-directs and always hit the network
        networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        networkRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);


        networkRequest.setUrl(_fbxURL);

        qCDebug(model_baking) << "Downloading" << _fbxURL;
        auto networkReply = networkAccessManager.get(networkRequest);

        connect(networkReply, &QNetworkReply::finished, this, &FBXBaker::handleFBXNetworkReply);
    }
}

void FBXBaker::handleFBXNetworkReply() {
    auto requestReply = qobject_cast<QNetworkReply*>(sender());

    if (requestReply->error() == QNetworkReply::NoError) {
        qCDebug(model_baking) << "Downloaded" << _fbxURL;

        // grab the contents of the reply and make a copy in the output folder
        QFile copyOfOriginal(pathToCopyOfOriginal());

        qDebug(model_baking) << "Writing copy of original FBX to" << copyOfOriginal.fileName();

        if (!copyOfOriginal.open(QIODevice::WriteOnly) || (copyOfOriginal.write(requestReply->readAll()) == -1)) {
            // add an error to the error list for this FBX stating that a duplicate of the original FBX could not be made
            handleError("Could not create copy of " + _fbxURL.toString());
            return;
        }

        // close that file now that we are done writing to it
        copyOfOriginal.close();

        // emit our signal to start the import of the FBX source copy
        emit sourceCopyReadyToLoad();
    } else {
        // add an error to our list stating that the FBX could not be downloaded
        handleError("Failed to download " + _fbxURL.toString());
    }
}

void FBXBaker::importScene() {
    // create an FBX SDK importer
    FbxImporter* importer = FbxImporter::Create(_sdkManager.get(), "");

    // import the copy of the original FBX file
    QString originalCopyPath = pathToCopyOfOriginal();
    bool importStatus = importer->Initialize(originalCopyPath.toLocal8Bit().data());

    if (!importStatus) {
        // failed to initialize importer, print an error and return
        handleError("Failed to import " + _fbxURL.toString() + " - " + importer->GetStatus().GetErrorString());
        return;
    } else {
        qCDebug(model_baking) << "Imported" << _fbxURL << "to FbxScene";
    }

    // setup a new scene to hold the imported file
    _scene = FbxScene::Create(_sdkManager.get(), "bakeScene");

    // import the file to the created scene
    importer->Import(_scene);

    // destroy the importer that is no longer needed
    importer->Destroy();
}

QString texturePathRelativeToFBX(QUrl fbxURL, QUrl textureURL) {
    auto fbxPath = fbxURL.toString(QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment);
    auto texturePath = textureURL.toString(QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment);

    if (texturePath.startsWith(fbxPath)) {
        // texture path is a child of the FBX path, return the texture path without the fbx path
        return texturePath.mid(fbxPath.length());
    } else {
        // the texture path was not a child of the FBX path, return the empty string
        return "";
    }
}

QString FBXBaker::createBakedTextureFileName(const QFileInfo& textureFileInfo) {
    // first make sure we have a unique base name for this texture
    // in case another texture referenced by this model has the same base name
    auto nameMatches = _textureNameMatchCount[textureFileInfo.baseName()];

    QString bakedTextureFileName{ textureFileInfo.completeBaseName() };

    if (nameMatches > 0) {
        // there are already nameMatches texture with this name
        // append - and that number to our baked texture file name so that it is unique
        bakedTextureFileName += "-" + QString::number(nameMatches);
    }

    bakedTextureFileName += BAKED_TEXTURE_EXT;

    // increment the number of name matches
    ++nameMatches;

    return bakedTextureFileName;
}

QUrl FBXBaker::getTextureURL(const QFileInfo& textureFileInfo, FbxFileTexture* fileTexture) {
    QUrl urlToTexture;

    if (textureFileInfo.exists() && textureFileInfo.isFile()) {
        // set the texture URL to the local texture that we have confirmed exists
        urlToTexture = QUrl::fromLocalFile(textureFileInfo.absoluteFilePath());
    } else {
        // external texture that we'll need to download or find

        // first check if it the RelativePath to the texture in the FBX was relative
        QString relativeFileName = fileTexture->GetRelativeFileName();
        auto apparentRelativePath = QFileInfo(relativeFileName.replace("\\", "/"));

        // this is a relative file path which will require different handling
        // depending on the location of the original FBX
        if (_fbxURL.isLocalFile() && apparentRelativePath.exists() && apparentRelativePath.isFile()) {
            // the absolute path we ran into for the texture in the FBX exists on this machine
            // so use that file
            urlToTexture = QUrl::fromLocalFile(apparentRelativePath.absoluteFilePath());
        } else {
            // we didn't find the texture on this machine at the absolute path
            // so assume that it is right beside the FBX to match the behaviour of interface
            urlToTexture = _fbxURL.resolved(apparentRelativePath.fileName());
        }
    }

    return urlToTexture;
}

image::TextureUsage::Type textureTypeForMaterialProperty(FbxProperty& property, FbxSurfaceMaterial* material) {
    using namespace image::TextureUsage;

    // this is a property we know has a texture, we need to match it to a High Fidelity known texture type
    // since that information is passed to the baking process

    // grab the hierarchical name for this property and lowercase it for case-insensitive compare
    auto propertyName = QString(property.GetHierarchicalName()).toLower();

    // figure out the type of the property based on what known value string it matches
    if ((propertyName.contains("diffuse") && !propertyName.contains("tex_global_diffuse"))
        || propertyName.contains("tex_color_map")) {
        return ALBEDO_TEXTURE;
    } else if (propertyName.contains("transparentcolor") || propertyName.contains("transparencyfactor")) {
        return ALBEDO_TEXTURE;
    } else if (propertyName.contains("bump")) {
        return BUMP_TEXTURE;
    } else if (propertyName.contains("normal")) {
        return NORMAL_TEXTURE;
    } else if ((propertyName.contains("specular") && !propertyName.contains("tex_global_specular"))
               || propertyName.contains("reflection")) {
        return SPECULAR_TEXTURE;
    } else if (propertyName.contains("tex_metallic_map")) {
        return METALLIC_TEXTURE;
    } else if (propertyName.contains("shininess")) {
        return GLOSS_TEXTURE;
    } else if (propertyName.contains("tex_roughness_map")) {
        return ROUGHNESS_TEXTURE;
    } else if (propertyName.contains("emissive")) {
        return EMISSIVE_TEXTURE;
    } else if (propertyName.contains("ambientcolor")) {
        return LIGHTMAP_TEXTURE;
    } else if (propertyName.contains("ambientfactor")) {
        // we need to check what the ambient factor is, since that tells Interface to process this texture
        // either as an occlusion texture or a light map
        auto lambertMaterial = FbxCast<FbxSurfaceLambert>(material);

        if (lambertMaterial->AmbientFactor == 0) {
            return LIGHTMAP_TEXTURE;
        } else if (lambertMaterial->AmbientFactor > 0) {
            return OCCLUSION_TEXTURE;
        } else {
            return UNUSED_TEXTURE;
        }

    } else if (propertyName.contains("tex_ao_map")) {
        return OCCLUSION_TEXTURE;
    }

    return UNUSED_TEXTURE;
}

void FBXBaker::compressMesh() {

    FbxManager* manager = FbxManager::Create();
    FbxGeometryConverter converter(manager);

    FbxVector4 out_data;
    FbxVector4 out_data1, out_data2;

    std::vector<std::unique_ptr<Mesh>> dracoMeshes;

    std::vector<std::vector<float>> positions;
    std::unique_ptr<Mesh> dracoMesh;
    FbxMesh* mesh;

    std::vector<std::vector<float>> normals;
    std::vector<std::vector<float>> uvs;
    std::vector<unsigned long> indices;
    std::vector<std::vector<float>> colors;
    //std::string buffers="";
    std::vector<char>buffers;

    if (converter.Triangulate(_scene, true, false)) {

        int numGeometry = _scene->GetGeometryCount();
        for (int i = 0; i < numGeometry; i++) {

            FbxGeometry* geometry = _scene->GetGeometry(i);

            if (geometry && FbxCast<FbxMesh>(geometry)) {

                mesh = FbxCast<FbxMesh>(geometry);

                TriangleSoupMeshBuilder meshBuilder;
                auto numPolygons = mesh->GetPolygonCount();
                meshBuilder.Start(numPolygons);

                // Extract Position 

                auto numControlPoints = mesh->GetControlPointsCount();

                std::vector<std::vector<float>> controlPoints;

                auto numIndices = mesh->GetPolygonVertexCount();

                indices.reserve(numIndices);
                auto vertices = mesh->GetPolygonVertices();

                for (int i = 0; i < numIndices; ++i) {
                    indices.push_back(vertices[i]);
                }

                const int positionAttributeId = meshBuilder.AddAttribute(GeometryAttribute::POSITION, 3, DT_FLOAT32);
                int k = -1;
                for (int j = 0; j < numPolygons; j++) {

                    auto pos = mesh->GetControlPointAt(indices[++k]);
                    std::vector<float> v1{ (float)pos[0], (float)pos[1], (float)pos[2] };

                    auto pos1 = mesh->GetControlPointAt(indices[++k]);
                    std::vector<float> v2{ (float)pos1[0], (float)pos1[1], (float)pos1[2] };

                    auto pos2 = mesh->GetControlPointAt(indices[++k]);
                    std::vector<float> v3{ (float)pos2[0], (float)pos2[1], (float)pos2[2] };

                    meshBuilder.SetAttributeValuesForFace(positionAttributeId, FaceIndex(j),
                                                          v1.data(),
                                                          v2.data(),
                                                          v3.data());

                }

                // Extract Normals

                auto normalLayer = mesh->GetElementNormal(0);
                auto mode = normalLayer->GetMappingMode();
                assert(mode == FbxGeometryElement::eByControlPoint || mode == FbxGeometryElement::eByPolygonVertex);
                assert(normalLayer->GetReferenceMode() == FbxGeometryElement::eDirect);
                normals.assign(numIndices, { 0,0,0 });
                int f = 0;
                int normalAttributeId = 0;
                normalAttributeId = meshBuilder.AddAttribute(GeometryAttribute::NORMAL, 3, DT_FLOAT32);

                for (int i = 0; i < numIndices; ++i) {
                    FbxDouble* n;
                    int index = 0;
                    if (mode == FbxLayerElement::eByControlPoint) {
                        index = indices[i];
                    } else {
                        index = i;
                    }

                    n = normalLayer->GetDirectArray().GetAt(index).mData;
                    std::vector<float> v{ (float)n[0], (float)n[1], (float)n[2] };
                    normals[indices[i]] = v;

                    if ((i + 1) % 3 == 0) {

                        meshBuilder.SetAttributeValuesForFace(normalAttributeId, FaceIndex(f),
                                                              normals[indices[i - 2]].data(),
                                                              normals[indices[i - 1]].data(),
                                                              normals[indices[i]].data());

                        f++;
                    }
                }

                // UV

                uvs.assign(numIndices, { 0, 0 });
                auto uvLayer = mesh->GetLayer(0)->GetUVs();
                int uvAttributeId = 0;
                if (uvLayer) {

                    auto mode = uvLayer->GetMappingMode();
                    assert(mode == FbxGeometryElement::eByControlPoint || mode == FbxGeometryElement::eByPolygonVertex);
                    assert(uvLayer->GetReferenceMode() == FbxGeometryElement::eDirect);

                    int p = 0;
                    uvAttributeId = meshBuilder.AddAttribute(GeometryAttribute::TEX_COORD, 3, DT_FLOAT32);

                    for (int i = 0; i < numIndices; i++) {

                        FbxDouble* uv;
                        int index = 0;
                        if (uvLayer->GetMappingMode() == FbxLayerElement::eByControlPoint) {
                            index = indices[i];
                            uv = uvLayer->GetDirectArray().GetAt(index).mData;
                            std::vector<float> coord{ (float)uv[0], (float)uv[1] };
                            uvs[indices[i]] = coord;
                        } else {

                            index = i;
                            uv = uvLayer->GetDirectArray().GetAt(index).mData;
                            std::vector<float> coord{ (float)uv[0], (float)uv[1] };
                            uvs[i] = coord;
                        }

                        //uv = uvLayer->GetDirectArray().GetAt(index).mData;
                        //std::vector<float> coord{ (float)uv[0], (float)uv[1] };
                        //uvs[i] = coord;
                        //qCDebug(model_baking) << "UVs" << uvs[indices[i]];
                        //qCDebug(model_baking) << "UV's" << uvs[indices[i]][0] << uvs[indices[i]][1];

                        if ((i + 1) % 3 == 0) {

                            meshBuilder.SetAttributeValuesForFace(uvAttributeId, FaceIndex(p),
                                                                  uvs[indices[i - 2]].data(),
                                                                  uvs[indices[i - 1]].data(),
                                                                  uvs[indices[i]].data());

                            ++p;
                        }
                    }

                    //int x = -1;


                    //for (int p = 0; p < numPolygons; ++p) {

                    //    //if (p > numGeometry) break;

                    //    int x1 = ++x;
                    //    int x2 = ++x;
                    //    int x3 = ++x;
                    //    meshBuilder.SetAttributeValuesForFace(uvAttributeId, FaceIndex(p),
                    //        uvs[indices[x1]].data(),
                    //        uvs[indices[x2]].data(),
                    //        uvs[indices[x3]].data());


                    //}

                }

                // Adding Colors

                auto colorLayer = mesh->GetElementVertexColor();
                if (colorLayer) {

                    auto mode = colorLayer->GetMappingMode();

                    colors.assign(numIndices, { 0, 0, 0 });
                    assert(mode == FbxGeometryElement::eByControlPoint || mode == FbxGeometryElement::eByPolygonVertex);
                    assert(colorLayer->GetReferenceMode() == FbxGeometryElement::eDirect);

                    const int colorAttributeId = meshBuilder.AddAttribute(GeometryAttribute::COLOR, 3, DT_FLOAT32);
                    f = 0;

                    for (int i = 0; i < numIndices; ++i) {

                        int index = 0;
                        if (mode == FbxLayerElement::eByControlPoint) {
                            index = indices[i];

                        } else {
                            index = i;
                        }

                        auto color = colorLayer->GetDirectArray().GetAt(index);
                        std::vector<float> v{ (float)color[0], (float)color[1], (float)color[2] };
                        colors[indices[i]] = v;
                        //qCDebug(model_baking) << "Colors" << v[0] << v[1] << v[2];

                        if ((i + 1) % 3 == 0) {

                            meshBuilder.SetAttributeValuesForFace(colorAttributeId, FaceIndex(f),
                                                                  colors[indices[i - 2]].data(),
                                                                  colors[indices[i - 1]].data(),
                                                                  colors[indices[i]].data());

                            f++;
                        }

                    }
                }

                //Finalize Draco Mesh

                dracoMesh = meshBuilder.Finalize();

                // Encoding Draco Mesh to a buffer 

                draco::Encoder encoder;
                draco::EncoderBuffer buffer;
                encoder.EncodeMeshToBuffer(*dracoMesh, &buffer);

                FbxNode* rootNode = _scene->GetRootNode();
                FbxNode* customNode = FbxNode::Create(geometry, "Custom Node");
                FbxBlob blob(buffer.data(), buffer.size());
                FbxPropertyT<FbxBlob> property;
                property = FbxProperty::Create(customNode, FbxBlobDT, "DracoProperty");
                //qCDebug(model_baking) << "Out Buffer" << buffer.size();
                property.Set(blob);

                mesh->Reset();

                const std::string &file_name = "C:/Users/utkarsh/Desktop/result.drc";
                std::ofstream out_file(file_name, std::ios::binary);
                out_file.write(buffer.data(), buffer.size());
                qCDebug(model_baking) << "SizeBC" << buffer.size();
            }


        }

    } else {
        handleError("Could not triangulate all node attributes that can be triangulated");
        return;
    }

}

void FBXBaker::rewriteAndBakeSceneTextures() {

    // enumerate the surface materials to find the textures used in the scene
    int numMaterials = _scene->GetMaterialCount();
    for (int i = 0; i < numMaterials; i++) {
        FbxSurfaceMaterial* material = _scene->GetMaterial(i);

        if (material) {
            // enumerate the properties of this material to see what texture channels it might have
            FbxProperty property = material->GetFirstProperty();

            while (property.IsValid()) {
                // first check if this property has connected textures, if not we don't need to bother with it here
                if (property.GetSrcObjectCount<FbxTexture>() > 0) {

                    // figure out the type of texture from the material property
                    auto textureType = textureTypeForMaterialProperty(property, material);

                    if (textureType != image::TextureUsage::UNUSED_TEXTURE) {
                        int numTextures = property.GetSrcObjectCount<FbxFileTexture>();

                        for (int j = 0; j < numTextures; j++) {
                            FbxFileTexture* fileTexture = property.GetSrcObject<FbxFileTexture>(j);

                            // use QFileInfo to easily split up the existing texture filename into its components
                            QString fbxTextureFileName{ fileTexture->GetFileName() };
                            QFileInfo textureFileInfo{ fbxTextureFileName.replace("\\", "/") };

                            // make sure this texture points to something and isn't one we've already re-mapped
                            if (!textureFileInfo.filePath().isEmpty()
                                && textureFileInfo.suffix() != BAKED_TEXTURE_EXT.mid(1)) {

                                // construct the new baked texture file name and file path
                                // ensuring that the baked texture will have a unique name
                                // even if there was another texture with the same name at a different path
                                auto bakedTextureFileName = createBakedTextureFileName(textureFileInfo);
                                QString bakedTextureFilePath{
                                    _uniqueOutputPath + BAKED_OUTPUT_SUBFOLDER + bakedTextureFileName
                                };

                                qCDebug(model_baking).noquote() << "Re-mapping" << fileTexture->GetFileName()
                                    << "to" << bakedTextureFilePath;

                                // figure out the URL to this texture, embedded or external
                                auto urlToTexture = getTextureURL(textureFileInfo, fileTexture);

                                // write the new filename into the FBX scene
                                fileTexture->SetFileName(bakedTextureFilePath.toLocal8Bit());

                                // write the relative filename to be the baked texture file name since it will
                                // be right beside the FBX
                                fileTexture->SetRelativeFileName(bakedTextureFileName.toLocal8Bit().constData());

                                if (!_bakingTextures.contains(urlToTexture)) {
                                    // bake this texture asynchronously
                                    bakeTexture(urlToTexture, textureType, _uniqueOutputPath + BAKED_OUTPUT_SUBFOLDER);
                                }
                            }
                        }
                    }
                }

                property = material->GetNextProperty(property);
            }
        }
    }
}

void FBXBaker::bakeTexture(const QUrl& textureURL, image::TextureUsage::Type textureType, const QDir& outputDir) {
    // start a bake for this texture and add it to our list to keep track of
    QSharedPointer<TextureBaker> bakingTexture{
        new TextureBaker(textureURL, textureType, outputDir),
        &TextureBaker::deleteLater
    };

    // make sure we hear when the baking texture is done
    connect(bakingTexture.data(), &Baker::finished, this, &FBXBaker::handleBakedTexture);

    // keep a shared pointer to the baking texture
    _bakingTextures.insert(textureURL, bakingTexture);

    // start baking the texture on one of our available worker threads
    bakingTexture->moveToThread(_textureThreadGetter());
    QMetaObject::invokeMethod(bakingTexture.data(), "bake");
}

void FBXBaker::handleBakedTexture() {
    TextureBaker* bakedTexture = qobject_cast<TextureBaker*>(sender());

    // make sure we haven't already run into errors, and that this is a valid texture
    if (bakedTexture) {
        if (!hasErrors()) {
            if (!bakedTexture->hasErrors()) {
                if (_copyOriginals) {
                    // we've been asked to make copies of the originals, so we need to make copies of this if it is a linked texture

                    // use the path to the texture being baked to determine if this was an embedded or a linked texture

                    // it is embeddded if the texure being baked was inside the original output folder
                    // since that is where the FBX SDK places the .fbm folder it generates when importing the FBX

                    auto originalOutputFolder = QUrl::fromLocalFile(_uniqueOutputPath + ORIGINAL_OUTPUT_SUBFOLDER);

                    if (!originalOutputFolder.isParentOf(bakedTexture->getTextureURL())) {
                        // for linked textures we want to save a copy of original texture beside the original FBX

                        qCDebug(model_baking) << "Saving original texture for" << bakedTexture->getTextureURL();

                        // check if we have a relative path to use for the texture
                        auto relativeTexturePath = texturePathRelativeToFBX(_fbxURL, bakedTexture->getTextureURL());

                        QFile originalTextureFile{
                            _uniqueOutputPath + ORIGINAL_OUTPUT_SUBFOLDER + relativeTexturePath + bakedTexture->getTextureURL().fileName()
                        };

                        if (relativeTexturePath.length() > 0) {
                            // make the folders needed by the relative path
                        }

                        if (originalTextureFile.open(QIODevice::WriteOnly) && originalTextureFile.write(bakedTexture->getOriginalTexture()) != -1) {
                            qCDebug(model_baking) << "Saved original texture file" << originalTextureFile.fileName()
                                << "for" << _fbxURL;
                        } else {
                            handleError("Could not save original external texture " + originalTextureFile.fileName()
                                        + " for " + _fbxURL.toString());
                            return;
                        }
                    }
                }


                // now that this texture has been baked and handled, we can remove that TextureBaker from our hash
                _bakingTextures.remove(bakedTexture->getTextureURL());

                checkIfTexturesFinished();
            } else {
                // there was an error baking this texture - add it to our list of errors
                _errorList.append(bakedTexture->getErrors());

                // we don't emit finished yet so that the other textures can finish baking first
                _pendingErrorEmission = true;

                // now that this texture has been baked, even though it failed, we can remove that TextureBaker from our list
                _bakingTextures.remove(bakedTexture->getTextureURL());

                checkIfTexturesFinished();
            }
        } else {
            // we have errors to attend to, so we don't do extra processing for this texture
            // but we do need to remove that TextureBaker from our list
            // and then check if we're done with all textures
            _bakingTextures.remove(bakedTexture->getTextureURL());

            checkIfTexturesFinished();
        }
    }
}

void FBXBaker::exportScene() {
    // setup the exporter
    FbxExporter* exporter = FbxExporter::Create(_sdkManager.get(), "");

    auto rewrittenFBXPath = _uniqueOutputPath + BAKED_OUTPUT_SUBFOLDER + _fbxName + BAKED_FBX_EXTENSION;

    // save the relative path to this FBX inside our passed output folder
    _bakedFBXRelativePath = rewrittenFBXPath;
    _bakedFBXRelativePath.remove(_baseOutputPath + "/");

    bool exportStatus = exporter->Initialize(rewrittenFBXPath.toLocal8Bit().data());

    if (!exportStatus) {
        // failed to initialize exporter, print an error and return
        handleError("Failed to export FBX file at " + _fbxURL.toString() + " to " + rewrittenFBXPath
                    + "- error: " + exporter->GetStatus().GetErrorString());
    }

    // export the scene
    exporter->Export(_scene);
    qCDebug(model_baking) << "Exported" << _fbxURL << "with re-written paths to" << rewrittenFBXPath;
}


void FBXBaker::removeEmbeddedMediaFolder() {
    // now that the bake is complete, remove the embedded media folder produced by the FBX SDK when it imports an FBX
    auto embeddedMediaFolderName = _fbxURL.fileName().replace(".fbx", ".fbm");
    QDir(_uniqueOutputPath + ORIGINAL_OUTPUT_SUBFOLDER + embeddedMediaFolderName).removeRecursively();
}

void FBXBaker::possiblyCleanupOriginals() {
    if (!_copyOriginals) {
        // caller did not ask us to keep the original around, so delete the original output folder now
        QDir(_uniqueOutputPath + ORIGINAL_OUTPUT_SUBFOLDER).removeRecursively();
    }
}

void FBXBaker::checkIfTexturesFinished() {
    // check if we're done everything we need to do for this FBX
    // and emit our finished signal if we're done

    if (_bakingTextures.isEmpty()) {
        // remove the embedded media folder that the FBX SDK produces when reading the original
        removeEmbeddedMediaFolder();

        // cleanup the originals if we weren't asked to keep them around
        possiblyCleanupOriginals();

        if (hasErrors()) {
            // if we're checking for completion but we have errors
            // that means one or more of our texture baking operations failed

            if (_pendingErrorEmission) {
                emit finished();
            }

            return;
        } else {
            qCDebug(model_baking) << "Finished baking" << _fbxURL;

            emit finished();
        }
    }
}
