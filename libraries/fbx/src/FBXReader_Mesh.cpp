//
//  FBXReader_Mesh.cpp
//  interface/src/fbx
//
//  Created by Sam Gateau on 8/27/2015.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <iostream>
#include <QBuffer>
#include <QDataStream>
#include <QIODevice>
#include <QStringList>
#include <QTextStream>
#include <QtDebug>
#include <QtEndian>
#include <QFileInfo>
#include <QHash>
#include <LogHandler.h>
#include "ModelFormatLogging.h"

#include "FBXReader.h"
#include <memory>
#include "draco\mesh\mesh.h"
#include "draco\io\obj_encoder.h"
#include "draco\core\draco_types.h"
#include "draco\mesh\triangle_soup_mesh_builder.h"
#include "draco\compression\encode.h"
#include <fstream>
#include "draco\io\obj_decoder.h"
#include "draco\compression\decode.h"

class Vertex {
public:
    int originalIndex;
    glm::vec2 texCoord;
    glm::vec2 texCoord1;
};

uint qHash(const Vertex& vertex, uint seed = 0) {
    return qHash(vertex.originalIndex, seed);
}

bool operator==(const Vertex& v1, const Vertex& v2) {
    return v1.originalIndex == v2.originalIndex && v1.texCoord == v2.texCoord && v1.texCoord1 == v2.texCoord1;
}

class AttributeData {
public:
    QVector<glm::vec2> texCoords;
    QVector<int> texCoordIndices;
    QString name;
    int index;
};

class MeshData {
public:
    ExtractedMesh extracted;
    QVector<glm::vec3> vertices;
    QVector<int> polygonIndices;
    bool normalsByVertex;
    QVector<glm::vec3> normals;
    QVector<int> normalIndices;

    bool colorsByVertex;
    glm::vec4 averageColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    QVector<glm::vec4> colors;
    QVector<int> colorIndices;

    QVector<glm::vec2> texCoords;
    QVector<int> texCoordIndices;

    QHash<Vertex, int> indices;

    std::vector<AttributeData> attributes;
};

void appendIndex(MeshData& data, QVector<int>& indices, int index) {
    if (index >= data.polygonIndices.size()) {
        return;
    }

    int vertexIndex = data.polygonIndices.at(index);
    if (vertexIndex < 0) {
        vertexIndex = -vertexIndex - 1;
    }
    Vertex vertex;
    vertex.originalIndex = vertexIndex;

    glm::vec3 position;
    if (vertexIndex < data.vertices.size()) {
        position = data.vertices.at(vertexIndex);
    }

    glm::vec3 normal;
    int normalIndex = data.normalsByVertex ? vertexIndex : index;
    if (data.normalIndices.isEmpty()) {
        if (normalIndex < data.normals.size()) {
            normal = data.normals.at(normalIndex);
        }
    } else if (normalIndex < data.normalIndices.size()) {
        normalIndex = data.normalIndices.at(normalIndex);
        if (normalIndex >= 0 && normalIndex < data.normals.size()) {
            normal = data.normals.at(normalIndex);
        }
    }


    glm::vec4 color;
    bool hasColors = (data.colors.size() > 1);
    if (hasColors) {
        int colorIndex = data.colorsByVertex ? vertexIndex : index;
        if (data.colorIndices.isEmpty()) {
            if (colorIndex < data.colors.size()) {
                color = data.colors.at(colorIndex);
            }
        } else if (colorIndex < data.colorIndices.size()) {
            colorIndex = data.colorIndices.at(colorIndex);
            if (colorIndex >= 0 && colorIndex < data.colors.size()) {
                color = data.colors.at(colorIndex);
            }
        }
    }

    if (data.texCoordIndices.isEmpty()) {
        if (index < data.texCoords.size()) {
            vertex.texCoord = data.texCoords.at(index);
        }
    } else if (index < data.texCoordIndices.size()) {
        int texCoordIndex = data.texCoordIndices.at(index);
        if (texCoordIndex >= 0 && texCoordIndex < data.texCoords.size()) {
            vertex.texCoord = data.texCoords.at(texCoordIndex);
        }
    }

    bool hasMoreTexcoords = (data.attributes.size() > 1);
    if (hasMoreTexcoords) {
        if (data.attributes[1].texCoordIndices.empty()) {
            if (index < data.attributes[1].texCoords.size()) {
                vertex.texCoord1 = data.attributes[1].texCoords.at(index);
            }
        } else if (index < data.attributes[1].texCoordIndices.size()) {
            int texCoordIndex = data.attributes[1].texCoordIndices.at(index);
            if (texCoordIndex >= 0 && texCoordIndex < data.attributes[1].texCoords.size()) {
                vertex.texCoord1 = data.attributes[1].texCoords.at(texCoordIndex);
            }
        }
    }

    QHash<Vertex, int>::const_iterator it = data.indices.find(vertex);
    if (it == data.indices.constEnd()) {
        int newIndex = data.extracted.mesh.vertices.size();
        indices.append(newIndex);
        data.indices.insert(vertex, newIndex);
        data.extracted.newIndices.insert(vertexIndex, newIndex);
        data.extracted.mesh.vertices.append(position);
        data.extracted.mesh.normals.append(normal);
        data.extracted.mesh.texCoords.append(vertex.texCoord);
        if (hasColors) {
            data.extracted.mesh.colors.append(glm::vec3(color));
        }
        if (hasMoreTexcoords) {
            data.extracted.mesh.texCoords1.append(vertex.texCoord1);
        }
    } else {
        indices.append(*it);
        data.extracted.mesh.normals[*it] += normal;
    }
}

