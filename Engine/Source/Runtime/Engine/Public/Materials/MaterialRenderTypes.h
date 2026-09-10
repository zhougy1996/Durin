#pragma once

#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Misc/EnumClassFlags.h"
#include "Shader/MaterialShaderIdentity.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
	enum class EMaterialRenderValidationFailure : uint8
	{
		None,
		UnsupportedVersion,
		UnsupportedIdentity,
		InvalidCounts,
		InvalidPayloadSize,
		InvalidField,
		DuplicateField,
		InvalidOffset,
		InvalidAlignment,
		OverlappingFields,
		NonFiniteValue,
		NonZeroPadding,
		InvalidResource,
	};

	struct FMaterialRenderValidationDiagnostic
	{
		EMaterialRenderValidationFailure Failure = EMaterialRenderValidationFailure::None;
		uint32 FieldIndex = 0;
		std::string Message;
	};

	struct FMaterialRenderRepresentationInput
	{
		FMaterialRenderLayout Layout;
		FByteBuffer UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
		std::vector<FMaterialSamplerState> Samplers;
		std::vector<EMaterialTextureFallback> TextureFallbacks;
	};

	// Immutable after construction; no reflected object or raw resource pointer
	// is retained in the published representation.
	class FMaterialRenderRepresentation final
	{
	public:
		ENGINE_API FMaterialRenderRepresentation();

		ENGINE_API static auto TryCreate(
			FMaterialRenderRepresentationInput Input,
			FMaterialRenderRepresentation& OutRepresentation,
			FMaterialRenderValidationDiagnostic& OutDiagnostic
		) -> bool;

		ENGINE_API auto GetLayout() const -> const FMaterialRenderLayout&;
		ENGINE_API auto GetUniformPayload() const -> FByteView;
		ENGINE_API auto GetResources() const
			-> std::span<const FRHITextureReferenceRef>;
		ENGINE_API auto IsError() const -> bool;
		auto GetSamplers() const -> std::span<const FMaterialSamplerState> { return Samplers; }
		auto GetTextureFallbacks() const -> std::span<const EMaterialTextureFallback> { return TextureFallbacks; }

	private:
		FMaterialRenderRepresentation(
			FMaterialRenderLayout InLayout,
			FByteBuffer InUniformPayload,
			std::vector<FRHITextureReferenceRef> InResources,
			std::vector<FMaterialSamplerState> InSamplers,
			std::vector<EMaterialTextureFallback> InFallbacks,
			bool bInError);

		FMaterialRenderLayout Layout;
		FByteBuffer UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
		std::vector<FMaterialSamplerState> Samplers;
		std::vector<EMaterialTextureFallback> TextureFallbacks;
		bool bError = false;
	};

	struct FMaterialRenderBinding
	{
		bool bError = false;
		FMaterialRenderLayoutIdentity LayoutIdentity;
		FByteBuffer CompiledUniformPayload;
		std::vector<FRHITextureReferenceRef> CompiledTextures;
		std::vector<FMaterialSamplerState> CompiledSamplers;
		std::vector<EMaterialTextureFallback> CompiledTextureFallbacks;

	};

	ENGINE_API auto TryGetMaterialRenderBinding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderBinding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	// Compiles GUID-addressed Engine values into one layout's compact payload.
	// GUID lookup is confined to this Engine-side builder; Renderer consumes the
	// resulting representation through offsets and resource indices.
	class FMaterialRenderRepresentationBuilder final
	{
	public:
		ENGINE_API explicit FMaterialRenderRepresentationBuilder(
			const FMaterialRenderRepresentation& Source);

		ENGINE_API explicit FMaterialRenderRepresentationBuilder(const FMaterialRenderLayout& Layout);

		ENGINE_API auto SetScalar(const FGuid& ParameterId, float Value) -> bool;
		ENGINE_API auto SetVector(const FGuid& ParameterId, const FVector3& Value) -> bool;
		// Vector2 values occupy XY of a 16-byte render slot; authored values remain
		// two-dimensional.
		ENGINE_API auto SetVector4(const FGuid& ParameterId, const FVector4& Value) -> bool;
		ENGINE_API auto SetVector2(const FGuid& ParameterId, const FVector2& Value) -> bool;
		ENGINE_API auto SetTexture(
			const FGuid& ParameterId,
			const FRHITextureReferenceRef& Value,
			FMaterialSamplerState Sampler = {},
			EMaterialTextureFallback Fallback = EMaterialTextureFallback::White
		) -> bool;
		ENGINE_API auto Build(
			FMaterialRenderRepresentation& OutRepresentation,
			FMaterialRenderValidationDiagnostic& OutDiagnostic
		) -> bool;

	private:
		ENGINE_API auto FindField(const FGuid& ParameterId) const
			-> const FMaterialRenderField*;
		ENGINE_API auto RejectField(const FGuid& ParameterId) -> bool;

		FMaterialRenderRepresentationInput Input;
		bool bInvalid = false;
		FGuid InvalidParameterId;
	};

	ENGINE_API auto MakeErrorMaterialRenderLayout() -> FMaterialRenderLayout;
	ENGINE_API auto ValidateMaterialRenderLayout(
		const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	struct FMaterialPlanningPassIdentity
	{
		FMaterialShaderMapIdentity ShaderMap;
		bool bTwoSided = false;
		EMaterialDepthWritePolicy DepthWritePolicy = EMaterialDepthWritePolicy::Automatic;

		auto operator==(const FMaterialPlanningPassIdentity&) const -> bool = default;
	};

	struct FMaterialRenderData
	{
		FMaterialRenderRepresentation Representation;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
		FMaterialPlanningPassIdentity PlanningPassIdentity{
			.ShaderMap = {
				.RenderLayout = MakeErrorMaterialRenderLayout().Identity,
				.BlendMode = FMaterialShaderBlendModeKey(EMaterialBlendMode::Opaque),
				.ShadingModel = FMaterialShaderShadingModelKey(EMaterialShadingModel::Unlit),
				.OpacityMaskThreshold = 0.333f,
			},
			.bTwoSided = true,
			.DepthWritePolicy = EMaterialDepthWritePolicy::Enabled,
		};
	};

	// Asset- and RHI-independent terminal for invalid whole-material state.
	// The returned immutable data is safe to inspect from the render thread.
	ENGINE_API auto GetErrorMaterialRenderData() -> const FMaterialRenderData&;

	enum class EMaterialFallbackReason : uint8
	{
		UnassignedDefault,
		DefaultAssetUnavailable,
		MaterialDataInvalid,
		UnsupportedLayout,
		MissingProxy,
		Count,
	};

	struct FMaterialFallbackDiagnosticsSnapshot
	{
		std::array<uint64, static_cast<size_t>(EMaterialFallbackReason::Count)>
			Counts{};

		auto Get(EMaterialFallbackReason Reason) const -> uint64
		{
			return Counts[static_cast<size_t>(Reason)];
		}
	};

	ENGINE_API auto RecordMaterialFallbackReason(
		EMaterialFallbackReason Reason) -> void;
	ENGINE_API auto GetMaterialFallbackDiagnosticsSnapshot()
		-> FMaterialFallbackDiagnosticsSnapshot;
	ENGINE_API auto ResetMaterialFallbackDiagnosticsForTests() -> void;

	// Selects which part of material render state changed.
	enum class EMaterialRenderDirtyFlags : uint8
	{
		None = 0,
		DynamicParameters = 1 << 0,
		ShaderMap = 1 << 1,
		PipelineState = 1 << 2,
		ParentChain = 1 << 3,
		AllRenderState = (1 << 0) | (1 << 1) | (1 << 2)
	};
	ENUM_CLASS_FLAGS(EMaterialRenderDirtyFlags);

}
