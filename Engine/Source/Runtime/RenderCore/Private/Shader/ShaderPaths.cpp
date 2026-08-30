#include "Shader/ShaderPaths.h"

#include "Misc/Paths.h"
#include "Misc/StringConvert.h"
#include "Hash/XxHash.h"
#include "Threading/RunnableThread.h"

namespace Durin::FShaderPaths
{
	namespace
	{
		auto NormalizeVirtualRoot(std::string_view VirtualRoot) -> std::string
		{
			std::string Result(VirtualRoot);
			std::ranges::replace(Result, '\\', '/');
			while (!Result.empty() && Result.back() == '/')
			{
				Result.pop_back();
			}

			if (Result.empty())
			{
				return "/";
			}
			if (Result.front() != '/')
			{
				Result.insert(Result.begin(), '/');
			}

			Result.push_back('/');
			return Result;
		}

		auto MakeDefaultCacheDirectory(std::string_view VirtualRoot) -> std::string
		{
			const std::string NormalizedVirtualRoot = NormalizeVirtualRoot(VirtualRoot);
			std::string ReadableName = StringUtils::SanitizeFileName(NormalizedVirtualRoot, "ShaderMount");
			while (!ReadableName.empty() && ReadableName.front() == '_')
			{
				ReadableName.erase(ReadableName.begin());
			}
			while (!ReadableName.empty() && ReadableName.back() == '_')
			{
				ReadableName.pop_back();
			}
			if (ReadableName.empty())
			{
				ReadableName = "Root";
			}

			const FXxHash128 RootHash = FXxHash128::HashBuffer(NormalizedVirtualRoot);
			const std::string Namespace = ReadableName + "." + RootHash.ToString();
			return (std::filesystem::path(FPaths::DerivedDataCacheDir()) / "Shaders" / "SPIR-V" / Namespace).generic_string();
		}

		class FShaderMountRegistry
		{
		public:
			auto GetMountPoints() const -> const std::vector<FShaderMountPoint>&
			{
				return MountPoints;
			}

			auto IsFrozen() const -> bool
			{
				return bFrozen;
			}

			auto AddOrUpdateMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view CacheDir) -> void
			{
				checkf(!bFrozen, "Shader mount points are immutable after finalization.");

				const std::string NormalizedVirtualRoot = NormalizeVirtualRoot(VirtualRoot);
				const std::string NormalizedSourceDir = NormalizeDirectory(SourceDir);
				const std::string NormalizedCacheDir = NormalizeDirectory(CacheDir);

				const auto FoundIt = std::ranges::find_if(MountPoints, [&NormalizedVirtualRoot](const FShaderMountPoint& MountPoint) {
					return MountPoint.VirtualRoot == NormalizedVirtualRoot;
				});

				if (FoundIt != MountPoints.end())
				{
					FoundIt->SourceDir = NormalizedSourceDir;
					FoundIt->CacheDir = NormalizedCacheDir;
					DURIN_DEBUG("Shader mount point updated: {} -> {} (cache: {})", NormalizedVirtualRoot, NormalizedSourceDir, NormalizedCacheDir);
				}
				else
				{
					MountPoints.push_back({NormalizedVirtualRoot, NormalizedSourceDir, NormalizedCacheDir});
					DURIN_DEBUG("Shader mount point: {} -> {} (cache: {})", NormalizedVirtualRoot, NormalizedSourceDir, NormalizedCacheDir);
				}
			}

			auto AddDefaultProjectMountPoint(std::string_view VirtualRoot, std::string_view ProjectDir) -> void
			{
				const std::string NormalizedProjectDir = NormalizeDirectory(ProjectDir);
				AddOrUpdateMountPoint(
					VirtualRoot,
					NormalizedProjectDir + "Shaders/Slang/",
					MakeDefaultCacheDirectory(VirtualRoot));
			}

			auto Finalize() -> void
			{
				if (bFrozen)
				{
					return;
				}

				std::ranges::sort(MountPoints, [](const FShaderMountPoint& A, const FShaderMountPoint& B) {
					return A.VirtualRoot.length() > B.VirtualRoot.length();
				});
				bFrozen = true;
			}

			auto Find(std::string_view VirtualShaderPath) const -> const FShaderMountPoint*
			{
				const auto FoundIt = std::ranges::find_if(MountPoints, [VirtualShaderPath](const FShaderMountPoint& MountPoint) {
					return VirtualShaderPath.starts_with(MountPoint.VirtualRoot);
				});

				return FoundIt != MountPoints.end() ? &*FoundIt : nullptr;
			}

		private:
			static auto NormalizePathString(std::string_view PathString) -> std::string
			{
				return std::filesystem::path(std::string(PathString)).lexically_normal().generic_string();
			}

			static auto NormalizeDirectory(std::string_view Directory) -> std::string
			{
				std::string Result = NormalizePathString(Directory);
				if (!Result.empty() && !Result.ends_with('/'))
				{
					Result.push_back('/');
				}
				return Result;
			}

			std::vector<FShaderMountPoint> MountPoints;
			bool bFrozen = false;
		};

