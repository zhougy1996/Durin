#include "AssetForge/Builtins/SceneImportData.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Misc/StringHelper.h"
namespace Durin::AssetForge::Builtins
{
	auto IsOneTimeModelSource(std::string_view Path) -> bool
	{
		const auto Extension = StringUtils::FoldAscii(std::filesystem::path(Path).extension().generic_string());
		return Extension == ".gltf" || Extension == ".glb" || Extension == ".fbx";
	}
	auto IsOneTimeModelImportedAsset(const DObject& Object) -> bool
	{
		const DAssetImportData* Data = nullptr;
		if (const auto* Mesh = Cast<DStaticMesh>(&Object)) Data = Mesh->GetAssetImportData();
		if (const auto* Texture = Cast<DTexture2D>(&Object)) Data = Texture->GetAssetImportData();
		if (const auto* Material = Cast<DMaterialInterface>(&Object))
			return IsOneTimeModelSource(Material->GetImportProvenance().SourceIdentity);
		if (const auto* Scene = Cast<DSceneImportData>(Data))
			if (IsOneTimeModelSource(Scene->SourceIdentity)) return true;
		const auto* Source = Data ? Data->GetSourceData().FindByRole("source") : nullptr;
		return Source && IsOneTimeModelSource(Source->Hint);
	}
}
