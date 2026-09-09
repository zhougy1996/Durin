#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"
#include "Shader/MaterialShaderIdentity.h"

#include <span>
#include <vector>

namespace Durin
{
	struct FMaterialCompilerParameterDeclaration
	{
		FGuid Id;
		EMaterialParameterType Type = EMaterialParameterType::Scalar;

		auto operator==(const FMaterialCompilerParameterDeclaration&) const
			-> bool = default;
	};

	// Describes the transient Engine-to-Renderer material payload protocol.
	enum class EMaterialRenderFieldStorage : uint8
	{
		Uniform,
		Resource,
	};

	enum class EMaterialRenderValueType : uint8
	{
		Scalar,
		Vector3,
		Vector4,
		Texture2D,
		Vector2,
	};

	inline constexpr uint32 MaterialRenderMaxFieldCount = 256;
	inline constexpr uint32 MaterialRenderMaxResourceCount = 64;
	inline constexpr uint32 MaterialRenderMaxUniformPayloadBytes = 16 * 1024;

	struct FMaterialRenderField
	{
		// The GUID is retained for Engine-side compilation and diagnostics only.
		FGuid ParameterId;
		EMaterialRenderFieldStorage Storage = EMaterialRenderFieldStorage::Uniform;
		EMaterialRenderValueType Type = EMaterialRenderValueType::Scalar;
		uint16 CompactIndex = 0;
		uint32 Offset = 0;
		uint32 Size = 0;

		auto operator==(const FMaterialRenderField&) const -> bool = default;
	};

	struct FMaterialRenderLayout
	{
		FMaterialRenderLayoutIdentity Identity;
		uint32 UniformPayloadSize = 0;
		uint16 UniformFieldCount = 0;
		uint16 ResourceFieldCount = 0;
		std::vector<FMaterialRenderField> Fields;

		auto operator==(const FMaterialRenderLayout&) const -> bool = default;
	};

	inline constexpr uint32 CompiledMaterialRenderLayoutVersion = 4;
	inline constexpr uint32 MaterialTextureBindingBase = 32;
	inline constexpr uint32 MaterialUniformControlBytes = 16;

	// Target policy, further clamped to the active device when one is available.
	// Reserves four sampled images, two samplers and three uniform buffers for
	// lighting, environment, shadows and geometry in the shared descriptor set.
	struct FMaterialCompilerResourceLimits
	{
		uint32 SampledImages = 16;
		uint32 Samplers = 16;
		uint32 UniformBuffers = 12;
		uint32 StageResources = 128;
		uint32 UniformBufferBytes = MaterialRenderMaxUniformPayloadBytes;
		auto operator==(const FMaterialCompilerResourceLimits&) const -> bool = default;
	};

	enum class EMaterialLayoutError : uint8
	{
		None,
		InvalidParameter,
		DuplicateParameter,
		InvalidType,
		ResourceLimit,
		InvalidField,
		InvalidIdentity,
		InvalidReflection,
	};

	struct FMaterialLayoutValidationResult
	{
		EMaterialLayoutError Error = EMaterialLayoutError::None;
		FGuid ParameterId;
		uint32 FieldIndex = 0;
		explicit operator bool() const { return Error == EMaterialLayoutError::None; }
	};

	struct FMaterialLayoutBuildResult
	{
		FMaterialLayoutValidationResult Validation;
		FMaterialRenderLayout Layout;
		explicit operator bool() const { return static_cast<bool>(Validation); }
	};

	[[nodiscard]] ENGINE_API auto CompileMaterialLayout(
		std::span<const FMaterialCompilerParameterDeclaration> Parameters,
		const FMaterialCompilerResourceLimits& Limits = {}) -> FMaterialLayoutBuildResult;
	[[nodiscard]] ENGINE_API auto ValidateCompiledMaterialLayout(
		const FMaterialRenderLayout& Layout,
		const FMaterialCompilerResourceLimits& Limits = {}) -> FMaterialLayoutValidationResult;
	ENGINE_API auto GetMaterialLayoutErrorText(EMaterialLayoutError Error) -> std::string_view;
}