ExtractedMesh FBXReader::extractMesh(const FBXNode& object, unsigned int& meshIndex) {
    MeshData data;
    data.extracted.mesh.meshIndex = meshIndex++;
    QVector<int> materials = {0};// { 0, 616 };
    QVector<int> textures = {};
    bool isMaterialPerPolygon = false;
    static const QVariant BY_VERTICE = QByteArray("ByVertice");
    static const QVariant INDEX_TO_DIRECT = QByteArray("IndexToDirect");
    int count = 0;
    bool dracoNode = false;
    foreach(const FBXNode& child, object.children) {

        if (child.name == "DracoMesh") {
            dracoNode = true;
            // Load draco mesh 
            draco::Decoder decoder;
            draco::DecoderBuffer decodedBuffer;
            QByteArray dracoArray = child.properties.at(0).value<QByteArray>();
            decodedBuffer.Init(dracoArray.data(), dracoArray.size());
            
            std::unique_ptr<draco::Mesh> dracoMesh1(new draco::Mesh());
            decoder.DecodeBufferToGeometry(&decodedBuffer, dracoMesh1.get());
            draco::Mesh* dracoMesh = dracoMesh1.get();
            
            // Process draco mesh into data.extracted
            ExtractedMesh &extractedMesh = data.extracted;
            
            // Positions
            
            auto positionAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
            QVector<glm::vec3> positionValues;
            
            if (positionAttribute) {
                
                std::array<float, 3> positionValue;
                for (draco::AttributeValueIndex i(0); i < positionAttribute->size(); ++i) {
                    positionAttribute->ConvertValue<float, 3>(i, &positionValue[0]);
                    float x = positionValue[0];
                    float y = positionValue[1];
                    float z = positionValue[2];
                    positionValues.append(glm::vec3(x, y, z));
                }

              }
            
            // Polygon Vertex Indices
            
            QVector<int> verticeIndices;
            for (draco::FaceIndex i(0); i < dracoMesh->num_faces(); ++i) {
                for (int j = 0; j < 3; ++j) {
                    const draco::PointIndex verticeIndex = dracoMesh->face(i)[j];
                    verticeIndices.push_back(positionAttribute->mapped_index(verticeIndex).value());
                }
            }
            
            // Normals
            
            //data.normalsByVertex = true;
            //bool indexToDirect = false;
            QVector<glm::vec3> normalValues;
            auto normalAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
            if (normalAttribute) {
                std::array<float, 3> normalValue;
                for (draco::AttributeValueIndex i(0); i < normalAttribute->size(); ++i) {
                    normalAttribute->ConvertValue<float, 3>(i, &normalValue[0]);
                    float x = normalValue[0];
                    float y = normalValue[1];
                    float z = normalValue[2];
                    normalValues.append(glm::vec3(x, y, z));
                }
            }
            
            //Getting UVs
            
            QVector<glm::vec2> uvValues;
            AttributeData attrib;
            attrib.index = 0;
            auto uvAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
            if (uvAttribute) {
                std::array<float, 3> uvValue;
                for (draco::AttributeValueIndex i(0); i < uvAttribute->size(); ++i) {
                    uvAttribute->ConvertValue<float, 3>(i, &uvValue[0]);
                    float x = uvValue[0];
                    float y = uvValue[1];
                    uvValues.append(glm::vec2(x, y));
                    attrib.texCoords.append(glm::vec2(x, y));
                }
            }
            
            data.extracted.texcoordSetMap.insert(attrib.name, data.attributes.size());
            data.attributes.push_back(attrib);

            QHash<QString, size_t>::iterator it = data.extracted.texcoordSetMap.find(attrib.name);
            if (it == data.extracted.texcoordSetMap.end()) {
                data.extracted.texcoordSetMap.insert(attrib.name, data.attributes.size());
                data.attributes.push_back(attrib);
            } else {
                // WTF same names for different UVs?
                qCDebug(modelformat) << "LayerElementUV #" << attrib.index << " is reusing the same name as #" << (*it) << ". Skip this texcoord attribute.";
            }

            
            //Getting Color
            
            //data.colorsByVertex = false;
            QVector<glm::vec4> colorValues;
            auto colorAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::COLOR);
            if (colorAttribute) {
                std::array<float, 3> colorValue;
                for (draco::AttributeValueIndex i(0); i < colorAttribute->size(); ++i) {
                    colorAttribute->ConvertValue<float, 3>(i, &colorValue[0]);
                    float x = colorValue[0];
                    float y = colorValue[1];
                    float z = colorValue[2];
                    float k = 0;
                    colorValues.append(glm::vec4(x, y, z, k));
                }
            }

            //if (indexToDirect && data.normalIndices.isEmpty()) {
            //    // hack to work around wacky Makehuman exports
            //    data.colorsByVertex = true;
            //}

            // Per Face Materials
           
           /* int32_t DRACO_ATTRIBUTE_MATERIAL_ID = 1000;
            auto materialAttribute = dracoMesh->attribute(DRACO_ATTRIBUTE_MATERIAL_ID);
            qCDebug(modelformat) << "MaterialAttribute" << materialAttribute;*/

            bool isMultiMaterial = false;
            if (isMaterialPerPolygon) {
                isMultiMaterial = true;
            }
            // TODO: make excellent use of isMultiMaterial
            Q_UNUSED(isMultiMaterial);

              int polygonIndex = 0;
              QHash<QPair<int, int>, int> materialTextureParts;
              int partIndex = 0;
              int nextIndex = 0;
              QVector<int> normalIndices = {};
              for (int beginIndex = 0; beginIndex < verticeIndices.size(); ) {
                  polygonIndex++;
                  QPair<int, int> materialTexture((polygonIndex < materials.size()) ? materials.at(polygonIndex) : 0,
                                                  (polygonIndex < textures.size()) ? textures.at(polygonIndex) : 0);

                  int& partIndex = materialTextureParts[materialTexture];
                  if (partIndex == 0) {
                      extractedMesh.partMaterialTextures.append(materialTexture);
                      extractedMesh.mesh.parts.resize(extractedMesh.mesh.parts.size() + 1);
                      partIndex = extractedMesh.mesh.parts.size();
                  }

                  FBXMeshPart& part = extractedMesh.mesh.parts[partIndex - 1];
                  for (nextIndex = beginIndex; nextIndex < beginIndex + 3; nextIndex++) {
                      
                      int vertexIndex = verticeIndices.at(nextIndex);
                      Vertex vertex;
                      vertex.originalIndex = vertexIndex;

                      //Positions

                      glm::vec3 position;
                      if (vertexIndex < positionValues.size()) {
                          position = positionValues.at(vertexIndex);
                      }

                      //Normals

                      glm::vec3 normal;
                      //int normalIndex = data.normalsByVertex ? vertexIndex : index;
                      int normalIndex = vertexIndex;
                      if (normalIndices.isEmpty()) {
                          if (normalIndex < normalValues.size()) {
                              normal = normalValues.at(normalIndex);
                              /* qCDebug(modelformat) << "Index" << normalIndex;
                              qCDebug(modelformat) << "NormalValue" << normal;*/
                          }
                      } else if (normalIndex < normalIndices.size()) {
                          normalIndex = normalIndices.at(normalIndex);
                          if (normalIndex >= 0 && normalIndex < normalValues.size()) {
                              normal = normalValues.at(normalIndex);
                          }
                      }

                      //Colors

                      glm::vec4 color;
                      bool hasColors = (colorValues.size() > 1);
                      if (hasColors) {
                          //int colorIndex = data.colorsByVertex ? vertexIndex : index;
                          int colorIndex = vertexIndex;
                          /*if (data.colorIndices.isEmpty()) {
                              if (colorIndex < data.colors.size()) {*/
                                  color = colorValues.at(colorIndex);
                              //}
                          /*} else if (colorIndex < data.colorIndices.size()) {
                              colorIndex = data.colorIndices.at(colorIndex);
                              if (colorIndex >= 0 && colorIndex < data.colors.size()) {
                                  color = data.colors.at(colorIndex);
                              }
                          }*/
                      }

                      //UVs

                      //if (data.texCoordIndices.isEmpty()) {
                          //if (nextIndex < uvValues.size()) {
                              vertex.texCoord = uvValues.at(vertexIndex);
                          //}
                      //} 
                      /*else if (index < data.texCoordIndices.size()) {
                          int texCoordIndex = data.texCoordIndices.at(index);
                          if (texCoordIndex >= 0 && texCoordIndex < data.texCoords.size()) {
                              vertex.texCoord = data.texCoords.at(texCoordIndex);
                          }
                      }*/

                      /*bool hasMoreTexcoords = (data.attributes.size() > 1);
                      if (hasMoreTexcoords) {
                      if (data.attributes[1].texCoordIndices.empty()) {
                      if (index < data.attributes[1].texCoords.size()) {
                      vertex.texCoord1 = data.attributes[1].texCoords.at(index);
                      }
                      } else if (index < data.attributes[1].texCoordIndices.size()) {
                      int texCoordIndex = data.attributes[1].texCoordIndices.at(index);
                      if (texCoordIndex >= 0 && texCoordIndex < data.attributes[1].texCoords.size()) {
                      vertex.texCoord1 = data.attributes[1].texCoords.at(texCoordIndex);
                      }
                      }
                      }*/


                      QHash<Vertex, int>::const_iterator it = data.indices.find(vertex);
                      if (it == data.indices.constEnd()) {
                          int newIndex = extractedMesh.mesh.vertices.size();
                          part.triangleIndices.append(newIndex);
                          data.indices.insert(vertex, newIndex);
                          //extractedMesh.indices.insert(vertex, newIndex);
                          extractedMesh.newIndices.insert(vertexIndex, newIndex);
                          extractedMesh.mesh.vertices.append(position);
                          extractedMesh.mesh.normals.append(normal);
                          extractedMesh.mesh.texCoords.append(vertex.texCoord);
                           if (hasColors) {
                          extractedMesh.mesh.colors.append(glm::vec3(color));
                          }
                          /*if (hasMoreTexcoords) {
                          data.extracted.mesh.texCoords1.append(vertex.texCoord1);
                          }*/
                      } else {
                          part.triangleIndices.append(*it);
                          extractedMesh.mesh.normals[*it] += normal;
                      }

                  }

                  beginIndex = nextIndex;
              }

            
        
        } else if (child.name == "Vertices") {
            data.vertices = createVec3Vector(getDoubleVector(child));

        } else if (child.name == "PolygonVertexIndex") {
            data.polygonIndices = getIntVector(child);
            
        } else if (child.name == "LayerElementNormal") {
            data.normalsByVertex = false;
            bool indexToDirect = false;
            foreach(const FBXNode& subdata, child.children) {
                if (subdata.name == "Normals") {
                    data.normals = createVec3Vector(getDoubleVector(subdata));

                } else if (subdata.name == "NormalsIndex") {
                    data.normalIndices = getIntVector(subdata);
                    

                } else if (subdata.name == "MappingInformationType" && subdata.properties.at(0) == BY_VERTICE) {
                    data.normalsByVertex = true;

                } else if (subdata.name == "ReferenceInformationType" && subdata.properties.at(0) == INDEX_TO_DIRECT) {
                    indexToDirect = true;
                }
            }
            if (indexToDirect && data.normalIndices.isEmpty()) {
                // hack to work around wacky Makehuman exports
                data.normalsByVertex = true;
            }
        } else if (child.name == "LayerElementColor") {
            data.colorsByVertex = false;
            bool indexToDirect = false;
            foreach(const FBXNode& subdata, child.children) {
                if (subdata.name == "Colors") {
                    data.colors = createVec4VectorRGBA(getDoubleVector(subdata), data.averageColor);
                } else if (subdata.name == "ColorsIndex") {
                    data.colorIndices = getIntVector(subdata);

                } else if (subdata.name == "MappingInformationType" && subdata.properties.at(0) == BY_VERTICE) {
                    data.colorsByVertex = true;

                } else if (subdata.name == "ReferenceInformationType" && subdata.properties.at(0) == INDEX_TO_DIRECT) {
                    indexToDirect = true;
                }
            }
            if (indexToDirect && data.normalIndices.isEmpty()) {
                // hack to work around wacky Makehuman exports
                data.colorsByVertex = true;
            }

#if defined(FBXREADER_KILL_BLACK_COLOR_ATTRIBUTE)
            // Potential feature where we decide to kill the color attribute is to dark?
            // Tested with the model:
            // https://hifi-public.s3.amazonaws.com/ryan/gardenLight2.fbx
            // let's check if we did have true data ?
            if (glm::all(glm::lessThanEqual(data.averageColor, glm::vec4(0.09f)))) {
                data.colors.clear();
                data.colorIndices.clear();
                data.colorsByVertex = false;
                qCDebug(modelformat) << "LayerElementColor has an average value of 0.0f... let's forget it.";
            }
#endif

        } else if (child.name == "LayerElementUV") {
            if (child.properties.at(0).toInt() == 0) {
                AttributeData attrib;
                attrib.index = child.properties.at(0).toInt();
                foreach(const FBXNode& subdata, child.children) {
                    if (subdata.name == "UV") {
                        data.texCoords = createVec2Vector(getDoubleVector(subdata));
                        attrib.texCoords = createVec2Vector(getDoubleVector(subdata));
                    } else if (subdata.name == "UVIndex") {
                        data.texCoordIndices = getIntVector(subdata);
                        attrib.texCoordIndices = getIntVector(subdata);
                    } else if (subdata.name == "Name") {
                        attrib.name = subdata.properties.at(0).toString();
                    }
#if defined(DEBUG_FBXREADER)
                    else {
                        int unknown = 0;
                        QString subname = subdata.name.data();
                        if ((subdata.name == "Version")
                            || (subdata.name == "MappingInformationType")
                            || (subdata.name == "ReferenceInformationType")) {
                        } else {
                            unknown++;
                        }
                    }
#endif
                }
                data.extracted.texcoordSetMap.insert(attrib.name, data.attributes.size());
                data.attributes.push_back(attrib);
            } else {
                AttributeData attrib;
                attrib.index = child.properties.at(0).toInt();
                foreach(const FBXNode& subdata, child.children) {
                    if (subdata.name == "UV") {
                        attrib.texCoords = createVec2Vector(getDoubleVector(subdata));
                    } else if (subdata.name == "UVIndex") {
                        attrib.texCoordIndices = getIntVector(subdata);
                    } else if (subdata.name == "Name") {
                        attrib.name = subdata.properties.at(0).toString();
                    }
#if defined(DEBUG_FBXREADER)
                    else {
                        int unknown = 0;
                        QString subname = subdata.name.data();
                        if ((subdata.name == "Version")
                            || (subdata.name == "MappingInformationType")
                            || (subdata.name == "ReferenceInformationType")) {
                        } else {
                            unknown++;
                        }
                    }
#endif
                }

                QHash<QString, size_t>::iterator it = data.extracted.texcoordSetMap.find(attrib.name);
                if (it == data.extracted.texcoordSetMap.end()) {
                    data.extracted.texcoordSetMap.insert(attrib.name, data.attributes.size());
                    data.attributes.push_back(attrib);
                } else {
                    // WTF same names for different UVs?
                    qCDebug(modelformat) << "LayerElementUV #" << attrib.index << " is reusing the same name as #" << (*it) << ". Skip this texcoord attribute.";
                }
            }
        } else if (child.name == "LayerElementMaterial") {
            static const QVariant BY_POLYGON = QByteArray("ByPolygon");
            foreach(const FBXNode& subdata, child.children) {
                if (subdata.name == "Materials") {
                    materials = getIntVector(subdata);
                    qCDebug(modelformat) << "MaterialsBC" << materials;
                } else if (subdata.name == "MappingInformationType") {
                    if (subdata.properties.at(0) == BY_POLYGON)
                        isMaterialPerPolygon = true;
                } else {
                    isMaterialPerPolygon = false;
                }
            }


        } else if (child.name == "LayerElementTexture") {
            foreach(const FBXNode& subdata, child.children) {
                if (subdata.name == "TextureId") {
                    textures = getIntVector(subdata);
                    qCDebug(modelformat) << "TexturesBC" << textures;
                }
            }
        }
    }
    //qCDebug(modelformat) << "NormalIndices" << data.normalIndices;
    bool isMultiMaterial = false;
    if (isMaterialPerPolygon) {
        isMultiMaterial = true;
    }
    // TODO: make excellent use of isMultiMaterial
    Q_UNUSED(isMultiMaterial);

    // convert the polygons to quads and triangles
    int polygonIndex = 0;
    //polygonIndex = 0;
    QHash<QPair<int, int>, int> materialTextureParts;
    for (int beginIndex = 0; beginIndex < data.polygonIndices.size(); polygonIndex++) {
        int endIndex = beginIndex;
        while (endIndex < data.polygonIndices.size() && data.polygonIndices.at(endIndex++) >= 0);

        QPair<int, int> materialTexture((polygonIndex < materials.size()) ? materials.at(polygonIndex) : 0,
            (polygonIndex < textures.size()) ? textures.at(polygonIndex) : 0);
       /* if (count == 0 && dracoNode) {

            materialTexture.first = -1631458632;
            materialTexture.second = 521;
            count++;

        } else if (count == 1 && dracoNode) {

            materialTexture.first = 0;
            materialTexture.second = 0;
        }*/
        /*qCDebug(modelformat) << "MaterialTextureFirst" << materialTexture.first;
        qCDebug(modelformat) << "MaterialTextureSecond" << materialTexture.second;*/
        int& partIndex = materialTextureParts[materialTexture];
        if (partIndex == 0) {
            data.extracted.partMaterialTextures.append(materialTexture);
            data.extracted.mesh.parts.resize(data.extracted.mesh.parts.size() + 1);
            partIndex = data.extracted.mesh.parts.size();
        }
        FBXMeshPart& part = data.extracted.mesh.parts[partIndex - 1];
        if (endIndex - beginIndex == 4) {
            appendIndex(data, part.quadIndices, beginIndex++);
            appendIndex(data, part.quadIndices, beginIndex++);
            appendIndex(data, part.quadIndices, beginIndex++);
            appendIndex(data, part.quadIndices, beginIndex++);

            int quadStartIndex = part.quadIndices.size() - 4;
            int i0 = part.quadIndices[quadStartIndex + 0];
            int i1 = part.quadIndices[quadStartIndex + 1];
            int i2 = part.quadIndices[quadStartIndex + 2];
            int i3 = part.quadIndices[quadStartIndex + 3];

            // Sam's recommended triangle slices
            // Triangle tri1 = { v0, v1, v3 };
            // Triangle tri2 = { v1, v2, v3 };
            // NOTE: Random guy on the internet's recommended triangle slices
            // Triangle tri1 = { v0, v1, v2 };
            // Triangle tri2 = { v2, v3, v0 };

            part.quadTrianglesIndices.append(i0);
            part.quadTrianglesIndices.append(i1);
            part.quadTrianglesIndices.append(i3);

            part.quadTrianglesIndices.append(i1);
            part.quadTrianglesIndices.append(i2);
            part.quadTrianglesIndices.append(i3);

        } else {
            for (int nextIndex = beginIndex + 1;; ) {
                appendIndex(data, part.triangleIndices, beginIndex);
                appendIndex(data, part.triangleIndices, nextIndex++);
                appendIndex(data, part.triangleIndices, nextIndex);
                if (nextIndex >= data.polygonIndices.size() || data.polygonIndices.at(nextIndex) < 0) {
                    break;
                }
            }
            beginIndex = endIndex;
        }
    }
    
    return data.extracted;
}

