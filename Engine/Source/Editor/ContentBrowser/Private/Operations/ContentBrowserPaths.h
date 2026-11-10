#pragma once

#include "CoreMinimal.h"

#include "Misc/MountPaths.h"
#include "Panels/ContentBrowserFilesystem.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Projects browser paths through the current mount registry; owns no registry cache.
	class FContentBrowserPaths
	{
	public:
		// Value-owned directory-tree display data, captured only by the Model.
		struct FMountSnapshot
		{
			std::string VirtualRoot;
			std::string PhysicalRoot;
			bool bContentWritable = false;
		};

		struct FMountPath
		{
			const FMountPoint* Mount = nullptr;
			std::string NormalizedPhysicalPath;
			std::string VirtualPath;

			explicit operator bool() const { return Mount != nullptr; }
		};

		FContentBrowserPaths() = delete;
		static auto CaptureMounts() -> std::vector<FMountSnapshot>
		{
			const auto& RegisteredMounts = FMountPaths::GetRegisteredMountPoints();
			const size_t ContentMountCount = std::ranges::count_if(
				RegisteredMounts,
				[](const FMountPoint& Mount) {
					return Mount.bAutoScan;
				});
			std::vector<FMountSnapshot> NextMountSnapshot;
			NextMountSnapshot.reserve(ContentMountCount);
			for (const FMountPoint& Mount : RegisteredMounts)
			{
				if (!Mount.bAutoScan) continue;
				const std::string ContentRoot = Mount.GetContentDir().generic_string();
				NextMountSnapshot.push_back({
					Mount.VirtualRoot,
					ContentBrowserFilesystem::NormalizePath(ContentRoot),
					Mount.bContentWritable});
			}
			const auto GameMount = std::ranges::find(
				NextMountSnapshot, std::string_view{"/Game/"}, &FMountSnapshot::VirtualRoot);
			const auto EngineMount = std::ranges::find(
				NextMountSnapshot, std::string_view{"/Engine/"}, &FMountSnapshot::VirtualRoot);
			if (GameMount != NextMountSnapshot.end()
				&& EngineMount != NextMountSnapshot.end()
				&& EngineMount < GameMount)
				std::rotate(EngineMount, GameMount, std::next(GameMount));

			return NextMountSnapshot;
		}
		static auto PhysicalToVirtualDirectory(
			std::string_view PhysicalPath) -> std::string
		{
			const FMountPath Resolved = ResolveMountPath(PhysicalPath);
			if (!Resolved) return {};
			std::string Result = Resolved.VirtualPath;
			if (!Result.ends_with('/')) Result += '/';
			return Result;
		}

		static auto ResolveMountPath(std::string_view PhysicalPath) -> FMountPath
		{
			const auto Classified = FMountPaths::ClassifyAssetPath(PhysicalPath);
			if (!Classified || !Classified->Mount->bAutoScan) return {};
			return {
				.Mount = Classified->Mount,
				.NormalizedPhysicalPath = ContentBrowserFilesystem::NormalizePath(PhysicalPath),
				.VirtualPath = Classified->NormalizedVirtualPath};
		}

		static auto VirtualToPhysical(std::string_view VirtualPath) -> std::string
		{
			// Asset resolution accepts leaf paths. Handle the mount directory itself
			// explicitly instead of inventing a child that could exist on disk.
			if (VirtualPath.ends_with('/')) VirtualPath.remove_suffix(1);
			for (const auto& Mount : FMountPaths::GetRegisteredMountPoints())
			{
				const std::string_view Root(Mount.VirtualRoot);
				if (Root.size() != VirtualPath.size() + 1
					|| !std::ranges::equal(VirtualPath, Root.substr(0, Root.size() - 1),
						[](char A, char B) { return std::tolower(static_cast<unsigned char>(A)) == std::tolower(static_cast<unsigned char>(B)); })) continue;
				auto PhysicalRoot = Mount.GetContentDir();
				if (PhysicalRoot.filename().empty() && PhysicalRoot.has_relative_path())
					PhysicalRoot = PhysicalRoot.parent_path();
				const auto Resolved = ResolveMountPath(PhysicalRoot.generic_string());
				return Resolved ? Resolved.NormalizedPhysicalPath : std::string{};
			}
			const auto Resolved = FMountPaths::ResolveAssetPath(VirtualPath);
			return Resolved && Resolved->Mount->bAutoScan
				? ContentBrowserFilesystem::NormalizePath(Resolved->PhysicalPath.generic_string()) : std::string{};
		}
	};
}
