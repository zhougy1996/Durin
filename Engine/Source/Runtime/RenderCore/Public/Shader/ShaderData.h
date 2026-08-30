#pragma once

#include "RenderCoreAPI.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	enum class EShaderDataDomain : uint8
	{
		Authored,
		Cooked,
	};

	// Immutable process-wide selection of the only legal Shader data path.
	struct FShaderDataConfiguration
	{
		EShaderDataDomain Domain = EShaderDataDomain::Authored;
		EShaderTargetPlatform TargetPlatform = EShaderTargetPlatform::Win64;
		EShaderTargetProfile TargetProfile = EShaderTargetProfile::EditorValidation;
		std::filesystem::path CookRoot;

		RENDERCORE_API static auto Authored() -> FShaderDataConfiguration;
		RENDERCORE_API static auto Cooked(
			std::filesystem::path InCookRoot,
			EShaderTargetPlatform InTargetPlatform = EShaderTargetPlatform::Win64,
			EShaderTargetProfile InTargetProfile = EShaderTargetProfile::Game)
			-> FShaderDataConfiguration;
	};

	RENDERCORE_API auto InitializeShaderData(
		FShaderDataConfiguration Configuration,
		std::string& OutError) -> bool;
	RENDERCORE_API auto ShutdownShaderData() -> void;
	RENDERCORE_API auto GetShaderDataDomain() -> EShaderDataDomain;
	RENDERCORE_API auto LoadCookedShaderRuntimeRequest(
		std::string_view RequestName,
		std::span<const FShaderType* const> ShaderTypes,
		FShaderCompilerOutput& OutOutput,
		std::string& OutError) -> bool;
}
