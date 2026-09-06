#pragma once

#include "Misc/MountPaths.h"
#include "Panels/ContentBrowserFilesystem.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Owns the mount/path view used by one operation; independent of navigation and UI.
	class FContentBrowserPaths
	{
	public:
		// Maps one virtual mount to its source and imported physical roots.
		struct FMountSnapshot
		{
			std::string VirtualRoot;
			std::string SourcePhysicalRoot;
			std::string PhysicalRoot;
			bool bContentWritable = false;
		};

		struct FMountPath
		{
			const FMountSnapshot* Mount = nullptr;
			std::string NormalizedPhysicalPath;
			std::string VirtualPath;

			explicit operator bool() const { return Mount != nullptr; }
		};

		FContentBrowserPaths() { RefreshMountSnapshot(); }
		auto RefreshMountSnapshot() -> void { MountSnapshot = CaptureMounts(); }
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
					ContentRoot,
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
		auto PhysicalToVirtualDirectory(
			std::string_view PhysicalPath) const -> std::string
		{
			const FMountPath Resolved = ResolveMountPath(PhysicalPath);
			if (!Resolved) return {};
			std::string Result = Resolved.VirtualPath;
			if (!Result.ends_with('/')) Result += '/';
			return Result;
		}

		auto ResolveMountPath(std::string_view PhysicalPath) const -> FMountPath
		{
			return ResolveMountPath(PhysicalPath, MountSnapshot);
		}

		static auto ResolveMountPath(std::string_view PhysicalPath,
			std::span<const FMountSnapshot> MountSnapshot) -> FMountPath
		{
			const FAssetPathResult Classified =
				FMountPaths::ClassifyAssetPath(PhysicalPath);
			if (!Classified) return {};
			const std::string ClassifiedRoot =
				ContentBrowserFilesystem::NormalizePath(Classified.Mount->GetContentDir().generic_string());
			const auto Mount = std::ranges::find_if(
				MountSnapshot,
				[&](const FMountSnapshot& Candidate) {
					return Candidate.VirtualRoot == Classified.Mount->VirtualRoot
						&& Candidate.PhysicalRoot == ClassifiedRoot;
				});
			if (Mount == MountSnapshot.end()) return {};
			return {
				.Mount = &*Mount,
				.NormalizedPhysicalPath = ContentBrowserFilesystem::NormalizePath(PhysicalPath),
				.VirtualPath = Classified.NormalizedVirtualPath};
		}

		static auto VirtualToPhysical(std::string_view VirtualPath)
			-> std::string
		{
			std::string EntryPath(VirtualPath);
			if (!EntryPath.ends_with('/')) EntryPath += '/';
			EntryPath += "_directory_";
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(EntryPath);
			return Resolved
				? ContentBrowserFilesystem::NormalizePath(Resolved.PhysicalPath.parent_path().generic_string())
				: std::string{};
		}


	private:
		std::vector<FMountSnapshot> MountSnapshot;
	};
}
