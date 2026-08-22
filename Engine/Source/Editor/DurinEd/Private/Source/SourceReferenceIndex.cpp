#include "Source/SourceReferenceIndex.h"

#include "AssetAuthoring.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::Editor
{
	namespace
	{
		auto AddReference(
			std::unordered_map<std::string, std::vector<FSourceReference>>& References,
			const Asset::FAssetData& Data,
			std::string_view SourcePath) -> void
		{
			if (SourcePath.empty()) return;
			References[std::string(SourcePath)].push_back({
				.AssetPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName});
		}

		auto InspectKnownSourceFields(
			const Asset::FAssetData& Data,
			std::unordered_map<std::string, std::vector<FSourceReference>>& References)
			-> bool
		{
			Asset::FAssetPackageInspection Inspection;
			if (!Asset::InspectAssetPackage(Data.PhysicalPath, Inspection)) return false;
			const Asset::FAssetPackageField* SourceField =
				Inspection.FindField("SourceImportData");
			if (!SourceField) return true;

			FTexture2DSourceImportData Texture2DSource;
			if (SourceField->TryReadStruct(
				FTexture2DSourceImportData::StaticStruct(), &Texture2DSource))
			{
				AddReference(
					References, Data, Texture2DSource.Source.SourcePath.Path);
				return true;
			}

			FVolumeTextureSourceImportData VolumeTextureSource;
			if (SourceField->TryReadStruct(
				FVolumeTextureSourceImportData::StaticStruct(), &VolumeTextureSource))
			{
				AddReference(References, Data,
					VolumeTextureSource.Source.SourcePath.Path);
				return true;
			}

			FTerrainHeightmapSourceImportData HeightmapSource;
			if (SourceField->TryReadStruct(
				FTerrainHeightmapSourceImportData::StaticStruct(), &HeightmapSource))
			{
				AddReference(References, Data, HeightmapSource.SourcePath.Path);
				return true;
			}

			FStaticMeshSourceImportData StaticMeshSource;
			if (SourceField->TryReadStruct(
				FStaticMeshSourceImportData::StaticStruct(), &StaticMeshSource))
			{
				AddReference(References, Data, StaticMeshSource.SourcePath.Path);
				return true;
			}

			FTextureCubeSourceImportData TextureCubeSource;
			if (SourceField->TryReadStruct(
				FTextureCubeSourceImportData::StaticStruct(), &TextureCubeSource))
			{
				if (TextureCubeSource.SourceLayout
					== ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					AddReference(
						References, Data,
						TextureCubeSource.Panorama.SourcePath.Path);
				}
				else
				{
					for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
						AddReference(
							References, Data,
							TextureCubeSource.GetFace(
								static_cast<ETextureCubeFace>(Index))
								.SourcePath.Path);
				}
				return true;
			}
			return true;
		}
	} // namespace

	auto FSourceReferenceIndex::Refresh(size_t MaximumPackageInspections) -> void
	{
		const Asset::FAssetCatalogSnapshot Catalog =
			Asset::CaptureAssetCatalogSnapshot();
		if (RegistryRevision == Catalog.Revision) return;

		References.clear();
		InspectedPackageCount = 0;
		Warning.clear();
		for (const auto& [Path, Asset] : Catalog.Assets)
		{
			if (Asset.AssetClassName.find("Texture2D") == std::string::npos
				&& Asset.AssetClassName.find("TextureCube") == std::string::npos
				&& Asset.AssetClassName.find("VolumeTexture") == std::string::npos
				&& Asset.AssetClassName.find("StaticMesh") == std::string::npos
				&& Asset.AssetClassName.find("TerrainHeightmap") == std::string::npos)
				continue;
			if (InspectedPackageCount >= MaximumPackageInspections)
			{
				Warning = std::format(
					"Source reference inspection stopped after {} packages; impact results are incomplete.",
					MaximumPackageInspections);
				break;
			}
			++InspectedPackageCount;
			if (!InspectKnownSourceFields(Asset, References) && Warning.empty())
				Warning = std::format(
					"One or more source-bearing packages could not be inspected; impact results may be incomplete.");
		}
		for (auto& [SourcePath, Assets] : References)
			std::ranges::sort(Assets, {}, [](const FSourceReference& Reference) {
				return Reference.AssetPath.ToString();
			});
		RegistryRevision = Catalog.Revision;
	}

	auto FSourceReferenceIndex::Invalidate() -> void
	{
		RegistryRevision = 0;
	}

	auto FSourceReferenceIndex::FindReferences(std::string_view SourceVirtualPath) const
		-> std::span<const FSourceReference>
	{
		const auto It = References.find(std::string(SourceVirtualPath));
		return It == References.end()
			? std::span<const FSourceReference>{}
			: std::span<const FSourceReference>(It->second);
	}
} // namespace Durin::Editor
