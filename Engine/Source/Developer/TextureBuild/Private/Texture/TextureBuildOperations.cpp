#include "Texture/TextureBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Asset/Asset.h"
#include "Hash/XxHash.h"
#include "Texture/Texture2DDerivedData.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildFunctions.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	using namespace ::Durin::DerivedData;

	auto BuildTexture2D(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		if (!EnsureTextureBuildFunctions(&OutError)) return false;
		const FTexture2DBuildSettings& Settings = Request.Settings;
		if (!Request.SourceData.IsValid())
		{
			OutError = "Texture2D build requires valid normalized RGBA8 source data.";
			return false;
		}
		if (!ValidateTexture2DBuildSettings(Settings, OutError)) return false;
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			OutError = "Texture2D build target is unsupported.";
			return false;
		}

		const bool bSRGB = ResolveTexture2DSRGB(Settings);
		const FXxHash128 SourceHash = Request.SourceData.GetImportedDataIdentity();
		const FTexture2DBuildKeyInput KeyInput{
			.ImportedDataIdentity = SourceHash,
			.Usage = Settings.Usage,
			.bSRGB = bSRGB,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.MaximumResolution = Settings.MaxResolution,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile};
		const FByteArray KeyBytes = BuildTexture2DDerivedDataKeyBytes(KeyInput);
		const std::string Key = BuildTexture2DDerivedDataKey(KeyInput);
		FBuildDefinition Definition;
		FBuildDefinitionBuilder DefinitionBuilder(
			AssetPrivate::Texture2DFunctionName, std::string(AssetPrivate::Texture2DValueName));
		DefinitionBuilder.SetKey(FBuildKey::FromString(Key), KeyBytes)
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game")
			.AddInput(FBuildValue::FromOwned(std::string(AssetPrivate::Texture2DInputName),
				AssetPrivate::EncodeTexture2DLocalInput(Request, bSRGB)));
		if (!DefinitionBuilder.Build(Definition, &OutError)) return false;
		const FBuildCancellationToken Cancellation(
			ExecutionControl ? ExecutionControl->ShouldCancel : std::function<bool()>{});
		if (Request.bPersistDerivedData && ExecutionControl && ExecutionControl->OnPersisting)
			ExecutionControl->OnPersisting();
		const auto PersistenceStart = std::chrono::steady_clock::now();
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true,
			.bAllowLocalBuild = true,
			.bStoreBuildResult = Request.bPersistDerivedData},
			ExecutionControl ? &Cancellation : nullptr);
		if (ExecutionControl && ExecutionControl->Metrics && Request.bPersistDerivedData)
		{
			ExecutionControl->Metrics->PersistenceNanoseconds =
				static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - PersistenceStart).count());
		}
		if (!Output.Succeeded())
		{
			OutError = Output.Diagnostic;
			return false;
		}
		FTexturePlatformData PlatformData;
		if (!AssetPrivate::DecodeTexture2DPlatformValue(Output.Value, PlatformData, OutError))
			return false;
		if (ExecutionControl && ExecutionControl->Metrics)
			ExecutionControl->Metrics->PeakIntermediateBytes = std::max<uint64>(
				Request.SourceData.Pixels.size(), Output.Value.GetSize());

		OutProduct = {
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = Key,
			.PersistenceDiagnostic = Output.StoreDiagnostic,
			.Origin = Output.Status == EBuildStatus::CacheHit
				? ETexture2DBuildProductOrigin::CacheHit
				: ETexture2DBuildProductOrigin::Rebuilt};
		OutError.clear();
		return true;
	}

}
