#include "pch.h"
#include "ZoneExporter.h"

#include <ostream>
#include <iostream>
#include <msclr/marshal_cppstd.h>

#include "Util.h"

bool ZoneExporter::export_zone(System::String^ game_path, System::String^ zone_path, System::String^ output_path)
{
    if (manager || scene)
        uninit();

    this->zone_path = Util::get_std_str(zone_path);
    this->out_folder = Util::get_std_str(output_path);
    this->zone_code = Util::get_zone_code(zone_path);
    this->material_cache = new std::unordered_map<unsigned long long, FbxSurfacePhong*>();
    this->mesh_cache = new std::unordered_map<std::string, FbxMesh*>();

    std::printf("Initializing...\n");
    auto init_result = init(game_path);
    if (!init_result)
    {
        std::printf("Error occurred during ZoneExporter initialization.\n");
        return false;
    }

    std::printf("Processing models and textures...\n");
    std::printf("Processing zone terrain.\n");
    if (!process_terrain())
    {
        std::printf("Failed to process zone terrain.\n");
        return false;
    }

    std::printf("Processing bg.lgb...\n");
    if (!process_bg())
    {
        std::printf("Failed to process zone bg.\n");
        return false;
    }

    std::printf("Saving scene...\n");
    if (!save_scene())
    {
        std::printf("Failed to save scene.\n");
        return false;
    }

    std::printf("Saved scene...\n");

    return true;
}

bool ZoneExporter::process_terrain()
{
    auto terrain_path = zone_path.substr(0, zone_path.rfind("/level"));
    terrain_path = "bg/" + terrain_path + "/bgplate/";
    auto terafile_path = terrain_path + "terrain.tera";

    if (!data->FileExists(Util::get_str_handle(terafile_path)))
        return true;
    
    auto terafile = data->GetFile<Lumina::Data::Files::TeraFile^>(Util::get_str_handle(terafile_path));

    if (terafile == nullptr)
        return false;

    FbxNode* terrain_node = FbxNode::Create(manager, "terrain");

    for (int i = 0; i < terafile->PlateCount; i++)
    {
        FbxNode* plate_node = FbxNode::Create(manager, (std::string("bgplate_") + std::to_string(i)).c_str());
        auto pos = terafile->GetPlatePosition(i);
        plate_node->LclTranslation.Set(FbxVectorTemplate3<double>(pos.X, 0, pos.Y));
        auto plate_model = gcnew Lumina::Models::Models::Model(data,
                                                               Util::get_str_handle(terrain_path) + System::String::Format("{0:D4}.mdl", i),
                                                               Lumina::Models::Models::Model::ModelLod::High,
                                                               1);
        process_model(plate_model, &plate_node);
        
        terrain_node->AddChild(plate_node);
    }

    scene->GetRootNode()->AddChild(terrain_node);
    return true;
}