//ExtractedMesh  extractDracoMesh(draco::Mesh* dracoMesh, unsigned int& meshIndex) {
//
//    //MeshData data;
//    ExtractedMesh extractedMesh;
//    //data.extracted.mesh.meshIndex = meshIndex++;
//    extractedMesh.mesh.meshIndex = meshIndex++;
//
//    QVector<int> materials = { 0 };
//    QVector<int> textures;
//    bool isMaterialPerPolygon = false;
//    static const QVariant BY_VERTICE = QByteArray("ByVertice");
//    static const QVariant INDEX_TO_DIRECT = QByteArray("IndexToDirect");
//
//    // Positions
//
//    auto positionAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
//    QVector<glm::vec3> positionValues;
//    if (positionAttribute) {
//
//        std::array<float, 3> positionValue;
//        
//        for (draco::AttributeValueIndex i(0); i < positionAttribute->size(); ++i) {
//            positionAttribute->ConvertValue<float, 3>(i, &positionValue[0]);
//            float x = positionValue[0];
//            float y = positionValue[1];
//            float z = positionValue[2];
//            positionValues.append(glm::vec3(x, y, z));
//        }
//
//        //data.vertices = positionValues;
//        
//    }
//
//    // Polygon Vertex Indices
//
//    QVector<int> verticeIndices;
//
//    for (draco::FaceIndex i(0); i < dracoMesh->num_faces(); ++i) {
//
//        for (int j = 0; j < 3; ++j) {
//            const draco::PointIndex verticeIndex = dracoMesh->face(i)[j];
//            verticeIndices.push_back(positionAttribute->mapped_index(verticeIndex).value());
//        }
//
//    }
//
//   /* int k = 0;
//    for (int i = 0;i < verticeIndices.size();i++) {
//        if (k == 2) {
//
//            int m = verticeIndices[i];
//            verticeIndices[i] = -m - 1;
//            k = 0;
//        }
//        k++;
//    }*/
//    //QVector<int> verticeIndices = { 1, 3, -1, 7, 5, -5, 4, 1, -1, 5, 2, -2, 2, 7, -4, 0, 7, -5, 1, 2, -4, 7, 6, -6, 4, 5, -2, 5, 6, -3, 2, 6, -8, 0, 3, -8 };
//
//    //data.polygonIndices = verticeIndices;
//    //qCDebug(modelformat) << "Faces" << data.polygonIndices;
//
//
//
//
//
//    // Normals
//
//    //data.normalsByVertex = true;
//    //bool indexToDirect = false;
//    
//    QVector<glm::vec3> normalValues;
//    auto normalAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
//    if (normalAttribute) {
//
//        std::array<float, 3> normalValue;
//
//        for (draco::AttributeValueIndex i(0); i < normalAttribute->size(); ++i) {
//            normalAttribute->ConvertValue<float, 3>(i, &normalValue[0]);
//            float x = normalValue[0];
//            float y = normalValue[1];
//            float z = normalValue[2];
//            normalValues.append(glm::vec3(x, y, z));
//        }
//
//        //data.normals = normalValues;
//       
//    }
//    /* QVector<glm::vec3> x = { (glm::vec3)(2.98023e-08, -1, 0), (glm::vec3)(2.98023e-08, -1, 0), (glm::vec3)(2.98023e-08, -1, 0), (glm::vec3)(0, 1, 0),(glm::vec3) (0, 1, 0), (glm::vec3)(0, 1, 0),(glm::vec3) (1, -2.38419e-07, 0), (glm::vec3)(1, -2.38419e-07, 0),(glm::vec3) (1, -2.38419e-07, 0), (glm::vec3)(-8.9407e-08, -4.76837e-07, 1),(glm::vec3) (-8.9407e-08, -4.76837e-07, 1),(glm::vec3) (-8.9407e-08, -4.76837e-07, 1), (glm::vec3)(-1, -1.49012e-07, -2.38419e-07), (glm::vec3)(-1, -1.49012e-07, -2.38419e-07), (glm::vec3)(-1, -1.49012e-07, -2.38419e-07),(glm::vec3) (2.68221e-07, 2.38419e-07, -1), (glm::vec3)(2.68221e-07, 2.38419e-07, -1),(glm::vec3) (2.68221e-07, 2.38419e-07, -1),(glm::vec3) (0, -1, 0), (glm::vec3)(0, -1, 0), (glm::vec3)(0, -1, 0), (glm::vec3)(5.96047e-08, 1, 0), (glm::vec3)(5.96047e-08, 1, 0), (glm::vec3)(5.96047e-08, 1, 0), (glm::vec3)(1, 3.27825e-07, 5.96046e-07), (glm::vec3)(1, 3.27825e-07, 5.96046e-07), (glm::vec3)(1, 3.27825e-07, 5.96046e-07), (glm::vec3)(-4.76837e-07, 1.19209e-07, 1), (glm::vec3)(-4.76837e-07, 1.19209e-07, 1), (glm::vec3)(-4.76837e-07, 1.19209e-07, 1), (glm::vec3)(-1, -1.19209e-07, -2.38419e-07),(glm::vec3) (-1, -1.19209e-07, -2.38419e-07), (glm::vec3)(-1, -1.19209e-07, -2.38419e-07), (glm::vec3)(2.08616e-07, 8.9407e-08, -1), (glm::vec3)(2.08616e-07, 8.9407e-08, -1), (glm::vec3)(2.08616e-07, 8.9407e-08, -1)
//    };*/
//     
//     //normalValues = x;
//
//    //if (indexToDirect && data.normalIndices.isEmpty()) {
//    //    // hack to work around wacky Makehuman exports
//    //    data.normalsByVertex = true;
//    //}
//
//    //Getting UVs
//    QVector<glm::vec2> uvValues;
//    AttributeData attrib;
//    attrib.index = 0;
//    auto uvAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
//    if (uvAttribute) {
//
//        std::array<float, 3> uvValue;
//        
//        for (draco::AttributeValueIndex i(0); i < uvAttribute->size(); ++i) {
//            uvAttribute->ConvertValue<float, 3>(i, &uvValue[0]);
//            float x = uvValue[0];
//            float y = uvValue[1];
//            uvValues.append(glm::vec2(x, y));
//            attrib.texCoords.append(glm::vec2(x, y));
//        }
//
//        //data.texCoords = uvValues;
//
//    }
//   /* extractedMesh.texcoordSetMap.insert(attrib.name, data.attributes.size());
//    data.attributes.push_back(attrib);*/
//    //Getting Color
//    //data.colorsByVertex = false;
//    QVector<glm::vec4> colorValues;
//    auto colorAttribute = dracoMesh->GetNamedAttribute(draco::GeometryAttribute::COLOR);
//    if (colorAttribute) {
//
//        std::array<float, 3> colorValue;
//        
//        for (draco::AttributeValueIndex i(0); i < colorAttribute->size(); ++i) {
//            colorAttribute->ConvertValue<float, 3>(i, &colorValue[0]);
//            float x = colorValue[0];
//            float y = colorValue[1];
//            float z = colorValue[2];
//            float k = 0;
//            colorValues.append(glm::vec4(x, y, z, k));
//        }
//
//        //data.colors = colorValues;
//
//    }
//
//    //if (indexToDirect && data.normalIndices.isEmpty()) {
//    //    // hack to work around wacky Makehuman exports
//    //    data.colorsByVertex = true;
//    //}
//
//    //bool isMultiMaterial = false;
//    //if (isMaterialPerPolygon) {
//    //    isMultiMaterial = true;
//    //}
//    //// TODO: make excellent use of isMultiMaterial
//    //Q_UNUSED(isMultiMaterial);
//
//    int polygonIndex = 0;
//    QHash<QPair<int, int>, int> materialTextureParts;
//    int partIndex = 0;
//    int nextIndex = 0;
//    QVector<int> normalIndices = {};
//    for (int beginIndex = 0; beginIndex < verticeIndices.size(); ){//beginIndex++) {
//        
//       /* int endIndex = beginIndex;
//        while (endIndex < verticeIndices.size() && verticeIndices.at(endIndex++) >= 0);
//        endIndex++;*/
//        
//        polygonIndex++;
//
//        QPair<int, int> materialTexture((polygonIndex < materials.size()) ? materials.at(polygonIndex) : 0,
//            (polygonIndex < textures.size()) ? textures.at(polygonIndex) : 0);
//        
//        int& partIndex = materialTextureParts[materialTexture];
//        
//        if (partIndex == 0) {
//            extractedMesh.partMaterialTextures.append(materialTexture);
//            extractedMesh.mesh.parts.resize(extractedMesh.mesh.parts.size() + 1);
//            partIndex = extractedMesh.mesh.parts.size();
//        }
//
//        FBXMeshPart& part = extractedMesh.mesh.parts[partIndex - 1];
//
//            //for (int nextIndex = beginIndex + 1;; ) {
//            //    appendDracoIndex(&extractedMesh,part.triangleIndices, beginIndex,verticeIndices,positionValues, &normalValues);
//            //    appendDracoIndex(&extractedMesh,part.triangleIndices, nextIndex++, verticeIndices, positionValues, &normalValues);
//            //    appendDracoIndex(&extractedMesh,part.triangleIndices, nextIndex, verticeIndices, positionValues, &normalValues);             
//            //    /*if (nextIndex >= verticeIndices.size() || (nextIndex + 1) %3 == 0) {
//            //        break;
//            //    }*/
//            //    if (nextIndex >= verticeIndices.size() || verticeIndices.at(nextIndex) < 0) {
//            //        break;
//            //    }
//            //}
//            //beginIndex = endIndex;
//            //    
//            //}
//
//        for (nextIndex = beginIndex; nextIndex < beginIndex + 3; nextIndex++) {
//            
//            int vertexIndex = verticeIndices.at(nextIndex);
//            Vertex vertex;
//            vertex.originalIndex = vertexIndex;
//
//            //Positions
//
//            glm::vec3 position;
//            if (vertexIndex < positionValues.size()) {
//                position = positionValues.at(vertexIndex);
//            }
//
//            //Normals
//
//            glm::vec3 normal;
//            //int normalIndex = data.normalsByVertex ? vertexIndex : index;
//            int normalIndex = nextIndex;
//            if (normalIndices.isEmpty()) {
//                if (normalIndex < normalValues.size()) {
//                    normal = normalValues.at(normalIndex);
//                   /* qCDebug(modelformat) << "Index" << normalIndex;
//                    qCDebug(modelformat) << "NormalValue" << normal;*/
//                }
//            } else if (normalIndex < normalIndices.size()) {
//                normalIndex = normalIndices.at(normalIndex);
//                if (normalIndex >= 0 && normalIndex < normalValues.size()) {
//                    normal = normalValues.at(normalIndex);
//                }
//            }
//
//            //Colors
//
//            glm::vec4 color;
//            bool hasColors = (colorValues.size() > 1);
//            if (hasColors) {
//                //int colorIndex = data.colorsByVertex ? vertexIndex : index;
//                int colorIndex = vertexIndex;
//                /*if (data.colorIndices.isEmpty()) {
//                    if (colorIndex < data.colors.size()) {*/
//                        color = colorValues.at(colorIndex);
//                    //}
//                /*} else if (colorIndex < data.colorIndices.size()) {
//                    colorIndex = data.colorIndices.at(colorIndex);
//                    if (colorIndex >= 0 && colorIndex < data.colors.size()) {
//                        color = data.colors.at(colorIndex);
//                    }
//                }*/
//            }
//
//            //UVs
//            vertex.texCoord = uvValues.at(vertexIndex);
//
//            //if (uvValues.isEmpty()) {
//            //    if (nextIndex < uvValues.size()) {
//            //        vertex.texCoord = uvValues.at(nextIndex);
//            //    }
//            //}
//            /* else if (index < data.texCoordIndices.size()) {
//                int texCoordIndex = data.texCoordIndices.at(index);
//                if (texCoordIndex >= 0 && texCoordIndex < data.texCoords.size()) {
//                    vertex.texCoord = data.texCoords.at(texCoordIndex);
//                }
//            }*/
//
//            /*bool hasMoreTexcoords = (data.attributes.size() > 1);
//            if (hasMoreTexcoords) {
//                if (data.attributes[1].texCoordIndices.empty()) {
//                    if (index < data.attributes[1].texCoords.size()) {
//                        vertex.texCoord1 = data.attributes[1].texCoords.at(index);
//                    }
//                } else if (index < data.attributes[1].texCoordIndices.size()) {
//                    int texCoordIndex = data.attributes[1].texCoordIndices.at(index);
//                    if (texCoordIndex >= 0 && texCoordIndex < data.attributes[1].texCoords.size()) {
//                        vertex.texCoord1 = data.attributes[1].texCoords.at(texCoordIndex);
//                    }
//                }
//            }*/
//
//
//            QHash<Vertex, int>::const_iterator it = extractedMesh.indices.find(vertex);
//            if (it == extractedMesh.indices.constEnd()) {
//                int newIndex = extractedMesh.mesh.vertices.size();
//                part.triangleIndices.append(newIndex);
//                //data.indices.insert(vertex, newIndex);
//                extractedMesh.indices.insert(vertex, newIndex);
//                extractedMesh.newIndices.insert(vertexIndex, newIndex);
//                extractedMesh.mesh.vertices.append(position);
//                extractedMesh.mesh.normals.append(normal);
//                extractedMesh.mesh.texCoords.append(vertex.texCoord);
//                if (hasColors) {
//                extractedMesh.mesh.colors.append(glm::vec3(color));
//                }
//                /*if (hasMoreTexcoords) {
//                data.extracted.mesh.texCoords1.append(vertex.texCoord1);
//                }*/
//            } else {
//                part.triangleIndices.append(*it);
//                extractedMesh.mesh.normals[*it] += normal;
//            }
//        
//        }
//
//        beginIndex = nextIndex;
//    }
//
//
//            
//
//
//                /*if (nextIndex >= verticeIndices.size() || (nextIndex + 1) % 3 == 0) {
//                    break;
//                }*/
//    //qCDebug(modelformat) << "Normals" << extractedMesh.mesh.normals;
//    /*QVector<glm::vec3> x ={ (glm::vec3)(2.98023e-08, -2, 0), (glm::vec3)(2.98023e-08, -2, 0), (glm::vec3)(2.98023e-08, -1, 0), (glm::vec3)(5.96047e-08, 2, 0), (glm::vec3)(5.96047e-08, 2, 0), (glm::vec3)(2.68221e-07, 1, -1), (glm::vec3)(2, 8.94068e-08, 5.96046e-07), (glm::vec3)(2, 8.94068e-08, 5.96046e-07), (glm::vec3)(1, -2.38419e-07, 0), (glm::vec3)(-5.66244e-07, -3.57628e-07, 2), (glm::vec3)(-5.66244e-07, -3.57628e-07, 2),(glm::vec3) (-8.9407e-08, -4.76837e-07, 1), glm::vec3(-2, -2.68221e-07, -4.76837e-07), (glm::vec3)(-2, -2.68221e-07, -4.76837e-07), (glm::vec3)(-1, -1.49012e-07, -2.38419e-07), (glm::vec3)(4.76837e-07, 3.27826e-07, -2), (glm::vec3)(4.76837e-07, 3.27826e-07, -2), (glm::vec3)(0, -1, 0), (glm::vec3)(5.96047e-08, 1, 0), (glm::vec3)(1, 3.27825e-07, 5.96046e-07),(glm::vec3) (-4.76837e-07, 1.19209e-07, 1), (glm::vec3)(-1, -1.19209e-07, -2.38419e-07),(glm::vec3)  (2.08616e-07, 8.9407e-08, -1) };
//    extractedMesh.mesh.normals = x;*/
//    return extractedMesh;
//}


