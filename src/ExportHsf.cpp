#include "ExportHsf.h"
#include <string>
#include "LinAlgOps.h"
#include "debugapi.h"

#include "HStream.h"
#include "stream_common/BStream.h"

ExportHsf::ExportHsf(const char* path) : m_modelKey(INVALID_KEY), m_savePath(path)
{
	HBaseModel* baseModel = new HBaseModel();
	HC_Open_Segment_By_Key(baseModel->GetModelKey());
	{
		m_modelKey = HC_Open_Segment("");
		{
			HC_Set_Heuristics("static model=on");
			HC_Set_Visibility("geometry=on");
		}HC_Close_Segment();
	}HC_Close_Segment();
}
ExportHsf::~ExportHsf()
{
}
void ExportHsf::beginFile(Node* group)
{
	HC_Open_Segment_By_Key(m_modelKey);


}

void ExportHsf::endFile()
{
    HTK_Write_Stream_File(m_savePath.c_str(), TK_Full_Resolution);
	HC_Close_Segment();
}

void ExportHsf::beginModel(Node* group)
{
	HC_Open_Segment("");
	std::string pjtName = "project=" + std::string(group->model.project);
	std::string modelName = "name=" + std::string(group->model.name);

	HC_Set_User_Options(pjtName.c_str());
	HC_Set_User_Options(modelName.c_str());
}

void ExportHsf::endModel()
{
	HC_Close_Segment();
}

void ExportHsf::beginGroup(Node* group)
{
	HC_Open_Segment("");

}

void ExportHsf::EndGroup()
{
	HC_Close_Segment();
}

void ExportHsf::attribute(const char* key, const char* val)
{
	std::string keyString = key;
	std::string valueString = val;
	std::string setString = keyString + "=" + valueString;
	HC_Set_User_Options(setString.c_str());
}

void ExportHsf::beginAttributes(Node* container)
{
	//Attribute* beginRef = container->attributes.first;
	//Attribute* endRef = container->attributes.last;
	////std::string setString = "";
	//while (beginRef != endRef) {
	//	std::string keyString;
	//	std::string valueString;
	//	keyString = beginRef->key;
	//	valueString = beginRef->val;
	//	std::string setString = keyString + "=" + valueString;
	//	HC_Set_User_Options(setString.c_str());
	//	beginRef = beginRef->next;
	//}
}