bool ZoneExporter::process_bg()
{
	System::Collections::Generic::List<System::String^>^ fnames =
        gcnew System::Collections::Generic::List<System::String^>();
	fnames->Add("/bg.lgb");
    fnames->Add("/planmap.lgb");
    fnames->Add("/planevent.lgb");

    for each(System::String^ s in fnames) {
		auto bg_path = "bg/" + Util::get_str_handle(zone_path.substr(0, zone_path.length() - 5)) + s;
		auto bg = data->GetFile<Lumina::Data::Files::LgbFile^>(bg_path);

        if (bg == nullptr)
            return false;

        for (int i = 0; i < bg->Layers->Length; i++)
        {
            auto layer = % bg->Layers[i];
            auto layer_node = FbxNode::Create(scene, Util::get_std_str(layer->Name).c_str());

            for (int j = 0; j < layer->InstanceObjects->Length; j++)
            {
                auto object = % layer->InstanceObjects[j];
                auto object_node = FbxNode::Create(scene, Util::get_std_str(object->Name).c_str());

                object_node->LclTranslation.Set(FbxDouble3(object->Transform.Translation.X, object->Transform.Translation.Y, object->Transform.Translation.Z));
                object_node->LclRotation.Set(FbxDouble3(Util::degrees(object->Transform.Rotation.X),
                    Util::degrees(object->Transform.Rotation.Y),
                    Util::degrees(object->Transform.Rotation.Z)));
                object_node->LclScaling.Set(FbxDouble3(object->Transform.Scale.X, object->Transform.Scale.Y, object->Transform.Scale.Z));

                switch (object->AssetType)
                {
                case Lumina::Data::Parsing::Layer::LayerEntryType::LayLight:
                {
                    auto instance_object = static_cast<Lumina::Data::Parsing::Layer::LayerCommon::LightInstanceObject^>(object->Object);
                    if (instance_object->LightType == Lumina::Data::Parsing::Layer::LightType::None ||
                            instance_object->LightType == Lumina::Data::Parsing::Layer::LightType::Line)
                        break;

                    auto light_node = FbxLight::Create (scene, "Light");
                    switch (instance_object->LightType) {
                    case Lumina::Data::Parsing::Layer::LightType::Point:
                    case Lumina::Data::Parsing::Layer::LightType::Specular:
                        light_node->LightType.Set(FbxLight::EType::ePoint);
                        break;
                    case Lumina::Data::Parsing::Layer::LightType::Directional:
                        light_node->LightType.Set(FbxLight::EType::eDirectional);
                        break;
                    case Lumina::Data::Parsing::Layer::LightType::Spot:
                        light_node->LightType.Set(FbxLight::EType::eSpot);
                        break;
                    case Lumina::Data::Parsing::Layer::LightType::Plane:
                        light_node->LightType.Set(FbxLight::EType::eArea);
                        light_node->AreaLightShape.Set(FbxLight::EAreaLightShape::eRectangle);
                        break;
                    default:
                        light_node->LightType.Set(FbxLight::EType::ePoint);
                        break;
                    }

                    light_node->Color.Set(FbxDouble3(instance_object->DiffuseColorHDRI.Red,
                        instance_object->DiffuseColorHDRI.Green, instance_object->DiffuseColorHDRI.Blue));
                    light_node->Intensity.Set(static_cast<double>(instance_object->DiffuseColorHDRI.Intensity) * 100.);
                    light_node->InnerAngle.Set(0.);
                    light_node->OuterAngle.Set(static_cast<double>(instance_object->ConeDegree));
                    light_node->CastShadows.Set(instance_object->BGShadowEnabled > 0);
                    light_node->DecayStart.Set(25.);

                    object_node->SetNodeAttribute(light_node);
                    // Technically this should be SetPostTargetRotation but that doesn't work with Blender importer
                    object_node->LclRotation.Set(FbxDouble3(
                        object_node->LclRotation.Get().mData[0] - 90.,
                        object_node->LclRotation.Get().mData[1],
                        object_node->LclRotation.Get().mData[2]
                    ));
                    layer_node->AddChild(object_node);
                    break;
                }
                case Lumina::Data::Parsing::Layer::LayerEntryType::BG:
                {
                    auto instance_object = static_cast<Lumina::Data::Parsing::Layer::LayerCommon::BGInstanceObject^>(object->Object);
                    auto object_path = instance_object->AssetPath;
                    if (!data->FileExists(object_path)) break;
                    auto model = gcnew Lumina::Models::Models::Model(data, object_path, Lumina::Models::Models::Model::ModelLod::High, 1);

                    auto model_node = FbxNode::Create(scene, Util::get_std_str(object_path->Substring(object_path->LastIndexOf('/') + 1)).c_str());

                    process_model(model, &model_node);

                    object_node->AddChild(model_node);
                    layer_node->AddChild(object_node);

                    break;
                }
                case Lumina::Data::Parsing::Layer::LayerEntryType::SharedGroup:
                {
                    auto instance_object = static_cast<Lumina::Data::Parsing::Layer::LayerCommon::SharedGroupInstanceObject^>(object->Object);
                    if (!instance_object->SgbFileObj) break;
                    auto object_path = instance_object->AssetPath;

                    export_sgb_models(data, instance_object->SgbFileObj, &object_node);
                    for each (SaintCoinach::Graphics::Sgb::ISgbData^ rootGimGroupF in instance_object->SgbFileObj->Data) {
                        if (rootGimGroupF == nullptr || rootGimGroupF->GetType() != SaintCoinach::Graphics::Sgb::SgbGroup::typeid)
                            continue;
                        else
                        {
                            auto rootGimGroup = static_cast<SaintCoinach::Graphics::Sgb::SgbGroup^>(rootGimGroupF);
                            for each (SaintCoinach::Graphics::Sgb::ISgbGroupEntry^ rootGimEntryF in rootGimGroup->Entries) {
                                if (rootGimEntryF == nullptr || rootGimEntryF->GetType() != SaintCoinach::Graphics::Sgb::SgbGimmickEntry::typeid)
                                    continue;
                                else
                                {
                                    auto rootGimEntry = static_cast<SaintCoinach::Graphics::Sgb::SgbGimmickEntry^>(rootGimEntryF);
                                    if (rootGimEntry->Gimmick != nullptr) {
                                        auto root_gim_entry_node = FbxNode::Create(scene, Util::get_std_str(rootGimEntry->Name).c_str());

                                        root_gim_entry_node->LclTranslation.Set(FbxDouble3(rootGimEntry->Header.Translation.X,
                                            rootGimEntry->Header.Translation.Y, rootGimEntry->Header.Translation.Z));
                                        root_gim_entry_node->LclRotation.Set(FbxDouble3(Util::degrees(rootGimEntry->Header.Rotation.X),
                                            Util::degrees(rootGimEntry->Header.Rotation.Y),
                                            Util::degrees(rootGimEntry->Header.Rotation.Z)));
                                        root_gim_entry_node->LclScaling.Set(FbxDouble3(rootGimEntry->Header.Scale.X,
                                            rootGimEntry->Header.Scale.Y, rootGimEntry->Header.Scale.Z));
                                        export_sgb_models(data, rootGimEntry->Gimmick, &root_gim_entry_node);
                                        for each (SaintCoinach::Graphics::Sgb::ISgbData^ subGimGroupF in rootGimEntry->Gimmick->Data) {
                                            if (subGimGroupF == nullptr || subGimGroupF->GetType() != SaintCoinach::Graphics::Sgb::SgbGroup::typeid)
                                                continue;
                                            else
                                            {
                                                auto subGimGroup = static_cast<SaintCoinach::Graphics::Sgb::SgbGroup^>(subGimGroupF);
                                                for each (SaintCoinach::Graphics::Sgb::ISgbGroupEntry^ subGimEntryF in subGimGroup->Entries) {
                                                    if (subGimEntryF == nullptr || subGimEntryF->GetType() != SaintCoinach::Graphics::Sgb::SgbGimmickEntry::typeid)
                                                        continue;
                                                    else
                                                    {
                                                        auto subGimEntry = static_cast<SaintCoinach::Graphics::Sgb::SgbGimmickEntry^>(subGimEntryF);
                                                        auto sub_gim_entry_node = FbxNode::Create(scene, Util::get_std_str(subGimEntry->Name).c_str());

                                                        sub_gim_entry_node->LclTranslation.Set(FbxDouble3(subGimEntry->Header.Translation.X,
                                                            subGimEntry->Header.Translation.Y, subGimEntry->Header.Translation.Z));
                                                        sub_gim_entry_node->LclRotation.Set(FbxDouble3(Util::degrees(subGimEntry->Header.Rotation.X),
                                                            Util::degrees(subGimEntry->Header.Rotation.Y),
                                                            Util::degrees(subGimEntry->Header.Rotation.Z)));
                                                        sub_gim_entry_node->LclScaling.Set(FbxDouble3(subGimEntry->Header.Scale.X,
                                                            subGimEntry->Header.Scale.Y, subGimEntry->Header.Scale.Z));
                                                        //var subGimTransform = CreateMatrix(subGimEntry.Header.Translation, subGimEntry.Header.Rotation, subGimEntry.Header.Scale);
                                                        export_sgb_models(data, subGimEntry->Gimmick, &sub_gim_entry_node);

                                                        root_gim_entry_node->AddChild(sub_gim_entry_node);
                                                    }
                                                }
                                            }
                                        }
                                        object_node->AddChild(root_gim_entry_node);
                                    }
                                }
                            }
                        }
                    }

                    layer_node->AddChild(object_node);
                    break;
                }
                case Lumina::Data::Parsing::Layer::LayerEntryType::EventObject:
                {
                    auto instance_object = static_cast<Lumina::Data::Parsing::Layer::LayerCommon::EventInstanceObject^>(object->Object);
                    auto eventObjectSheet = data->GetExcelSheet<Lumina::Excel::GeneratedSheets::EObj^>();
                    auto exportedSgSheet = data->GetExcelSheet<Lumina::Excel::GeneratedSheets::ExportedSG^>();

                    for each (Lumina::Excel::GeneratedSheets::EObj^ row in eventObjectSheet) {
                        if (row->RowId == instance_object->ParentData.BaseId) {
                            auto sgbPathRow = row->SgbPath->Value;
                            if (sgbPathRow) {
                                System::String^ sgbPath = sgbPathRow->SgbPath;
                                auto rawFile = data->GetFile(sgbPath);
                                auto sgbFile = gcnew SaintCoinach::Graphics::Sgb::SgbFile(data,
                                    rawFile->Data);
                                export_sgb_models(data, sgbFile, &object_node);

                                layer_node->AddChild(object_node);
                            }
                        }
                    }
                    break;
                }
				default:
					break;
				}
            }
            scene->GetRootNode()->AddChild(layer_node);
        }
    }
    
    return true;
}