void FBXReader::buildModelMesh(FBXMesh& extractedMesh, const QString& url) {
    static QString repeatedMessage = LogHandler::getInstance().addRepeatedMessageRegex("buildModelMesh failed -- .*");

    unsigned int totalSourceIndices = 0;
    foreach(const FBXMeshPart& part, extractedMesh.parts) {
        totalSourceIndices += (part.quadTrianglesIndices.size() + part.triangleIndices.size());
    }

    if (!totalSourceIndices) {
        qCDebug(modelformat) << "buildModelMesh failed -- no indices, url = " << url;
        return;
    }

    if (extractedMesh.vertices.size() == 0) {
        qCDebug(modelformat) << "buildModelMesh failed -- no vertices, url = " << url;
        return;
    }

    FBXMesh& fbxMesh = extractedMesh;
    model::MeshPointer mesh(new model::Mesh());

    // Grab the vertices in a buffer
    auto vb = std::make_shared<gpu::Buffer>();
    vb->setData(extractedMesh.vertices.size() * sizeof(glm::vec3),
        (const gpu::Byte*) extractedMesh.vertices.data());
    gpu::BufferView vbv(vb, gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::XYZ));
    mesh->setVertexBuffer(vbv);

    // evaluate all attribute channels sizes
    int normalsSize = fbxMesh.normals.size() * sizeof(glm::vec3);
    int tangentsSize = fbxMesh.tangents.size() * sizeof(glm::vec3);
    int colorsSize = fbxMesh.colors.size() * sizeof(glm::vec3);
    int texCoordsSize = fbxMesh.texCoords.size() * sizeof(glm::vec2);
    int texCoords1Size = fbxMesh.texCoords1.size() * sizeof(glm::vec2);

    int clusterIndicesSize = fbxMesh.clusterIndices.size() * sizeof(uint8_t);
    if (fbxMesh.clusters.size() > UINT8_MAX) {
        // we need 16 bits instead of just 8 for clusterIndices
        clusterIndicesSize *= 2;
    }
    int clusterWeightsSize = fbxMesh.clusterWeights.size() * sizeof(uint8_t);

    int normalsOffset = 0;
    int tangentsOffset = normalsOffset + normalsSize;
    int colorsOffset = tangentsOffset + tangentsSize;
    int texCoordsOffset = colorsOffset + colorsSize;
    int texCoords1Offset = texCoordsOffset + texCoordsSize;
    int clusterIndicesOffset = texCoords1Offset + texCoords1Size;
    int clusterWeightsOffset = clusterIndicesOffset + clusterIndicesSize;
    int totalAttributeSize = clusterWeightsOffset + clusterWeightsSize;

    // Copy all attribute data in a single attribute buffer
    auto attribBuffer = std::make_shared<gpu::Buffer>();
    attribBuffer->resize(totalAttributeSize);
    attribBuffer->setSubData(normalsOffset, normalsSize, (gpu::Byte*) fbxMesh.normals.constData());
    attribBuffer->setSubData(tangentsOffset, tangentsSize, (gpu::Byte*) fbxMesh.tangents.constData());
    attribBuffer->setSubData(colorsOffset, colorsSize, (gpu::Byte*) fbxMesh.colors.constData());
    attribBuffer->setSubData(texCoordsOffset, texCoordsSize, (gpu::Byte*) fbxMesh.texCoords.constData());
    attribBuffer->setSubData(texCoords1Offset, texCoords1Size, (gpu::Byte*) fbxMesh.texCoords1.constData());

    if (fbxMesh.clusters.size() < UINT8_MAX) {
        // yay! we can fit the clusterIndices within 8-bits
        int32_t numIndices = fbxMesh.clusterIndices.size();
        QVector<uint8_t> clusterIndices;
        clusterIndices.resize(numIndices);
        for (int32_t i = 0; i < numIndices; ++i) {
            assert(fbxMesh.clusterIndices[i] <= UINT8_MAX);
            clusterIndices[i] = (uint8_t)(fbxMesh.clusterIndices[i]);
        }
        attribBuffer->setSubData(clusterIndicesOffset, clusterIndicesSize, (gpu::Byte*) clusterIndices.constData());
    } else {
        attribBuffer->setSubData(clusterIndicesOffset, clusterIndicesSize, (gpu::Byte*) fbxMesh.clusterIndices.constData());
    }
    attribBuffer->setSubData(clusterWeightsOffset, clusterWeightsSize, (gpu::Byte*) fbxMesh.clusterWeights.constData());

    if (normalsSize) {
        mesh->addAttribute(gpu::Stream::NORMAL,
                           model::BufferView(attribBuffer, normalsOffset, normalsSize,
                           gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::XYZ)));
    }
    if (tangentsSize) {
        mesh->addAttribute(gpu::Stream::TANGENT,
                           model::BufferView(attribBuffer, tangentsOffset, tangentsSize,
                           gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::XYZ)));
    }
    if (colorsSize) {
        mesh->addAttribute(gpu::Stream::COLOR,
                           model::BufferView(attribBuffer, colorsOffset, colorsSize,
                           gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::RGB)));
    }
    if (texCoordsSize) {
        mesh->addAttribute(gpu::Stream::TEXCOORD,
                           model::BufferView(attribBuffer, texCoordsOffset, texCoordsSize,
                           gpu::Element(gpu::VEC2, gpu::FLOAT, gpu::UV)));
    }
    if (texCoords1Size) {
        mesh->addAttribute(gpu::Stream::TEXCOORD1,
                           model::BufferView(attribBuffer, texCoords1Offset, texCoords1Size,
                           gpu::Element(gpu::VEC2, gpu::FLOAT, gpu::UV)));
    } else if (texCoordsSize) {
        mesh->addAttribute(gpu::Stream::TEXCOORD1,
                           model::BufferView(attribBuffer, texCoordsOffset, texCoordsSize,
                           gpu::Element(gpu::VEC2, gpu::FLOAT, gpu::UV)));
    }

    if (clusterIndicesSize) {
        if (fbxMesh.clusters.size() < UINT8_MAX) {
            mesh->addAttribute(gpu::Stream::SKIN_CLUSTER_INDEX,
                               model::BufferView(attribBuffer, clusterIndicesOffset, clusterIndicesSize,
                               gpu::Element(gpu::VEC4, gpu::UINT8, gpu::XYZW)));
        } else {
            mesh->addAttribute(gpu::Stream::SKIN_CLUSTER_INDEX,
                               model::BufferView(attribBuffer, clusterIndicesOffset, clusterIndicesSize,
                               gpu::Element(gpu::VEC4, gpu::UINT16, gpu::XYZW)));
        }
    }
    if (clusterWeightsSize) {
        mesh->addAttribute(gpu::Stream::SKIN_CLUSTER_WEIGHT,
                           model::BufferView(attribBuffer, clusterWeightsOffset, clusterWeightsSize,
                           gpu::Element(gpu::VEC4, gpu::NUINT8, gpu::XYZW)));
    }


    unsigned int totalIndices = 0;
    foreach(const FBXMeshPart& part, extractedMesh.parts) {
        totalIndices += (part.quadTrianglesIndices.size() + part.triangleIndices.size());
    }

    if (!totalIndices) {
        qCDebug(modelformat) << "buildModelMesh failed -- no indices, url = " << url;
        return;
    }

    auto indexBuffer = std::make_shared<gpu::Buffer>();
    indexBuffer->resize(totalIndices * sizeof(int));

    int indexNum = 0;
    int offset = 0;

    std::vector< model::Mesh::Part > parts;
    if (extractedMesh.parts.size() > 1) {
        indexNum = 0;
    }
    foreach(const FBXMeshPart& part, extractedMesh.parts) {
        model::Mesh::Part modelPart(indexNum, 0, 0, model::Mesh::TRIANGLES);

        if (part.quadTrianglesIndices.size()) {
            indexBuffer->setSubData(offset,
                                    part.quadTrianglesIndices.size() * sizeof(int),
                                    (gpu::Byte*) part.quadTrianglesIndices.constData());
            offset += part.quadTrianglesIndices.size() * sizeof(int);
            indexNum += part.quadTrianglesIndices.size();
            modelPart._numIndices += part.quadTrianglesIndices.size();
        }

        if (part.triangleIndices.size()) {
            indexBuffer->setSubData(offset,
                                    part.triangleIndices.size() * sizeof(int),
                                    (gpu::Byte*) part.triangleIndices.constData());
            offset += part.triangleIndices.size() * sizeof(int);
            indexNum += part.triangleIndices.size();
            modelPart._numIndices += part.triangleIndices.size();
        }

        parts.push_back(modelPart);
    }

    gpu::BufferView indexBufferView(indexBuffer, gpu::Element(gpu::SCALAR, gpu::UINT32, gpu::XYZ));
    mesh->setIndexBuffer(indexBufferView);

    if (parts.size()) {
        auto pb = std::make_shared<gpu::Buffer>();
        pb->setData(parts.size() * sizeof(model::Mesh::Part), (const gpu::Byte*) parts.data());
        gpu::BufferView pbv(pb, gpu::Element(gpu::VEC4, gpu::UINT32, gpu::XYZW));
        mesh->setPartBuffer(pbv);
    } else {
        qCDebug(modelformat) << "buildModelMesh failed -- no parts, url = " << url;
        return;
    }

    // model::Box box =
    mesh->evalPartBound(0);

    extractedMesh._mesh = mesh;
}
