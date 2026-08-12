#include "Texture/TextureBuildOperations.h"

#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Source/SourcePath.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/Texture2DDerivedData.h"

namespace Durin::AssetBuild
{
	namespace
	{
		constexpr uint64 TextureDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureDerivedDataCleanupDeleteLimit = 16;

		auto IsCanonicalTextureHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}

		auto StoreTexture2DDerivedData(
			std::string_view Key,
			const FTexturePlatformData& PlatformData,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTexturePlatformData&>(PlatformData).Serialize(Ar, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError())
			{
				OutError = Ar.GetError();
				return false;
			}
			Asset::FDerivedDataObjectStore Store(
				"Textures/Objects", MaximumTexturePayloadBytes);
			if (!Store.Write(Key, Bytes, &OutError)) return false;
			const Asset::FDerivedDataObjectCleanupResult Cleanup = Store.CleanupToBudget(
				TextureDerivedDataBudgetBytes, TextureDerivedDataCleanupDeleteLimit);
			if (!Cleanup.Message.empty())
				DURIN_WARN("Texture2D DDC cleanup: {}", Cleanup.Message);
			return true;
		}
	}

	auto BuildTexture2D(
		FTexture2DBuildRequest Request,
		FTexture2DBuildProduct& OutProduct,
		std::string& OutError,
		const FTexture2DBuildExecutionControl* ExecutionControl) -> bool
	{
		OutProduct = {};
		const FTexture2DBuildSettings& Settings = Request.Settings;
		if (!Request.SourceData.IsValid())
		{
			OutError = "Texture2D build requires valid normalized RGBA8 source data.";
			return false;
		}
		if (Request.SourceContentHashLow == 0 && Request.SourceContentHashHigh == 0)
		{
			OutError = "Texture2D build requires a captured source-content identity.";
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
		FTexturePlatformData PlatformData;
		TextureBuilder::FBuildMipChainMetrics BuilderMetrics;
		const TextureBuilder::FBuildExecutionControl BuilderControl{
			.ShouldCancel = ExecutionControl ? ExecutionControl->ShouldCancel : nullptr,
			.Metrics = ExecutionControl && ExecutionControl->Metrics ? &BuilderMetrics : nullptr};
		if (!TextureBuilder::BuildMipChain(
			Request.SourceData,
			Settings.Usage,
			bSRGB,
			PlatformData,
			OutError,
			Settings.MaxResolution,
			Settings.CompressionQuality,
			Settings.AlphaMipMode,
			Settings.AlphaCoverageThreshold,
			ExecutionControl ? &BuilderControl : nullptr)) return false;
		if (ExecutionControl && ExecutionControl->Metrics)
		{
			ExecutionControl->Metrics->MipGenerationNanoseconds =
				BuilderMetrics.MipGenerationNanoseconds;
			ExecutionControl->Metrics->CompressionNanoseconds =
				BuilderMetrics.CompressionNanoseconds;
			ExecutionControl->Metrics->PeakIntermediateBytes =
				BuilderMetrics.PeakIntermediateBytes;
		}
		if (ExecutionControl && ExecutionControl->ShouldCancel
			&& ExecutionControl->ShouldCancel())
		{
			OutError = "Texture2D build was cancelled.";
			return false;
		}

		const FXxHash128 SourceHash{
			.HashLow = Request.SourceContentHashLow,
			.HashHigh = Request.SourceContentHashHigh};
		const std::string Key = BuildTexture2DDerivedDataKey({
			.SourceContentHash = SourceHash,
			.Usage = Settings.Usage,
			.bSRGB = bSRGB,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.MaximumResolution = Settings.MaxResolution,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Request.bPersistDerivedData)
		{
			if (ExecutionControl && ExecutionControl->OnPersisting)
				ExecutionControl->OnPersisting();
			const auto PersistenceStart = std::chrono::steady_clock::now();
			if (!StoreTexture2DDerivedData(Key, PlatformData, OutError)) return false;
			if (ExecutionControl && ExecutionControl->Metrics)
			{
				ExecutionControl->Metrics->PersistenceNanoseconds =
					static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - PersistenceStart).count());
			}
		}

		OutProduct = {
			.SourceData = std::move(Request.SourceData),
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = Key,
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
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				Context.SourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Dependency =
			PathUtilities::CheckMountDependency(
				Texture.GetPackage()->GetPackagePath(), Resolved.NormalizedVirtualPath);
		if (!Dependency)
		{
			OutError = Dependency.Message;
			return false;
		}

		const FXxHash128 SourceHash{
			.HashLow = Product.SourceContentHashLow,
			.HashHigh = Product.SourceContentHashHigh};
		return Texture.PublishImportedState({
			.SourceImportData = {
				.Source = {
					.SourcePath = {.Path = Resolved.NormalizedVirtualPath},
					.SourceContentHashLow = SourceHash.HashLow,
					.SourceContentHashHigh = SourceHash.HashHigh},
				.DecoderId = Context.DecoderId,
				.DecoderVersion = Context.DecoderVersion},
			.SourceContentHash = SourceHash.ToString(),
			.SourceFileSize = Context.SourceFileSize,
			.SourceLastWriteTime = Context.SourceLastWriteTime,
			.SourceData = std::make_unique<FTextureSourceData>(std::move(Product.SourceData)),
			.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData)),
			.DerivedDataKey = std::move(Product.DerivedDataKey),
			.Usage = Product.Settings.Usage,
			.bSRGB = Product.bSRGB,
			.MaxResolution = Product.Settings.MaxResolution,
			.CompressionQuality = Product.Settings.CompressionQuality,
			.AlphaMipMode = Product.Settings.AlphaMipMode,
			.AlphaCoverageThreshold = Product.Settings.AlphaCoverageThreshold,
			.bMarkPackageDirty = Context.bMarkPackageDirty,
			.bReportLoadMutation = Context.bReportLoadMutation}, OutError);
	}

	auto MakeTexture2DDerivedDataKey(
		const DTexture2D& Texture,
		std::string& OutKey,
		std::string& OutError) -> bool
	{
		FXxHash128 SourceHash;
		if (Texture.GetSourceImportData().Source.HasContentHash())
		{
			const FTextureSourceFile& Source = Texture.GetSourceImportData().Source;
			SourceHash.HashLow = Source.SourceContentHashLow;
			SourceHash.HashHigh = Source.SourceContentHashHigh;
		}
		else if (IsCanonicalTextureHash(Texture.GetSourceContentHash()))
			SourceHash = FXxHash128::FromString(Texture.GetSourceContentHash());
		else
		{
			OutError = "Texture source content hash is missing or invalid.";
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
		std::vector<uint8> Bytes;
		const Asset::FDerivedDataObjectReadResult Read = Asset::FDerivedDataObjectStore(
			"Textures/Objects", MaximumTexturePayloadBytes).Read(Key, Bytes);
		if (!Read)
		{
			OutStatus = Read.Status == Asset::EDerivedDataObjectReadStatus::Missing
				? ETextureDerivedDataStatus::Missing
				: ETextureDerivedDataStatus::Corrupt;
			OutMessage = Read.Message;
			return false;
		}
		auto Candidate = std::make_unique<FTexturePlatformData>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
		{
			OutStatus = Ar.GetFailure()
				&& Ar.GetFailure()->Code == EArchiveFailureCode::UnsupportedVersion
				? ETextureDerivedDataStatus::Incompatible
				: ETextureDerivedDataStatus::Corrupt;
			OutMessage = Ar.GetError();
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutStatus = ETextureDerivedDataStatus::Hit;
		OutMessage.clear();
		return true;
	}
}
