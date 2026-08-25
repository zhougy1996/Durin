#include "Texture2DPostLoad.h"
#include "Texture2DBuildAdapter.h"
#include "DObject/Package.h"

#include "Hash/XxHash.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/ImportService.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		std::mutex GRecoveryMutex;
		std::unordered_map<std::string, FImportHandle> GRecoveries;

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
			std::string_view,
			uint64,
			int64,
			bool,
			std::string& OutError) -> bool
		{
			FAssetPath Destination;
			if (!Texture.GetPackage()
				|| !FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
				return false;
			FImportProvenance Existing;
			std::optional<FImportProvenance> Provenance;
			if (InspectTexture2DImportProvenance(Texture, Existing, OutError))
				Provenance = std::move(Existing);
			else OutError.clear();
			FImportRequest Request;
			const FTexture2DImportSettings Settings{
				.Usage = Texture.GetUsage(),
				.CompressionQuality = Texture.GetCompressionQuality(),
				.AlphaMipMode = Texture.GetAlphaMipMode(),
				.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
				.MaxResolution = Texture.GetMaxResolution(),
				.bSRGB = Texture.IsSRGB()};
			if (!MakeTexture2DImportRequest(
				Texture.GetSourceImportData().Source.SourcePath, Destination,
				Settings, EImportMode::Recover,
				{.OwnerId = std::format("Texture2D.Recovery:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Provenance), Request, OutError)) return false;
			Request.Lifetime = EImportOperationLifetime::SessionCritical;
			const FImportHandle Handle = GetImportService().SubmitImport(
				std::move(Request), std::format("Recover Texture2D {}", Destination.GetAssetName()));
			if (!Handle)
			{
				OutError = "Texture2D AssetForge recovery could not be submitted.";
				return false;
			}
			{
				std::lock_guard Lock(GRecoveryMutex);
				GRecoveries.insert_or_assign(Texture.GetObjectPath(), Handle);
			}
			OutError.clear();
			return true;
		}

		auto PostLoadTexture2DImpl(DTexture2D& Texture, std::string& OutError) -> bool
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
				CurrentLastWriteTime = FileTime::ToStableTicks(LastWriteTime);
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
				}
			}

			const bool bHasPersistedIdentity =
				Texture.GetSourceImportData().Source.HasContentHash()
				|| IsCanonicalTextureHash(Texture.GetSourceContentHash());
			if (bHasPersistedIdentity && (!bSourceAvailable || bSourceContentMatches))
			{
				std::string Key;
				if (!Asset::Build::MakeTexture2DDerivedDataKey(Texture, Key, OutError))
					return FailLoad(
						Texture,
						ETextureDerivedDataStatus::Incompatible,
						OutError,
						OutError);
				std::unique_ptr<FTexturePlatformData> PlatformData;
				ETextureDerivedDataStatus CacheStatus = ETextureDerivedDataStatus::Missing;
				std::string CacheMessage;
				if (Asset::Build::LoadTexture2DDerivedData(
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

	auto PostLoadTexture2DFeature(DTexture2D& Texture, std::string& OutError) -> bool
	{
		return PostLoadTexture2DImpl(Texture, OutError);
	}

	auto WaitForTexture2DImportRecovery(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		FImportHandle Handle;
		{
			std::lock_guard Lock(GRecoveryMutex);
			const auto Found = GRecoveries.find(Texture.GetObjectPath());
			if (Found == GRecoveries.end())
				return Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
			Handle = Found->second;
		}
		const auto Deadline = std::chrono::steady_clock::now()
			+ std::chrono::duration<double>(TimeoutSeconds);
		FImportResult Result;
		while (!Handle.TryGetResult(Result) && std::chrono::steady_clock::now() < Deadline)
		{
			(void)GetImportService().PumpImportOperations();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!Handle.TryGetResult(Result)) return false;
		{
			std::lock_guard Lock(GRecoveryMutex);
			GRecoveries.erase(Texture.GetObjectPath());
		}
		return Result.Outcome.State == EImportOperationState::Succeeded
			&& Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
	}
}
