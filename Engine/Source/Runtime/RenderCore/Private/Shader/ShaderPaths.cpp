#include "Shader/ShaderPaths.h"

#include "Misc/Paths.h"
#include "Misc/StringConvert.h"
#include "Threading/RunnableThread.h"

namespace Durin::FShaderPaths
{
	static std::vector<FShaderMountPoint> ShaderMountPoints;

	static auto NormalizeVirtualRoot(std::string_view VirtualRoot) -> std::string
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

	auto GetRegisteredMountPoints() -> const std::vector<FShaderMountPoint>&
	{
		return ShaderMountPoints;
	}

	static auto RegisterMountPointWithoutSorting(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view BinaryDir) -> void
	{
		const std::string NormalizedVirtualRoot = NormalizeVirtualRoot(VirtualRoot);
		const std::string NormalizedSourceDir = NormalizeDirectory(SourceDir);
		const std::string NormalizedBinaryDir = NormalizeDirectory(BinaryDir);

		const auto FoundIt = std::ranges::find_if(ShaderMountPoints, [&NormalizedVirtualRoot](const FShaderMountPoint& MountPoint) {
			return MountPoint.VirtualRoot == NormalizedVirtualRoot;
		});

		if (FoundIt != ShaderMountPoints.end())
		{
			FoundIt->SourceDir = NormalizedSourceDir;
			FoundIt->BinaryDir = NormalizedBinaryDir;
			DURIN_DEBUG("Shader mount point updated: {} -> {} (binary: {})", NormalizedVirtualRoot, NormalizedSourceDir, NormalizedBinaryDir);
		}
		else
		{
			ShaderMountPoints.push_back({NormalizedVirtualRoot, NormalizedSourceDir, NormalizedBinaryDir});
			DURIN_DEBUG("Shader mount point: {} -> {} (binary: {})", NormalizedVirtualRoot, NormalizedSourceDir, NormalizedBinaryDir);
		}
	}

	// Sort the mount points by the length of their virtual root in descending order, so that we can match the longest virtual root first when resolving paths.
	static auto Sort(std::vector<FShaderMountPoint>& InMountPoints) -> void
	{
		std::ranges::sort(InMountPoints, [](const FShaderMountPoint& A, const FShaderMountPoint& B) {
			return A.VirtualRoot.length() > B.VirtualRoot.length();
		});
	}

	static auto FindMountPoint(std::string_view VirtualShaderPath) -> const FShaderMountPoint*
	{
		const auto FoundIt = std::ranges::find_if(ShaderMountPoints, [VirtualShaderPath](const FShaderMountPoint& MountPoint) {
			return VirtualShaderPath.starts_with(MountPoint.VirtualRoot);
		});

		return FoundIt != ShaderMountPoints.end() ? &*FoundIt : nullptr;
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

	auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view BinaryDir) -> void
	{
		checkf(IsInGameThread(), "AddShaderMountPoint must be called from the game thread.");
		RegisterMountPointWithoutSorting(VirtualRoot, SourceDir, BinaryDir);
		Sort(ShaderMountPoints);
	}

	auto SourcePath(std::string_view VirtualShaderPath) -> std::string
	{
		if (const FShaderMountPoint* MountPoint = FindMountPoint(VirtualShaderPath))
		{
			return MountPoint->SourceDir + std::string(GetRelativeVirtualShaderPath(VirtualShaderPath, *MountPoint)) + ".slang";
		}

		DURIN_WARN("Failed to resolve virtual shader path. Make sure the path is correct and a mount point is registered for it. Virtual shader path: {}", VirtualShaderPath);
		return std::string(VirtualShaderPath);
	}

	auto TryMakeVirtualSourcePath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) -> bool
	{
		const std::string NormalizedPhysical = NormalizePathString(PhysicalSourcePath);
		for (const auto& MountPoint : ShaderMountPoints)
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

	static auto MakeDirectoryString(const std::filesystem::path& InPath) -> std::string
	{
		std::string Result = InPath.lexically_normal().generic_string();
		if (!Result.ends_with('/'))
		{
			Result.push_back('/');
		}
		return Result;
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
				CachePath /= String::SanitizeFileName(Component, "Shader");
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
		if (const FShaderMountPoint* MountPoint = FindMountPoint(VirtualShaderPath))
		{
			const std::string_view RelativeVirtualShaderPath = GetRelativeVirtualShaderPath(VirtualShaderPath, *MountPoint);
			return (std::filesystem::path(MountPoint->BinaryDir) / GetRelativeShaderCacheDirectory(RelativeVirtualShaderPath)).lexically_normal();
		}

		DURIN_WARN("Failed to resolve virtual shader path. Make sure the path is correct and a mount point is registered for it. Virtual shader path: {}", VirtualShaderPath);
		return std::filesystem::path("ShaderCache") / "SPIR-V" / (String::SanitizeFileName(VirtualShaderPath, "Shader") + ".slang");
	}

	auto ShaderDirectory(std::string_view VirtualShaderPath) -> std::string
	{
		return MakeDirectoryString(ResolveShaderDirectoryPath(VirtualShaderPath));
	}

	auto CacheDirectory(std::string_view VirtualShaderPath, std::string_view CacheKey) -> std::string
	{
		return MakeDirectoryString(ResolveShaderDirectoryPath(VirtualShaderPath) / std::string(CacheKey));
	}

	auto MetaPath(std::string_view VirtualShaderPath) -> std::string
	{
		const std::filesystem::path ShaderDirectoryPath = ResolveShaderDirectoryPath(VirtualShaderPath);
		const std::string MetaFileName = ShaderDirectoryPath.filename().generic_string() + ".meta";
		return (ShaderDirectoryPath / MetaFileName).generic_string();
	}

	auto BinaryPath(std::string_view VirtualShaderPath, std::string_view EntryPoint, std::string_view CacheKey) -> std::string
	{
		const std::string FileName = String::SanitizeFileName(EntryPoint, "Shader") + ".spv";
		return (ResolveShaderDirectoryPath(VirtualShaderPath) / std::string(CacheKey) / FileName).generic_string();
	}

	auto InitDefaultMountPoints() -> void
	{
		const std::string EngineShaderSourceDir = FPaths::EngineDir() + "Shaders/" + "Slang/";
		const std::string EngineShaderBinaryDir = FPaths::EngineDir() + "ShaderCache/" + "SPIR-V/";
		RegisterMountPointWithoutSorting("/Engine/", EngineShaderSourceDir, EngineShaderBinaryDir);
		Sort(ShaderMountPoints);
	}
} // namespace Durin::ShaderPaths
