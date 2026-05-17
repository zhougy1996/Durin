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

	auto SourcePath(std::string_view ShaderName) -> std::string
	{
		for (const auto& MountPoint : ShaderMountPoints)
		{
			if (ShaderName.starts_with(MountPoint.VirtualRoot))
			{
				std::string RelativePath = std::string(ShaderName.substr(MountPoint.VirtualRoot.size()));
				return MountPoint.SourceDir + RelativePath + ".slang";
			}
		}

		DURIN_WARN("Failed to resolve shader name. Make sure the shader name is correct and a mount point is registered for it. Shader name: {}", ShaderName);
		return {ShaderName.data()};
	}

	static auto MakeFullShaderBinaryPath(std::string_view RawShaderBinaryPath, std::string_view EntryPoint, uint64 ShaderHash) -> std::string
	{
		return std::format("{}_{}_{:016x}{}", RawShaderBinaryPath, EntryPoint, ShaderHash, ".spv");
	}

	auto BinaryPath(std::string_view ShaderName, std::string_view EntryPoint, uint64 ShaderHash) -> std::string
	{
		for (const auto& MountPoint : ShaderMountPoints)
		{
			if (ShaderName.starts_with(MountPoint.VirtualRoot))
			{
				std::string RawRelativePath = std::string(ShaderName.substr(MountPoint.VirtualRoot.size()));
				return MountPoint.BinaryDir + MakeFullShaderBinaryPath(RawRelativePath, EntryPoint, ShaderHash);
			}
		}

		DURIN_WARN("Failed to resolve shader name. Make sure the shader name is correct and a mount point is registered for it. Shader name: {}", ShaderName);
		return MakeFullShaderBinaryPath(std::string(ShaderName), EntryPoint, ShaderHash);
	}

	auto InitDefaultMountPoints() -> void
	{
		const std::string EngineShaderSourceDir = FPaths::EngineDir() + "Shaders/" + "Slang/";
		const std::string EngineShaderBinaryDir = FPaths::EngineDir() + "ShaderCache/" + "SPIR-V/";
		RegisterMountPointWithoutSorting("/Engine/", EngineShaderSourceDir, EngineShaderBinaryDir);
		Sort(ShaderMountPoints);
	}
} // namespace Durin::ShaderPaths
