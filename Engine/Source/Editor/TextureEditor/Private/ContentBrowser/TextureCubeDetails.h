#pragma once

#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin::Editor::Texture
{
	// Inspects cube metadata without loading the asset; cached by file fingerprint and catalog revision.
	struct FTextureCubeDetailsSnapshot
	{
		bool bAvailable = false;
		bool bPanorama = false;
		std::string SourceLayout = "-";
		std::string Source = "-";
		std::string SourceSize = "-";
		std::string FaceOverride = "-";
		std::string InputRange = "-";
		std::string Exposure = "-";
		std::string Dimensions = "-";
		std::string Faces = "-";
		std::string Mips = "-";
		std::string Output = "-";
		std::string BuildDiagnostic = "Metadata is unavailable.";
		uint64 PackageHashLow = 0;
		uint64 PackageHashHigh = 0;
	};

	// Bounds metadata reads to one cached package and invalidates on catalog or file changes.
	class FTextureCubeDetailsCache
	{
	public:
		using FBuilder = std::function<FTextureCubeDetailsSnapshot(std::string_view)>;

		explicit FTextureCubeDetailsCache(FBuilder InBuilder = {});
		auto Get(std::string_view PhysicalPath, uint64 RegistryRevision)
			-> const FTextureCubeDetailsSnapshot&;
		auto Invalidate() -> void;

	private:
		FBuilder Builder;
		std::string CachedPhysicalPath;
		uint64 CachedRegistryRevision = std::numeric_limits<uint64>::max();
		uintmax_t CachedFileSize = 0;
		int64 CachedLastWriteTimeTicks = 0;
		bool bCachedFileStatValid = false;
		std::optional<FTextureCubeDetailsSnapshot> CachedSnapshot;
	};

	auto BuildTextureCubeDetailsSnapshot(std::string_view PhysicalPath)
		-> FTextureCubeDetailsSnapshot;

	// Each registration owns its cache; returned snapshots contain text only.
	auto MakeTextureCubeDetailsProvider() -> ContentBrowser::FDetailsProvider;
}
