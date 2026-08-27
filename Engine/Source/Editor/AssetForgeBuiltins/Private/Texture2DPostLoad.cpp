#include "Texture2DPostLoad.h"
#include "DObject/Package.h"

#include "Hash/XxHash.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
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
			return RecoverTexture2DDerivedData(Texture, OutError);
		}

		auto PostLoadTexture2DImpl(DTexture2D& Texture, std::string& OutError) -> bool
		{
			if (Texture.GetSourceFile().empty())
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
			}

			const bool bHasPersistedIdentity = Texture.GetImportedSource() != nullptr;
			if (bHasPersistedIdentity && (!bSourceAvailable || bSourceContentMatches))
			{
				std::string Key;
				if (!Asset::MakeTexture2DDerivedDataKey(Texture, Key, OutError))
					return FailLoad(
						Texture,
						ETextureDerivedDataStatus::Incompatible,
						OutError,
						OutError);
				std::unique_ptr<FTexturePlatformData> PlatformData;
				ETextureDerivedDataStatus CacheStatus = ETextureDerivedDataStatus::Missing;
				std::string CacheMessage;
				if (Asset::LoadTexture2DDerivedData(
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
}
