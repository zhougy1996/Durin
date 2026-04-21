#pragma once

#include "RenderCore/API.h"

namespace Doge::FShaderPaths
{
	struct FShaderMountPoint
	{
		std::string VirtualRoot;
		std::string SourceDir;
		std::string BinaryDir;
	};

	RENDERCORE_API auto GetRegisteredMountPoints() -> const std::vector<FShaderMountPoint>&;

	RENDERCORE_API auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view BinaryDir) -> void;

	RENDERCORE_API auto SourcePath(std::string_view ShaderName) -> std::string;

	RENDERCORE_API auto BinaryPath(std::string_view ShaderName, std::string_view EntryPoint, uint64 ShaderHash) -> std::string;

	auto InitDefaultMountPoints() -> void;
}
