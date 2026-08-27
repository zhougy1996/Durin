#include "Texture/TextureBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Asset.h"
#include "Hash/XxHash.h"
#include "Texture/Texture2DDerivedData.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildFunctions.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::Asset
{
	using namespace ::Durin::DerivedData;

	auto BuildTexture2D(
		FTexture2DBuildRequest Request,
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
		if (!TextureBuilder::IsValidUsage(Settings.Usage)
			|| !TextureBuilder::IsValidCompressionQuality(Settings.CompressionQuality)
			|| !TextureBuilder::IsValidAlphaMipMode(Settings.AlphaMipMode)
			|| !TextureBuilder::IsValidAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D build settings are invalid.";
			return false;
		}

		const bool bSRGB = Settings.bSRGB.value_or(
			TextureBuilder::GetDefaultSRGB(Settings.Usage));
		const FXxHash128 SourceHash = Request.SourceData.GetImportedDataIdentity();
		const FTexture2DBuildKeyInput KeyInput{
			.SourceContentHash = SourceHash,
			.Usage = Settings.Usage,
			.bSRGB = bSRGB,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.MaximumResolution = Settings.MaxResolution,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		const std::vector<std::byte> KeyBytes = BuildTexture2DDerivedDataKeyBytes(KeyInput);
		const std::string Key = BuildTexture2DDerivedDataKey(KeyInput);
		FBuildDefinition Definition;
		FBuildDefinitionBuilder DefinitionBuilder(
			Private::Texture2DFunctionIdentity, std::string(Private::Texture2DValueName));
		DefinitionBuilder.SetKey(FBuildKey::FromString(Key), KeyBytes)
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game")
			.AddInput(FBuildValue::FromOwned(std::string(Private::Texture2DInputName),
				Private::EncodeTexture2DLocalInput(Request, bSRGB)));
		if (!DefinitionBuilder.Build(Definition, &OutError)) return false;
		const FBuildCancellationToken Cancellation(
			ExecutionControl ? ExecutionControl->ShouldCancel : std::function<bool()>{});
		if (Request.bPersistDerivedData && ExecutionControl && ExecutionControl->OnPersisting)
			ExecutionControl->OnPersisting();
		const auto PersistenceStart = std::chrono::steady_clock::now();
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true,
			.bAllowLocalBuild = true,
			.bStoreBuildResult = Request.bPersistDerivedData,
			.bRequireStoreSuccess = false,
			.bReturnData = true}, ExecutionControl ? &Cancellation : nullptr);
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
		if (!Private::DecodeTexture2DPlatformValue(Output.Value, PlatformData, OutError))
			return false;
		if (ExecutionControl && ExecutionControl->Metrics)
			ExecutionControl->Metrics->PeakIntermediateBytes = std::max<uint64>(
				Request.SourceData.Pixels.size(), Output.Value.GetSize());

		OutProduct = {
			.SourceData = std::move(Request.SourceData),
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = Key,
			.PersistenceDiagnostic = Output.StoreDiagnostic,
			.SourceContentHashLow = SourceHash.HashLow,
			.SourceContentHashHigh = SourceHash.HashHigh,
			.Settings = Settings,
			.bSRGB = bSRGB};
		OutError.clear();
		return true;
	}

	auto PublishTexture2DProduct(
		DTexture2D& Texture,
		FTexture2DBuildProduct Product,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Texture2D product publication requires a package.";
			return false;
		}
		if (!Product.SourceData.IsValid() || !Product.PlatformData.IsValid()
			|| Product.DerivedDataKey.empty())
		{
			OutError = "Texture2D product publication requires a complete detached product.";
			return false;
		}
		return Texture.PublishImportedState({
			.SourceData = std::make_unique<FTextureSourceData>(std::move(Product.SourceData)),
			.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData)),
			.DerivedDataKey = std::move(Product.DerivedDataKey),
			.BuildDiagnostic = std::move(Product.PersistenceDiagnostic),
			.Usage = Product.Settings.Usage,
			.bSRGB = Product.bSRGB,
			.MaxResolution = Product.Settings.MaxResolution,
			.CompressionQuality = Product.Settings.CompressionQuality,
			.AlphaMipMode = Product.Settings.AlphaMipMode,
			.AlphaCoverageThreshold = Product.Settings.AlphaCoverageThreshold,
			.bMarkPackageDirty = Context.bMarkPackageDirty,
			.bReportLoadMutation = Context.bReportLoadMutation,
			.bSourceDecoderInvoked = Context.bSourceDecoderInvoked}, OutError);
	}

	auto MakeTexture2DDerivedDataKey(
		const DTexture2D& Texture,
		std::string& OutKey,
		std::string& OutError) -> bool
	{
		const FXxHash128 SourceHash = Texture.GetImportedDataIdentity();
		if (SourceHash.IsZero())
		{
			OutError = "Texture canonical imported-data identity is missing or invalid.";
			return false;
		}
		OutKey = BuildTexture2DDerivedDataKey({
			.SourceContentHash = SourceHash,
			.Usage = Texture.GetUsage(),
			.bSRGB = Texture.IsSRGB(),
			.CompressionQuality = Texture.GetCompressionQuality(),
			.AlphaMipMode = Texture.GetAlphaMipMode(),
			.MaximumResolution = Texture.GetMaxResolution(),
			.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		OutError.clear();
		return true;
	}

	auto LoadTexture2DDerivedData(
		std::string_view Key,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool
	{
		std::string Error;
		if (!EnsureTextureBuildFunctions(&Error))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			OutMessage = Error;
			return false;
		}
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(
			Private::Texture2DFunctionIdentity, std::string(Private::Texture2DValueName));
		Builder.SetKey(FBuildKey::FromString(Key))
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game");
		if (!Builder.Build(Definition, &Error))
		{
			OutStatus = ETextureDerivedDataStatus::Incompatible;
			OutMessage = Error;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false, .bReturnData = true});
		if (!Output.Succeeded())
		{
			OutStatus = Output.Status == EBuildStatus::CacheMiss
				? ETextureDerivedDataStatus::Missing : ETextureDerivedDataStatus::Corrupt;
			OutMessage = Output.Diagnostic;
			return false;
		}
		auto Candidate = std::make_unique<FTexturePlatformData>();
		if (!Private::DecodeTexture2DPlatformValue(Output.Value, *Candidate, OutMessage))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutStatus = ETextureDerivedDataStatus::Hit;
		OutMessage.clear();
		return true;
	}
}
