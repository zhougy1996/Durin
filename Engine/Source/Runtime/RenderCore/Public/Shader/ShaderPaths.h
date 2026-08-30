#pragma once

#include "RenderCoreAPI.h"
#include "RHIDefinitions.h"

namespace Durin::FShaderPaths
{
	// Maps a stable virtual shader root to source and cache directories.
	struct FShaderMountPoint
	{
		std::string VirtualRoot;
		std::string SourceDir;
		std::string CacheDir;
	};

	RENDERCORE_API auto GetRegisteredMountPoints() -> const std::vector<FShaderMountPoint>&;

	// Shader mount points are expected to be registered only during RenderCore initialization.
	// Runtime mutation after initialization is not supported. The cache defaults to a
	// mount-specific namespace beneath FPaths::DerivedDataCacheDir().
	RENDERCORE_API auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir) -> void;

	// Explicit cache roots support tests and externally managed shader caches.
	RENDERCORE_API auto RegisterMountPoint(std::string_view VirtualRoot, std::string_view SourceDir, std::string_view CacheDir) -> void;

	RENDERCORE_API auto SourcePath(std::string_view VirtualShaderPath) -> std::string;

	// Converts an on-disk shader source path back to the registered virtual shader path when possible.
	RENDERCORE_API auto TryMakeVirtualSourcePath(std::string_view PhysicalSourcePath, std::string& OutVirtualSourcePath) -> bool;

	RENDERCORE_API auto MetaPath(std::string_view VirtualShaderPath, std::string_view DependencyKey) -> std::string;

	auto InitDefaultMountPoints() -> void;
}
