#include "Texture2DPostLoad.h"

#include "Hash/XxHash.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "SourceFingerprintCache.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::StandardAssetImport
{
	namespace
	{
		bool GTexture2DPostLoadPolicyRegistered = false;

		auto IsCanonicalTextureHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}

		auto FailLoad(
			DTexture2D& Texture,
			ETextureDerivedDataStatus DerivedDataStatus,
			std::string Message,
			std::string& OutError,
			std::string DerivedDataKey = {}) -> bool
		{
			OutError = Message;
			return Texture.PublishUncookedLoadFailure(
				DerivedDataStatus,
				ETextureBuildStatus::MissingSource,
				std::move(Message),
				std::move(DerivedDataKey));
		}

		auto SubmitLoadBuild(
			DTexture2D& Texture,
			std::string_view PhysicalPath,
			uint64 SourceFileSize,
			int64 SourceLastWriteTime,
			bool bMetadataChanged,
			std::string& OutError) -> bool
		{
			std::vector<uint8> EncodedBytes;
			if (!FFileHelper::LoadFileToArray(EncodedBytes, PhysicalPath))
			{
				OutError = std::format("Failed to read texture source file: {}", PhysicalPath);
				return Texture.PublishUncookedLoadFailure(
					ETextureDerivedDataStatus::SourceUnavailable,
					ETextureBuildStatus::MissingSource,
					OutError);
			}
			FTextureSourceData SourceData;
			if (!TranslateTexture2DSource(EncodedBytes, SourceData, OutError))
				return Texture.PublishUncookedLoadFailure(
					ETextureDerivedDataStatus::Corrupt,
					ETextureBuildStatus::DecodeFailure,
					OutError);
			const FXxHash128 SourceHash = FXxHash128::HashBuffer(EncodedBytes);
			Asset::StoreSourceFingerprint(std::filesystem::path(PhysicalPath), {
				.FileSize = SourceFileSize,
				.LastWriteTimeTicks = SourceLastWriteTime,
				.ContentHash = SourceHash.ToString()});
			return AssetBuild::SubmitTexture2DBuild(Texture, {
				.SourceData = std::move(SourceData),
				.SourceContentHashLow = SourceHash.HashLow,
				.SourceContentHashHigh = SourceHash.HashHigh,
				.SourcePath = Texture.GetSourceImportData().Source.SourcePath,
				.Settings = MakeTexture2DBuildSettings(Texture),
				.DecoderId = "DurinImage",
				.DecoderVersion = 1,
				.SourceFileSize = SourceFileSize,
				.SourceLastWriteTime = SourceLastWriteTime,
				.Priority = AssetBuild::ETexture2DBuildPriority::Background,
				.bPersistDerivedData = true,
				.bMarkPackageDirty = bMetadataChanged,
				.bReportLoadMutation = bMetadataChanged}, OutError);
		}

		auto PostLoadTexture2D(DTexture2D& Texture, std::string& OutError) -> bool
		{
			if (!Texture.GetSourceImportData().HasSource())
				return FailLoad(
					Texture,
					ETextureDerivedDataStatus::SourceUnavailable,
					"Texture asset has no source file.",
					OutError);

			const FTextureSourceDiagnostic Source = Texture.InspectSource();
			if (Source.Status == ETextureSourceStatus::Invalid)
				return FailLoad(
					Texture,
					ETextureDerivedDataStatus::Incompatible,
					Source.Message,
					OutError);
			const bool bSourceAvailable = Source.Status != ETextureSourceStatus::Missing;
			const bool bSourceContentMatches = Source.Status == ETextureSourceStatus::Available;
			uint64 CurrentFileSize = 0;
			int64 CurrentLastWriteTime = 0;
			if (bSourceAvailable)
			{
				std::error_code Error;
				const std::filesystem::path PhysicalPath(Source.PhysicalPath);
				CurrentFileSize = std::filesystem::file_size(PhysicalPath, Error);
				const std::filesystem::file_time_type LastWriteTime =
					std::filesystem::last_write_time(PhysicalPath, Error);
				if (Error)
					return FailLoad(
						Texture,
						ETextureDerivedDataStatus::SourceUnavailable,
						std::format("Failed to inspect texture source file: {}", Error.message()),
						OutError);
				CurrentLastWriteTime = DerivedDataCache::FileTimeToStableTicks(LastWriteTime);
				if (bSourceContentMatches)
				{
					Texture.PublishSourceFingerprint(CurrentFileSize, CurrentLastWriteTime);
					const FTextureSourceFile& PersistedSource =
						Texture.GetSourceImportData().Source;
					const std::string PersistedHash = PersistedSource.HasContentHash()
						? FXxHash128{
							.HashLow = PersistedSource.SourceContentHashLow,
							.HashHigh = PersistedSource.SourceContentHashHigh}.ToString()
						: Texture.GetSourceContentHash();
					if (IsCanonicalTextureHash(PersistedHash))
						Asset::StoreSourceFingerprint(PhysicalPath, {
							.FileSize = CurrentFileSize,
							.LastWriteTimeTicks = CurrentLastWriteTime,
							.ContentHash = PersistedHash});
				}
			}

			const bool bHasPersistedIdentity =
				Texture.GetSourceImportData().Source.HasContentHash()
				|| IsCanonicalTextureHash(Texture.GetSourceContentHash());
			if (bHasPersistedIdentity && (!bSourceAvailable || bSourceContentMatches))
			{
				std::string Key;
				if (!AssetBuild::MakeTexture2DDerivedDataKey(Texture, Key, OutError))
					return FailLoad(
						Texture,
						ETextureDerivedDataStatus::Incompatible,
						OutError,
						OutError);
				std::unique_ptr<FTexturePlatformData> PlatformData;
				ETextureDerivedDataStatus CacheStatus = ETextureDerivedDataStatus::Missing;
				std::string CacheMessage;
				if (AssetBuild::LoadTexture2DDerivedData(
						Key, PlatformData, CacheStatus, CacheMessage))
					return Texture.PublishDerivedDataLoad(
						std::move(PlatformData), std::move(Key), bSourceAvailable, OutError);
				if (!bSourceAvailable)
					return FailLoad(
						Texture,
						ETextureDerivedDataStatus::SourceUnavailable,
						std::format(
							"Texture source file does not exist: {}. Cached payload was unavailable: {}",
							Texture.GetSourceFile(), CacheMessage),
						OutError,
						std::move(Key));
			}
			else if (!bSourceAvailable)
				return FailLoad(
					Texture,
					ETextureDerivedDataStatus::SourceUnavailable,
					std::format("Texture source file does not exist: {}", Texture.GetSourceFile()),
					OutError);

			const bool bMetadataChanged = !bHasPersistedIdentity || !bSourceContentMatches;
			return SubmitLoadBuild(
				Texture,
				Source.PhysicalPath,
				CurrentFileSize,
				CurrentLastWriteTime,
				bMetadataChanged,
				OutError);
		}
	}

	auto RegisterTexture2DPostLoadPolicy() -> bool
	{
		if (GTexture2DPostLoadPolicyRegistered) return true;
		GTexture2DPostLoadPolicyRegistered =
			RegisterTexture2DUncookedPostLoadHandler(PostLoadTexture2D);
		return GTexture2DPostLoadPolicyRegistered;
	}

	auto UnregisterTexture2DPostLoadPolicy() -> void
	{
		if (!GTexture2DPostLoadPolicyRegistered) return;
		UnregisterTexture2DUncookedPostLoadHandler();
		GTexture2DPostLoadPolicyRegistered = false;
	}
}
