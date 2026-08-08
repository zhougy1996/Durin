#include "Thumbnail/Texture2DAssetThumbnail.h"

#include "AssetSystem.h"
#include "ImageDecoder.h"
#include "Misc/Paths.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	namespace
	{
		auto FindImageWithStem(const std::filesystem::path& PathWithoutExtension)
			-> std::filesystem::path
		{
			std::error_code Error;
			for (std::filesystem::directory_iterator It(
					 PathWithoutExtension.parent_path(),
					 std::filesystem::directory_options::skip_permission_denied, Error),
				 End;
				 !Error && It != End;
				 It.increment(Error))
			{
				if (!It->is_regular_file(Error)
					|| It->path().stem() != PathWithoutExtension.filename())
					continue;
				if (Asset::IsSupportedImageExtension(
						It->path().extension().generic_string()))
					return It->path();
			}
			return {};
		}

		auto FindTextureSourceFile(const Asset::FAssetData& Data)
			-> std::filesystem::path
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Data.PackagePath.GetView());
			if (!Lookup || !Lookup.Mount->bAutoScan) return {};
			const PathUtilities::FMountPoint& Mount = *Lookup.Mount;

			Asset::FAssetPackageInspection Inspection;
			if (Asset::InspectAssetPackage(Data.PhysicalPath, Inspection))
			{
				const Asset::FAssetPackageField* SourceField =
					Inspection.FindField("SourceImportData");
				FTexture2DSourceImportData SourceImportData;
				if (SourceField
					&& SourceField->TryReadStruct(
						FTexture2DSourceImportData::StaticStruct(), &SourceImportData)
					&& SourceImportData.HasSource())
				{
					const PathUtilities::FSourcePathResult Resolved =
						PathUtilities::ResolveSourcePath(
							SourceImportData.Source.SourcePath.Path);
					if (Resolved
						&& Asset::IsSupportedImageExtension(
							Resolved.PhysicalPath.extension().generic_string()))
						return Resolved.PhysicalPath;
				}
			}

			const std::filesystem::path ContentDir = Mount.GetContentDir();
			const std::filesystem::path SourceRoot = ContentDir / "Textures";
			if (const std::filesystem::path Direct = FindImageWithStem(
					SourceRoot / std::string(Data.PackagePath.GetAssetName()));
				!Direct.empty())
				return Direct;
			std::filesystem::path RelativePackage =
				std::filesystem::path(Data.PhysicalPath).lexically_relative(ContentDir);
			RelativePackage.replace_extension();
			return FindImageWithStem(SourceRoot / RelativePackage);
		}
	} // namespace

	auto FTexture2DAssetThumbnailProvider::GetRegistration() const
		-> FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
			.ProviderName = "Texture2DSourceThumbnail",
			.GeneratorSchemaVersion = 1};
	}

	auto FTexture2DAssetThumbnailProvider::CaptureGenerationRequest(
		const FAssetThumbnailRequest&,
		uint64,
		FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError = "Texture2D thumbnails use the source-image thumbnail path.";
		return false;
	}

	auto FTexture2DAssetThumbnailProvider::CaptureSourceImage(
		const Asset::FAssetData& AssetData,
		FAssetThumbnailSourceImage& OutSource,
		std::string& OutError) -> bool
	{
		OutSource = {};
		OutError.clear();
		if (AssetData.AssetClassName != GetRegistration().AssetClassName)
		{
			OutError = "The Texture2D thumbnail provider received the wrong asset class.";
			return false;
		}
		const std::filesystem::path SourcePath = FindTextureSourceFile(AssetData);
		if (SourcePath.empty()) return false;
		std::error_code Error;
		OutSource.PhysicalPath = std::filesystem::absolute(SourcePath)
			.lexically_normal().generic_string();
		OutSource.FileSize = std::filesystem::file_size(SourcePath, Error);
		if (Error)
		{
			OutSource = {};
			return false;
		}
		OutSource.LastWriteTime = std::filesystem::last_write_time(SourcePath, Error);
		if (Error)
		{
			OutSource = {};
			return false;
		}
		return true;
	}
} // namespace Durin
