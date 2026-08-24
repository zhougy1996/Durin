#include "BuiltinImportProviderCommon.h"

namespace Durin::AssetForge::Builtins
{
	auto FAssetForgeBuiltinsAuthoringFeatures::BuildFileProduct(
		DStaticMesh& Mesh,
		std::string_view SourcePath,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceContentHash,
		FStaticMeshAuthoringProduct& OutProduct,
		std::string& OutError) -> bool
	{
		return BuildStaticMeshFileProduct(
			Mesh, SourcePath, std::move(SourceImportData), SourceContentHash, OutProduct, OutError);
	}

	auto FAssetForgeBuiltinsAuthoringFeatures::PostLoadUncooked(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		return PostLoadStaticMesh(Mesh, OutDiagnostic, OutError);
	}

	auto FAssetForgeBuiltinsAuthoringFeatures::ChangeSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return ChangeStaticMeshSourceReference(Mesh, SourceVirtualPath, OutError);
	}

	auto FAssetForgeBuiltinsAuthoringFeatures::PostLoadUncooked(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return PostLoadTexture2DFeature(Texture, OutError);
	}

	auto FAssetForgeBuiltinsAuthoringFeatures::WaitForRecovery(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		return WaitForTexture2DImportRecovery(Texture, TimeoutSeconds);
	}

	auto FAssetForgeBuiltinsAuthoringFeatures::RecoverUncooked(
		DVolumeTexture& Texture, std::string& OutError) -> bool
	{
		const std::string Key = Asset::Build::MakeVolumeTextureDerivedDataKey(Texture, OutError);
		if (Key.empty()) return false;
		std::unique_ptr<FVolumeTexturePlatformData> Cached;
		ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
		std::string Message;
		if (Asset::Build::LoadVolumeTextureDerivedData(Key, Cached, Status, Message))
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

	auto FAssetForgeBuiltinsAuthoringFeatures::PostLoadUncooked(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeFeature(Texture, OutError);
	}

}
