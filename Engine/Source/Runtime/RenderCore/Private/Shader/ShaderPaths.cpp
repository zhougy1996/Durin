#include "Shader/ShaderPaths.h"

#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

namespace Durin::FShaderPaths
{
	static std::vector<FShaderMountPoint> ShaderMountPoints;

	auto GetRegisteredMountPoints() -> const std::vector<FShaderMountPoint>&
	{
		return ShaderMountPoints;
	}

	static auto RegisterMountPointWithoutSorting(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view BinaryDir) -> void
	{
		const auto FoundIt = std::ranges::find_if(ShaderMountPoints, [VirtualRoot](const FShaderMountPoint& MountPoint) {
			return MountPoint.VirtualRoot == VirtualRoot;
		});

		if (FoundIt != ShaderMountPoints.end())
		{
			FoundIt->SourceDir = SourceDir;
			FoundIt->BinaryDir = BinaryDir;
			DURIN_DEBUG("Shader mount point updated: {} -> {} (binary: {})", VirtualRoot, SourceDir, BinaryDir);
		}
		else
		{
			ShaderMountPoints.push_back({std::string(VirtualRoot), std::string(SourceDir), std::string(BinaryDir)});
			DURIN_DEBUG("Shader mount point: {} -> {} (binary: {})", VirtualRoot, SourceDir, BinaryDir);
		}
	}

	// Sort the mount points by the length of their virtual root in descending order, so that we can match the longest virtual root first when resolving paths.
	static auto Sort(std::vector<FShaderMountPoint>& InMountPoints) -> void
	{
		std::ranges::sort(InMountPoints, [](const FShaderMountPoint& A, const FShaderMountPoint& B) {
			return A.VirtualRoot.length() > B.VirtualRoot.length();
		});
	}

	auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view BinaryDir) -> void
	{
		checkf(IsInGameThread(), "AddShaderMountPoint must be called from the game thread.");
		RegisterMountPointWithoutSorting(VirtualRoot, SourceDir, BinaryDir);
		Sort(ShaderMountPoints);
	}

	auto SourcePath(std::string_view VirtualShaderPath) -> std::string
	{
		for (const auto& MountPoint : ShaderMountPoints)
		{
			if (VirtualShaderPath.starts_with(MountPoint.VirtualRoot))
			{
				std::string RelativePath = std::string(VirtualShaderPath.substr(MountPoint.VirtualRoot.size()));
				return MountPoint.SourceDir + RelativePath + ".slang";
			}
		}

		DURIN_WARN("Failed to resolve virtual shader path. Make sure the path is correct and a mount point is registered for it. Virtual shader path: {}", VirtualShaderPath);
		return std::string(VirtualShaderPath);
	}

	auto TryMakeVirtualSourcePath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) -> bool
	{
		const std::filesystem::path NormalizedPhysicalPath = std::filesystem::path(std::string(PhysicalSourcePath)).lexically_normal();
		const std::string NormalizedPhysical = NormalizedPhysicalPath.generic_string();
		for (const auto& MountPoint : ShaderMountPoints)
		{
			const std::filesystem::path NormalizedSourceDir = std::filesystem::path(MountPoint.SourceDir).lexically_normal();
			const std::string SourceDir = NormalizedSourceDir.generic_string();
			if (NormalizedPhysical.starts_with(SourceDir))
			{
				std::string RelativePath = NormalizedPhysical.substr(SourceDir.size());
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

	static auto SanitizeFileName(std::string_view Value) -> std::string
	{
		std::string Result;
		Result.reserve(Value.size());
		for (const char Char : Value)
		{
			const bool bAlphaNumeric =
				(Char >= 'a' && Char <= 'z') ||
				(Char >= 'A' && Char <= 'Z') ||
				(Char >= '0' && Char <= '9');
			if (bAlphaNumeric || Char == '_' || Char == '-' || Char == '.')
			{
				Result.push_back(Char);
			}
			else
			{
				Result.push_back('_');
			}
		}
		return Result.empty() ? "Shader" : Result;
	}

	static auto GetStageSuffix(EShaderFrequency Frequency) -> std::string_view
	{
		switch (Frequency)
		{
		case EShaderFrequency::Vertex:
			return "vs";
		case EShaderFrequency::Pixel:
			return "ps";
		case EShaderFrequency::Compute:
			return "cs";
		case EShaderFrequency::RayGen:
			return "rgen";
		case EShaderFrequency::RayHitGroup:
			return "rhit";
		case EShaderFrequency::RayMiss:
			return "rmiss";
		default:
			return "shader";
		}
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

	static auto GetRelativeShaderCachePath(std::string_view VirtualShaderPath, const FShaderMountPoint& MountPoint) -> std::filesystem::path
	{
		std::filesystem::path RelativePath(std::string(VirtualShaderPath.substr(MountPoint.VirtualRoot.size())));
		if (RelativePath.empty())
		{
			RelativePath = "Shader";
		}
		if (RelativePath.is_absolute())
		{
			RelativePath = RelativePath.relative_path();
		}
		return RelativePath.lexically_normal();
	}

	auto ShaderDirectory(std::string_view VirtualShaderPath) -> std::string
	{
		for (const auto& MountPoint : ShaderMountPoints)
		{
			if (VirtualShaderPath.starts_with(MountPoint.VirtualRoot))
			{
				return MakeDirectoryString(std::filesystem::path(MountPoint.BinaryDir) / GetRelativeShaderCachePath(VirtualShaderPath, MountPoint));
			}
		}

		DURIN_WARN("Failed to resolve virtual shader path. Make sure the path is correct and a mount point is registered for it. Virtual shader path: {}", VirtualShaderPath);
		return MakeDirectoryString(std::filesystem::path("ShaderCache") / "SPIR-V" / SanitizeFileName(VirtualShaderPath));
	}

	auto CacheDirectory(std::string_view VirtualShaderPath, std::string_view CacheKey) -> std::string
	{
		return MakeDirectoryString(std::filesystem::path(ShaderDirectory(VirtualShaderPath)) / std::string(CacheKey));
	}

	auto MetaPath(std::string_view VirtualShaderPath) -> std::string
	{
		const std::filesystem::path ShaderDir(ShaderDirectory(VirtualShaderPath));
		const std::string ShaderBaseName = ShaderDir.filename().empty()
			? ShaderDir.parent_path().filename().generic_string()
			: ShaderDir.filename().generic_string();
		return (ShaderDir / (SanitizeFileName(ShaderBaseName) + ".slang.meta")).generic_string();
	}

	auto BinaryPath(std::string_view VirtualShaderPath, std::string_view EntryPoint, EShaderFrequency Frequency, std::string_view CacheKey) -> std::string
	{
		const std::string FileName = std::format("{}.{}.spv", SanitizeFileName(EntryPoint), GetStageSuffix(Frequency));
		return (std::filesystem::path(CacheDirectory(VirtualShaderPath, CacheKey)) / FileName).generic_string();
	}

	auto InitDefaultMountPoints() -> void
	{
		const std::string EngineShaderSourceDir = FPaths::EngineDir() + "Shaders/" + "Slang/";
		const std::string EngineShaderBinaryDir = FPaths::EngineDir() + "ShaderCache/" + "SPIR-V/";
		RegisterMountPointWithoutSorting("/Engine/", EngineShaderSourceDir, EngineShaderBinaryDir);
		Sort(ShaderMountPoints);
	}
} // namespace Durin::ShaderPaths