		FShaderMountRegistry GShaderMountRegistry;
	}

	auto GetRegisteredMountPoints() -> const std::vector<FShaderMountPoint>&
	{
		return GShaderMountRegistry.GetMountPoints();
	}

	static auto GetRelativeVirtualShaderPath(std::string_view VirtualShaderPath, const FShaderMountPoint& MountPoint) -> std::string_view
	{
		std::string_view RelativePath = VirtualShaderPath.substr(MountPoint.VirtualRoot.size());
		const size_t RelativeBegin = RelativePath.find_first_not_of('/');
		if (RelativeBegin == std::string_view::npos)
		{
			return {};
		}

		return RelativePath.substr(RelativeBegin);
	}

	auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir) -> void
	{
		RegisterMountPoint(VirtualRoot, SourceDir, MakeDefaultCacheDirectory(VirtualRoot));
	}

	auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view CacheDir) -> void
	{
		checkf(IsInGameThread(), "AddShaderMountPoint must be called from the game thread.");
		checkf(!GShaderMountRegistry.IsFrozen(),
			"Shader mount points are immutable after FShaderPaths::InitDefaultMountPoints(). Register all shader mount points during startup and restart to change them.");
		GShaderMountRegistry.AddOrUpdateMountPoint(VirtualRoot, SourceDir, CacheDir);
	}

	auto SourcePath(std::string_view VirtualShaderPath) -> std::string
	{
		if (const FShaderMountPoint* MountPoint = GShaderMountRegistry.Find(VirtualShaderPath))
		{
			return MountPoint->SourceDir + std::string(GetRelativeVirtualShaderPath(VirtualShaderPath, *MountPoint)) + ".slang";
		}

		DURIN_WARN("Failed to resolve virtual shader path. Make sure the path is correct and a mount point is registered for it. Virtual shader path: {}", VirtualShaderPath);
		return std::string(VirtualShaderPath);
	}

	auto TryMakeVirtualSourcePath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) -> bool
	{
		const std::string NormalizedPhysical = std::filesystem::path(std::string(PhysicalSourcePath)).lexically_normal().generic_string();
		for (const auto& MountPoint : GShaderMountRegistry.GetMountPoints())
		{
			if (NormalizedPhysical.starts_with(MountPoint.SourceDir))
			{
				std::string RelativePath = NormalizedPhysical.substr(MountPoint.SourceDir.size());
				if (RelativePath.ends_with(".slang"))
				{
					RelativePath.resize(RelativePath.size() - 6);
				}
				OutVirtualSourcePath = MountPoint.VirtualRoot + RelativePath;
				return true;
			}
		}

		return false;
	}

	static auto GetRelativeShaderCacheDirectory(std::string_view RelativeVirtualShaderPath) -> std::filesystem::path
	{
		std::filesystem::path CachePath;
		size_t ComponentBegin = 0;
		while (ComponentBegin < RelativeVirtualShaderPath.size())
		{
			const size_t ComponentEnd = RelativeVirtualShaderPath.find('/', ComponentBegin);
			const std::string_view Component = ComponentEnd == std::string::npos
				? RelativeVirtualShaderPath.substr(ComponentBegin)
				: RelativeVirtualShaderPath.substr(ComponentBegin, ComponentEnd - ComponentBegin);

			if (!Component.empty() && Component != "." && Component != "..")
			{
				CachePath /= StringUtils::SanitizeFileName(Component, "Shader");
			}

			if (ComponentEnd == std::string::npos)
			{
				break;
			}
			ComponentBegin = ComponentEnd + 1;
		}

		if (CachePath.empty())
		{
			return std::filesystem::path("Shader.slang");
		}

		std::filesystem::path ParentPath = CachePath.parent_path();
		std::string ShaderDirectoryName = CachePath.filename().generic_string() + ".slang";
		return (ParentPath / ShaderDirectoryName).lexically_normal();
	}

		static auto ResolveShaderDirectoryPath(std::string_view VirtualShaderPath) -> std::filesystem::path
		{
			if (const FShaderMountPoint* MountPoint = GShaderMountRegistry.Find(VirtualShaderPath))
			{
				const std::string_view RelativeVirtualShaderPath = GetRelativeVirtualShaderPath(VirtualShaderPath, *MountPoint);
				return (std::filesystem::path(MountPoint->CacheDir) / GetRelativeShaderCacheDirectory(RelativeVirtualShaderPath)).lexically_normal();
			}

			DURIN_WARN("Failed to resolve virtual shader path. Make sure the path is correct and a mount point is registered for it. Virtual shader path: {}", VirtualShaderPath);
			return std::filesystem::path(MakeDefaultCacheDirectory(VirtualShaderPath)) / "Shader.slang";
		}

	auto MetaPath(std::string_view VirtualShaderPath, std::string_view DependencyKey) -> std::string
	{
		const std::filesystem::path ShaderDirectoryPath = ResolveShaderDirectoryPath(VirtualShaderPath);
		return (ShaderDirectoryPath / "Manifests" / (std::string(DependencyKey) + ".json")).generic_string();
	}

	auto InitDefaultMountPoints() -> void
	{
		checkf(IsInGameThread(), "InitDefaultMountPoints must be called from the game thread.");
		if (GShaderMountRegistry.IsFrozen())
		{
			return;
		}

		GShaderMountRegistry.AddDefaultProjectMountPoint("/Engine/", FPaths::EngineDir());

		const std::string ProjectDir = FPaths::ProjectDir();
		if (!ProjectDir.empty() && ProjectDir != FPaths::EngineDir()
			&& std::filesystem::is_directory(std::filesystem::path(ProjectDir) / "Shaders" / "Slang"))
		{
			GShaderMountRegistry.AddDefaultProjectMountPoint("/Project/", ProjectDir);
		}

		GShaderMountRegistry.Finalize();
	}
} // namespace Durin::ShaderPaths