void ExportHsf::geometry(Geometry* geometry)
{
    float scale = 1.f;

    // color
    uint32_t colorId = (geometry->color << 8) | geometry->transparency;
    if (!m_definedColors.get((uint64_t(colorId) << 1) | 1)) {
        m_definedColors.insert(((uint64_t(colorId) << 1) | 1), 1);

        double r = (1.0 / 255.0) * ((geometry->color >> 16) & 0xFF);
        double g = (1.0 / 255.0) * ((geometry->color >> 8) & 0xFF);
        double b = (1.0 / 255.0) * ((geometry->color) & 0xFF);

        char chTransmission[MVO_SMALL_BUFFER_SIZE];
        sprintf(chTransmission, "faces = (transmission = (R=%f G=%f B=%f))",
            r,
            g,
            b);

        HC_Set_Color(chTransmission);
    }

    switch (geometry->kind)
    {
 //   case Geometry::Kind::Pyramid:
 //       break;
 //   case Geometry::Kind::Box:
 //       break;
 //   case Geometry::Kind::RectangularTorus:
 //       break;
 //   case Geometry::Kind::CircularTorus:
 //       break;
 //   case Geometry::Kind::EllipticalDish:
 //       break;
 //   case Geometry::Kind::SphericalDish:
 //       break;
	//case Geometry::Kind::Snout:
 //       break;
	//case Geometry::Kind::Cylinder:
 //       break;
	//case Geometry::Kind::Sphere:
 //       break;
    //case Geometry::Kind::FacetGroup:
    //    break;
	case Geometry::Kind::Line:
        auto a = scale * mul(geometry->M_3x4, makeVec3f(geometry->line.a, 0, 0));
        auto b = scale * mul(geometry->M_3x4, makeVec3f(geometry->line.b, 0, 0));
		HC_Insert_Line(a.x, a.y, a.z, b.x, b.y, b.z);  
        break;
	default:
	{
        float scale = 1.f;
        std::vector<HPoint> localVectorPoints;
        std::vector<HPoint> localNormalPoints;
        std::vector<int> localIndices;
        if (geometry->kind == Geometry::Kind::Line) {
            auto a = scale * mul(geometry->M_3x4, makeVec3f(geometry->line.a, 0, 0));
            auto b = scale * mul(geometry->M_3x4, makeVec3f(geometry->line.b, 0, 0));

            off_v += 2;
        }
        else {
            assert(geometry->triangulation);
            auto* tri = geometry->triangulation;

            if (tri->indices != 0) {
                //fprintf(out, "g\n");
                if (geometry->triangulation->error != 0.f) {
                    OutputDebugStringA(("Triangulation Error " + std::to_string(geometry->triangulation->error) + "\n").c_str());
                }
                for (size_t i = 0; i < 3 * tri->vertices_n; i += 3) {

                    auto p = scale * mul(geometry->M_3x4, makeVec3f(tri->vertices + i));
                    Vec3f n = normalize(mul(makeMat3f(geometry->M_3x4.data), makeVec3f(tri->normals + i)));
                    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) {
                        n = makeVec3f(1.f, 0.f, 0.f);
                    }
                    HPoint cPoint = HPoint(p.x, p.y, p.z);
                    localVectorPoints.push_back(cPoint);
                    HPoint cNormal = HPoint(n.x, n.y, n.z);
                    localNormalPoints.push_back(cNormal);
                }
                if (tri->texCoords) {
                    /*for (size_t i = 0; i < tri->vertices_n; i++) {
                        const Vec2f vt = makeVec2f(tri->texCoords + 2 * i);
                        fprintf(out, "vt %f %f\n", vt.x, vt.y);
                    }*/
                }  //texture not required.
                else {
                    /*for (size_t i = 0; i < tri->vertices_n; i++) {
                        auto p = scale * mul(geometry->M_3x4, makeVec3f(tri->vertices + 3 * i));
                        fprintf(out, "vt %f %f\n", 0 * p.x, 0 * p.y);
                    }*/

                    for (size_t i = 0; i < 3 * tri->triangles_n; i += 3) {
                        auto a = tri->indices[i + 0];
                        auto b = tri->indices[i + 1];
                        auto c = tri->indices[i + 2];
                        localIndices.push_back(3);
                        localIndices.push_back(a);
                        localIndices.push_back(b);
                        localIndices.push_back(c);

                        /*std::string debugString;
                        debugString = "indices 1: " + std::to_string(a + off_v) + "/" + std::to_string(a + off_t) + "/" + std::to_string(a + off_n);
                        debugString += "indices 2: " + std::to_string(b + off_v) + "/" + std::to_string(b + off_t) + "/" + std::to_string(b + off_n);
                        debugString += "indices 3: " + std::to_string(c + off_v) + "/" + std::to_string(c + off_t) + "/" + std::to_string(c + off_n);
                        OutputDebugStringA(debugString.c_str());*/
                    }
                }

                off_v += tri->vertices_n;
                off_n += tri->vertices_n;
                off_t += tri->vertices_n;
            }
        }

        int* faces = new int[localIndices.size()];
        HPoint* points = new HPoint[localVectorPoints.size()];
        for (size_t i = 0; i < localVectorPoints.size(); ++i) {
            points[i] = localVectorPoints[i];
        }
        for (size_t i = 0; i < localIndices.size(); ++i) {
            faces[i] = localIndices[i];
        }
        int verticesCount = static_cast<int>(localVectorPoints.size());
        int indiceCount = static_cast<int>(localIndices.size());
        HC_KEY shell = HC_Insert_Shell(verticesCount, points, indiceCount, faces);
        //HC_Close_Segment();
        delete[]faces;
        delete[]points;
	
	}
        break;

    }

    


}