void ZoneExporter::process_model(Lumina::Models::Models::Model^ model, FbxNode** node)
{
    auto path = model->File->FilePath->Path;
    path = path->Substring(path->LastIndexOf('/') + 1);

    for (int j = 0; j < model->Meshes->Length; j++)
    {
        const auto mesh_name = Util::get_std_str(path + "_") + std::to_string(j);
        FbxNode* mesh_node = FbxNode::Create(manager, mesh_name.c_str());

        auto result = mesh_cache->find(mesh_name);
        FbxMesh* mesh;
        if (result != mesh_cache->end())
        {
            mesh = result->second;
        } else {
            mesh = create_mesh(model->Meshes[j], mesh_name.c_str());
            FbxSurfacePhong* material;
            create_material(model->Meshes[j]->Material, &material);
            mesh_node->AddMaterial(material);
            mesh_cache->insert({mesh_name, mesh});
        }
        
        mesh_node->SetNodeAttribute(mesh);
        (*node)->AddChild(mesh_node);
    }
}

FbxMesh* ZoneExporter::create_mesh(Lumina::Models::Models::Mesh^ game_mesh, const char* mesh_name)
{
    FbxMesh* mesh = FbxMesh::Create(scene, mesh_name);
    mesh->InitControlPoints(game_mesh->Vertices->Length);
    mesh->InitNormals(game_mesh->Vertices->Length);

    FbxGeometryElementVertexColor* colorElement = mesh->CreateElementVertexColor();
    colorElement->SetMappingMode(FbxLayerElement::EMappingMode::eByControlPoint);

    FbxGeometryElementUV* uvElement1 = mesh->CreateElementUV("uv1");
    uvElement1->SetMappingMode(FbxLayerElement::EMappingMode::eByControlPoint);

    FbxGeometryElementUV* uvElement2 = mesh->CreateElementUV("uv2");
    uvElement2->SetMappingMode(FbxLayerElement::EMappingMode::eByControlPoint);

    FbxGeometryElementTangent* tangentElem1 = mesh->CreateElementTangent();
    tangentElem1->SetMappingMode(FbxLayerElement::EMappingMode::eByControlPoint);

    FbxGeometryElementTangent* tangentElem2 = mesh->CreateElementTangent();
    tangentElem2->SetMappingMode(FbxLayerElement::EMappingMode::eByControlPoint);

    for (int i = 0; i < game_mesh->Vertices->Length; i++)
    {
        FbxVector4 pos, norm, uv, tangent1, tangent2;
        FbxColor color;
        if (game_mesh->Vertices[i].Position.HasValue) {}
            pos = FbxVector4(game_mesh->Vertices[i].Position.Value.X,
                             game_mesh->Vertices[i].Position.Value.Y,
                             game_mesh->Vertices[i].Position.Value.Z,
                             game_mesh->Vertices[i].Position.Value.W);

        if (game_mesh->Vertices[i].Normal.HasValue)
            norm = FbxVector4(game_mesh->Vertices[i].Normal.Value.X,
                              game_mesh->Vertices[i].Normal.Value.Y,
                              game_mesh->Vertices[i].Normal.Value.Z,
                              0);
        // if (game_mesh->Vertices[i].UV.HasValue)
        //     uv = FbxVector4(game_mesh->Vertices[i].UV.Value.X,
        //                             game_mesh->Vertices[i].UV.Value.Y,
        //                             game_mesh->Vertices[i].UV.Value.Z,
        //                             game_mesh->Vertices[i].UV.Value.W);

        if (game_mesh->Vertices[i].Color.HasValue)
            color = FbxColor(game_mesh->Vertices[i].Color.Value.X,
                             game_mesh->Vertices[i].Color.Value.Y,
                             game_mesh->Vertices[i].Color.Value.Z,
                             game_mesh->Vertices[i].Color.Value.W);

        if (game_mesh->Vertices[i].Tangent1.HasValue)
            tangent1 = FbxVector4(game_mesh->Vertices[i].Tangent1.Value.X,
                                  game_mesh->Vertices[i].Tangent1.Value.Y,
                                  game_mesh->Vertices[i].Tangent1.Value.Z,
                                  game_mesh->Vertices[i].Tangent1.Value.W);
        if (game_mesh->Vertices[i].Tangent2.HasValue)
            tangent2 = FbxVector4(game_mesh->Vertices[i].Tangent2.Value.X,
                                  game_mesh->Vertices[i].Tangent2.Value.Y,
                                  game_mesh->Vertices[i].Tangent2.Value.Z,
                                  game_mesh->Vertices[i].Tangent2.Value.W);

        if (pos && norm)
            mesh->SetControlPointAt(pos, norm, i);

        if (game_mesh->Vertices[i].UV.HasValue)
        {
            uvElement1->GetDirectArray().Add(FbxVector2(game_mesh->Vertices[i].UV.Value.X, game_mesh->Vertices[i].UV.Value.Y * -1));
            uvElement2->GetDirectArray().Add(FbxVector2(game_mesh->Vertices[i].UV.Value.Z, game_mesh->Vertices[i].UV.Value.W * -1));
        }

        // Color
        colorElement->GetDirectArray().Add(color);

        // Tangents
        tangentElem1->GetDirectArray().Add(tangent1);
        tangentElem2->GetDirectArray().Add(tangent2);
    }

    for (int i = 0; i < game_mesh->Indices->Length; i += 3)
    {
        mesh->BeginPolygon();
        mesh->AddPolygon(game_mesh->Indices[i]);
        mesh->AddPolygon(game_mesh->Indices[i + 1]);
        mesh->AddPolygon(game_mesh->Indices[i + 2]);
        mesh->EndPolygon();
    }

    return mesh;
}

