#pragma once

#include "RenderCoreAPI.h"

namespace Durin::FShaderPaths
{
	struct FShaderMountPoint
	{
		std::string VirtualRoot;
		std::string SourceDir;
		std::string BinaryDir;
	};

	RENDERCORE_API auto GetRegisteredMountPoints() -> const std::vector<FShaderMountPoint>&;

	RENDERCORE_API auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view BinaryDir) -> void;

	RENDERCORE_API auto SourcePath(std::string_view VirtualShaderPath) -> std::string;

	// Converts an on-disk shader source path back to the registered virtual shader path when possible.
	RENDERCORE_API auto TryMakeVirtualSourcePath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) -> bool;

	RENDERCORE_API auto CacheDirectory(std::string_view VirtualShaderPath, std::string_view CacheKey) -> std::string;

	RENDERCORE_API auto ShaderDirectory(std::string_view VirtualShaderPath) -> std::string;

	RENDERCORE_API auto MetaPath(std::string_view VirtualShaderPath) -> std::string;

	RENDERCORE_API auto BinaryPath(std::string_view VirtualShaderPath, std::string_view EntryPoint, std::string_view CacheKey) -> std::string;

	auto InitDefaultMountPoints() -> void;
}
