#include "BuiltinImportProviderCommon.h"

#include "DObject/Package.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace Durin::AssetForge::Builtins
{
	auto FAssetForgeBuiltinsAssetFeatures::Validate(const DObject& Object) const
		-> FAssetAuthoringReadinessFeatureResult
	{
		auto NotReady = [](std::string_view Domain) {
			return FAssetAuthoringReadinessFeatureResult{
				.bHandled = true,
				.Result = {Asset::EAssetError::StaleData,
					std::format("{} post-load recovery did not publish domain-ready data.", Domain)}};
		};
		if (const auto* Mesh = Cast<DStaticMesh>(&Object))
			return Mesh->GetRenderData()
				? FAssetAuthoringReadinessFeatureResult{.bHandled = true}
				: NotReady("StaticMesh");
		if (const auto* Mesh = Cast<DSkeletalMesh>(&Object))
			return Mesh->GetRenderData()
				? FAssetAuthoringReadinessFeatureResult{.bHandled = true}
				: NotReady("SkeletalMesh");
		if (const auto* Texture = Cast<DTexture2D>(&Object))
			return Texture->GetPlatformData()
				&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready
				? FAssetAuthoringReadinessFeatureResult{.bHandled = true}
				: NotReady("Texture2D");
		if (const auto* Texture = Cast<DTextureCube>(&Object))
			return Texture->GetPlatformData()
				&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready
				? FAssetAuthoringReadinessFeatureResult{.bHandled = true}
				: NotReady("TextureCube");
		if (const auto* Texture = Cast<DVolumeTexture>(&Object))
			return Texture->GetPlatformData()
				&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready
				? FAssetAuthoringReadinessFeatureResult{.bHandled = true}
				: NotReady("VolumeTexture");
		if (const auto* Heightmap = Cast<DTerrainHeightmap>(&Object))
			return Heightmap->GetPayload()
				&& Heightmap->GetStatus() == ETerrainHeightmapStatus::Ready
				? FAssetAuthoringReadinessFeatureResult{.bHandled = true}
				: NotReady("TerrainHeightmap");
		return {};
	}

	auto FAssetForgeBuiltinsAssetFeatures::BuildFileProduct(
		DStaticMesh& Mesh,
		std::string_view SourcePath,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceContentHash,
		FStaticMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		return BuildStaticMeshFileProduct(
			Mesh, SourcePath, std::move(SourceImportData), SourceContentHash, OutProduct, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::PostLoadUncooked(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		return PostLoadStaticMesh(Mesh, OutDiagnostic, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::ChangeSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return ChangeStaticMeshSourceReference(Mesh, SourceVirtualPath, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::PostLoadUncooked(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return PostLoadTexture2DFeature(Texture, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::WaitForRecovery(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		return WaitForTexture2DImportRecovery(Texture, TimeoutSeconds);
	}

	auto FAssetForgeBuiltinsAssetFeatures::RecoverUncooked(
		DVolumeTexture& Texture, std::string& OutError) -> bool
	{
		const std::string Key = Asset::MakeVolumeTextureDerivedDataKey(Texture, OutError);
		if (Key.empty()) return false;
		std::unique_ptr<FVolumeTexturePlatformData> Cached;
		ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
		std::string Message;
		if (Asset::LoadVolumeTextureDerivedData(Key, Cached, Status, Message))
			return Texture.PublishDerivedDataLoad(std::move(Cached), Key, OutError);
		const FVolumeTextureSourceImportData& Source = Texture.GetSourceImportData();
		FAssetPath Destination;
		if (!Texture.GetPackage() || !Source.HasSource()
			|| !FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
			return false;
		const FVolumeTextureImportSettings Settings{
			.ImportFormat = Source.ImportFormat, .Channels = Source.Channels,
			.SliceWidth = Source.SliceWidth, .SliceHeight = Source.SliceHeight,
			.Depth = Source.Depth, .TilesX = Source.TilesX, .TilesY = Source.TilesY};
		FImportProvenance Existing;
		std::optional<FImportProvenance> Provenance;
		if (InspectVolumeTextureImportProvenance(Texture, Existing, OutError))
			Provenance = std::move(Existing);
		else OutError.clear();
		FImportRequest Request;
		if (!MakeVolumeTextureImportRequest(Source.Source.SourcePath, Destination,
			Settings, EImportMode::Recover,
			{.OwnerId = std::format("VolumeTexture.Recovery:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}},
			std::move(Provenance), Request, OutError)) return false;
		Request.Lifetime = EImportOperationLifetime::SessionCritical;
		const FImportHandle Handle = GetImportService().SubmitImport(
			std::move(Request), std::format("Recover VolumeTexture {}", Destination.GetAssetName()));
		if (!Handle)
		{
			OutError = "VolumeTexture AssetForge recovery could not be submitted.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FAssetForgeBuiltinsAssetFeatures::PostLoadUncooked(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeFeature(Texture, OutError);
	}

}