void ZoneExporter::export_sgb_models(Lumina::GameData^ data, SaintCoinach::Graphics::Sgb::SgbFile^ sgbFile, FbxNode** node)
{
    for each(SaintCoinach::Graphics::Sgb::ISgbData^ sgbGroupF in sgbFile->Data) {
        if (sgbGroupF == nullptr || sgbGroupF->GetType() != SaintCoinach::Graphics::Sgb::SgbGroup::typeid)
            continue;
        else
        {
            auto sgbGroup = static_cast<SaintCoinach::Graphics::Sgb::SgbGroup^>(sgbGroupF);
            bool newGroup = true;

            for each (SaintCoinach::Graphics::Sgb::ISgbGroupEntry^ sgb1CEntryF in sgbGroup->Entries) {
                if (sgb1CEntryF == nullptr || sgb1CEntryF->GetType() != SaintCoinach::Graphics::Sgb::SgbGroup1CEntry::typeid)
                    continue;
                else
                {
                    auto sgb1CEntry = static_cast<SaintCoinach::Graphics::Sgb::SgbGroup1CEntry^>(sgb1CEntryF);
                    if (sgb1CEntry->Gimmick != nullptr) {
                        export_sgb_models(data, sgb1CEntry->Gimmick, node);
                        for each(SaintCoinach::Graphics::Sgb::ISgbData^ subGimGroupF in sgb1CEntry->Gimmick->Data) {
                            if (subGimGroupF == nullptr || subGimGroupF->GetType() != SaintCoinach::Graphics::Sgb::SgbGroup::typeid)
                                continue;
                            else
                            {
                                auto subGimGroup = static_cast<SaintCoinach::Graphics::Sgb::SgbGroup^>(subGimGroupF);
                                for each(SaintCoinach::Graphics::Sgb::ISgbGroupEntry^ subGimEntryF in subGimGroup->Entries) {
                                    if (subGimEntryF == nullptr || subGimEntryF->GetType() != SaintCoinach::Graphics::Sgb::SgbGimmickEntry::typeid)
                                        continue;
                                    else
                                    {
                                        auto subGimEntry = static_cast<SaintCoinach::Graphics::Sgb::SgbGimmickEntry^>(subGimEntryF);
                                        auto sub_gim_entry_node = FbxNode::Create(scene, Util::get_std_str(subGimEntry->Name).c_str());

                                        sub_gim_entry_node->LclTranslation.Set(FbxDouble3(subGimEntry->Header.Translation.X,
                                            subGimEntry->Header.Translation.Y, subGimEntry->Header.Translation.Z));
                                        sub_gim_entry_node->LclRotation.Set(FbxDouble3(Util::degrees(subGimEntry->Header.Rotation.X),
                                            Util::degrees(subGimEntry->Header.Rotation.Y),
                                            Util::degrees(subGimEntry->Header.Rotation.Z)));
                                        sub_gim_entry_node->LclScaling.Set(FbxDouble3(subGimEntry->Header.Scale.X,
                                            subGimEntry->Header.Scale.Y, subGimEntry->Header.Scale.Z));
                                        export_sgb_models(data, subGimEntry->Gimmick, &sub_gim_entry_node);

                                        (*node)->AddChild(sub_gim_entry_node);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            for each (SaintCoinach::Graphics::Sgb::ISgbGroupEntry^ mdlF in sgbGroup->Entries) {
                if (mdlF == nullptr || mdlF->GetType() != SaintCoinach::Graphics::Sgb::SgbModelEntry::typeid)
                    continue;
                else
                {
                    auto mdl = static_cast<SaintCoinach::Graphics::Sgb::SgbModelEntry^>(mdlF);
                    System::String^ mdlFilePath = mdl->ModelFilePath;
                    auto model = gcnew Lumina::Models::Models::Model(data,
                        mdlFilePath,
                        Lumina::Models::Models::Model::ModelLod::High,
                        1);

                    auto model_node = FbxNode::Create(scene, Util::get_std_str(mdlFilePath->Substring(mdlFilePath->LastIndexOf('/') + 1)).c_str());

                    model_node->LclTranslation.Set(FbxDouble3(mdl->Header.Translation.X,
                        mdl->Header.Translation.Y, mdl->Header.Translation.Z));
                    model_node->LclRotation.Set(FbxDouble3(Util::degrees(mdl->Header.Rotation.X),
                        Util::degrees(mdl->Header.Rotation.Y),
                        Util::degrees(mdl->Header.Rotation.Z)));
                    model_node->LclScaling.Set(FbxDouble3(mdl->Header.Scale.X,
                        mdl->Header.Scale.Y, mdl->Header.Scale.Z));
                    process_model(model, &model_node);

                    (*node)->AddChild(model_node);
                }
            }
        }
    }
}

bool ZoneExporter::create_material0(Lumina::Models::Materials::Material^ mat, FbxSurfacePhong** out)
{
    auto mat_path = mat->MaterialPath;
    auto material_name = mat_path->Substring(mat_path->LastIndexOf('/') + 1);
    auto std_material_name = Util::get_std_str(material_name);

    const auto hash = mat->File->FilePath->IndexHash;
    auto result = material_cache->find(hash);
    if (result != material_cache->end())
    {
        *out = result->second;
        return true;
    }
    extract_textures(mat);
    *out = FbxSurfacePhong::Create(scene, std_material_name.c_str());

    (*out)->AmbientFactor.Set(1.);
    (*out)->DiffuseFactor.Set(1.);
    (*out)->SpecularFactor.Set(0.25);
    (*out)->ReflectionFactor.Set(0.);
    (*out)->BumpFactor.Set(0.4);  // looks good on most things, higher will cause wrong lighting
    (*out)->Shininess.Set(25.);

    (*out)->ShadingModel.Set("Phong");

    // really don't like raw pointers
    std::queue<FbxFileTexture*> diffuseTextures, normalTextures, specularTextures;

    for (int i = 0; i < mat->Textures->Length; i++)
    {
        if (mat->Textures[i]->TexturePath->Contains("dummy"))
            continue;

        auto usage_name = mat->Textures[i]->TextureUsageSimple.ToString();

        auto texture = FbxFileTexture::Create(scene, Util::get_std_str(usage_name).c_str());
        auto rel = Util::get_relative_texture_path(out_folder, zone_code, mat->Textures[i]->TexturePath);
        texture->SetFileName(rel.c_str());
        texture->SetMappingType(FbxTexture::eUV);
        texture->SetTextureUse(FbxTexture::eStandard);
        texture->SetMaterialUse(FbxFileTexture::eModelMaterial);
        texture->SetSwapUV(false);
        texture->SetTranslation(0.0, 0.0);
        texture->SetScale(1.0, 1.0);
        texture->SetRotation(0.0, 0.0);

        //System::Console::WriteLine(mat->Textures[i]->TextureUsageRaw);
        //System::Console::WriteLine(mat->Textures[i]->TextureUsageSimple);
        switch (mat->Textures[i]->TextureUsageRaw)
        {
        case Lumina::Data::Parsing::TextureUsage::Sampler0:
        case Lumina::Data::Parsing::TextureUsage::Sampler1:
        case Lumina::Data::Parsing::TextureUsage::SamplerColorMap0:
        case Lumina::Data::Parsing::TextureUsage::SamplerColorMap1:
            diffuseTextures.push(texture); break;
        case Lumina::Data::Parsing::TextureUsage::SamplerSpecularMap0:
        case Lumina::Data::Parsing::TextureUsage::SamplerSpecularMap1:
            normalTextures.push(texture); break;
        case Lumina::Data::Parsing::TextureUsage::SamplerNormalMap0:
        case Lumina::Data::Parsing::TextureUsage::SamplerNormalMap1:
            specularTextures.push(texture); break;
        default:;
        }
        // We are ignoring the 2nd texture that they use for blending
        /*switch (mat->Textures[i]->TextureUsageRaw)
        {
        case Lumina::Data::Parsing::TextureUsage::SamplerColorMap0:
            (*out)->Diffuse.ConnectSrcObject(texture); break;
        case Lumina::Data::Parsing::TextureUsage::SamplerSpecularMap0:
            (*out)->Specular.ConnectSrcObject(texture); break;
        case Lumina::Data::Parsing::TextureUsage::SamplerNormalMap0:
            (*out)->NormalMap.ConnectSrcObject(texture);
            break;
        default:;
        }*/
    }

    // connect textures
    makeMaybeLayeredTexture(diffuseTextures, std_material_name, (*out)->Diffuse);
    makeMaybeLayeredTexture(normalTextures, std_material_name, (*out)->NormalMap);
    makeMaybeLayeredTexture(specularTextures, std_material_name, (*out)->Specular);

    material_cache->insert({ hash, *out });

    return true;
}

void ZoneExporter::makeMaybeLayeredTexture(std::queue<fbxsdk::FbxFileTexture*>& textures, std::string& std_material_name, FbxPropertyT<FbxDouble3>& out)
{
    if (textures.size() > 1) {
        auto layeredTexture = FbxLayeredTexture::Create(scene, (std_material_name + std::string("_texture_group")).c_str());
        int index = 0;
        while (!textures.empty()) {
            layeredTexture->ConnectSrcObject(textures.front());
            textures.pop();
            layeredTexture->SetTextureBlendMode(index, FbxLayeredTexture::EBlendMode::eAdditive);
            index++;
        }
        out.ConnectSrcObject(layeredTexture);
    }
    else if (!textures.empty()) {
        out.ConnectSrcObject(textures.front());
        textures.pop();
    }
}

bool ZoneExporter::create_material(Lumina::Models::Materials::Material^ mat, FbxSurfacePhong** out)
{
    auto mat_path = mat->MaterialPath;
    auto material_name = mat_path->Substring(mat_path->LastIndexOf('/') + 1);
    auto std_material_name = Util::get_std_str(material_name);

    const auto hash = mat->File->FilePath->IndexHash;
    auto result = material_cache->find(hash);
    if (result != material_cache->end())
    {
        *out = result->second;
        return true;
    }
    extract_textures(mat);
    *out = FbxSurfacePhong::Create(scene, std_material_name.c_str());

    (*out)->AmbientFactor.Set(1.);
    (*out)->DiffuseFactor.Set(1.);
    (*out)->SpecularFactor.Set(0.25);
    (*out)->ReflectionFactor.Set(0.);
    (*out)->BumpFactor.Set(0.3);  // looks good on most things, higher will cause wrong lighting
    (*out)->Shininess.Set(25.);

    (*out)->ShadingModel.Set("Phong");

    for (int i = 0; i < mat->Textures->Length; i++)
    {
        if (mat->Textures[i]->TexturePath->Contains("dummy"))
            continue;

        auto usage_name = mat->Textures[i]->TextureUsageSimple.ToString();

        auto texture = FbxFileTexture::Create(scene, Util::get_std_str(usage_name).c_str());
        auto rel = Util::get_relative_texture_path(out_folder, zone_code, mat->Textures[i]->TexturePath);
        texture->SetFileName(rel.c_str());
        texture->SetMappingType(FbxTexture::eUV);
        texture->SetTextureUse(FbxTexture::eStandard);
        texture->SetMaterialUse(FbxFileTexture::eModelMaterial);
        texture->SetSwapUV(false);
        texture->SetTranslation(0.0, 0.0);
        texture->SetScale(1.0, 1.0);
        texture->SetRotation(0.0, 0.0);

        //System::Console::WriteLine(mat->Textures[i]->TextureUsageRaw);
        //System::Console::WriteLine(mat->Textures[i]->TextureUsageSimple);
        // We are ignoring the 2nd texture that they use for blending
        switch (mat->Textures[i]->TextureUsageRaw)
        {
            case Lumina::Data::Parsing::TextureUsage::SamplerColorMap0:
                (*out)->Diffuse.ConnectSrcObject(texture); break;
            case Lumina::Data::Parsing::TextureUsage::SamplerSpecularMap0:
                (*out)->Specular.ConnectSrcObject(texture); break;
            case Lumina::Data::Parsing::TextureUsage::SamplerNormalMap0:
                (*out)->NormalMap.ConnectSrcObject(texture);
                break;
            default: ;
        }
    }

    material_cache->insert({hash, *out});


    return true;
}

void ZoneExporter::extract_textures(Lumina::Models::Materials::Material^ mat)
{
    // I wish this was a C# function
    for (int i = 0; i < mat->Textures->Length; i++)
    {
        auto tex_path = Util::get_texture_path(out_folder, zone_code, mat->Textures[i]->TexturePath);

        if (System::IO::File::Exists(tex_path))
            continue;

        Lumina::Data::Files::TexFile^ texfile;
        try { texfile = mat->Textures[i]->GetTextureNc(data); } catch (System::Exception^ exception) { continue; }
        
        // end me
        unsigned char* arr = new unsigned char[texfile->ImageData->Length];
        for (int i = 0; i < texfile->ImageData->Length; i++)
            arr[i] = texfile->ImageData[i];

        System::Drawing::Image^ texture = gcnew System::Drawing::Bitmap(texfile->Header.Width,
                                                                        texfile->Header.Height,
                                                                        texfile->Header.Width * 4,
                                                                        System::Drawing::Imaging::PixelFormat::Format32bppArgb,
                                                                        System::IntPtr(arr));
        System::IO::Directory::CreateDirectory(System::IO::Path::GetDirectoryName(tex_path));
        texture->Save(tex_path, System::Drawing::Imaging::ImageFormat::Png);
        delete[] arr;
    }
}

bool ZoneExporter::save_scene()
{
    auto exporter = FbxExporter::Create(manager, "exporter");
    auto out_fbx = out_folder + zone_code + ".fbx";

    if (!exporter->Initialize(out_fbx.c_str(), -1, manager->GetIOSettings()))
        return false;

    const auto result = exporter->Export(scene);
    auto test = exporter->GetStatus();
    exporter->Destroy();
    return result;
}

bool ZoneExporter::init(System::String^ game_path)
{
    data = gcnew Lumina::GameData(game_path, gcnew Lumina::LuminaOptions());
    auto name = zone_path.substr(zone_path.rfind("/level") - 4, 4);

    manager = FbxManager::Create();
    if (!manager)
        return false;

    scene = FbxScene::Create(manager, name.c_str());
    if (!scene)
        return false;
    FbxGlobalSettings& globalSettings = scene->GetGlobalSettings();
    globalSettings.SetSystemUnit(FbxSystemUnit::m);

    FbxIOSettings* io = FbxIOSettings::Create(manager, "IOSRoot");
    manager->SetIOSettings(io);

    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_MATERIAL, true);
    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_TEXTURE, true);
    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_EMBEDDED, false);
    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_SHAPE, true);
    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_GOBO, true);
    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_ANIMATION, false);
    (*manager->GetIOSettings()).SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);

    return true;
}

void ZoneExporter::uninit()
{
    data = nullptr;
    if (manager)
        manager->Destroy();
    zone_path = "";
    out_folder = "";
    zone_code = "";
    delete material_cache;
    delete mesh_cache;
}

ZoneExporter::ZoneExporter()
{
    // We don't want to restrict a ZoneExporter object to a single export,
    // so any initialization code is contained in export_zone
}

ZoneExporter::~ZoneExporter()
{
    uninit();
}
