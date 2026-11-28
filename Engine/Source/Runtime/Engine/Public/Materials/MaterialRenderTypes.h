#pragma once

#include "CoreMinimal.h"
#include "UniformBuffer.h"

#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Misc/EnumClassFlags.h"
#include "Shader/MaterialShaderIdentity.h"

namespace Durin
{
	struct FMaterialRenderValidationDiagnostic
	{
		std::string Message;
	};

	struct FMaterialRenderRepresentationInput
	{
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
		FMaterialRenderRepresentation(const FMaterialRenderRepresentation&) = default;
		auto operator=(const FMaterialRenderRepresentation&) -> FMaterialRenderRepresentation& = default;

		ENGINE_API static auto TryCreate(
			FMaterialRenderLayout Layout,
			FMaterialRenderRepresentationInput Input,
			FMaterialRenderRepresentation& OutRepresentation,
			FMaterialRenderValidationDiagnostic& OutDiagnostic
		) -> bool;

		ENGINE_API auto GetLayout() const -> const FMaterialRenderLayout&;
		ENGINE_API auto GetUniformPayload() const -> FByteView;
		ENGINE_API auto GetResources() const
			-> std::span<const FRHITextureReferenceRef>;
		ENGINE_API auto GetSamplers() const -> std::span<const FMaterialSamplerState>;
		ENGINE_API auto GetTextureFallbacks() const -> std::span<const EMaterialTextureFallback>;
		ENGINE_API auto GetRecordId() const -> uint64;
		ENGINE_API auto GetContentHash() const -> uint64;

	private:
		FMaterialRenderRepresentation(
			std::shared_ptr<const FMaterialRenderLayout> InLayout,
			FByteBuffer InUniformPayload,
			std::vector<FRHITextureReferenceRef> InResources,
			std::vector<FMaterialSamplerState> InSamplers,
			std::vector<EMaterialTextureFallback> InFallbacks);

		friend class FMaterialRenderRepresentationBuilder;
		static auto TryCreateFromLayout(std::shared_ptr<const FMaterialRenderLayout> Layout,
			FMaterialRenderRepresentationInput Input, FMaterialRenderRepresentation& OutRepresentation,
			FMaterialRenderValidationDiagnostic& OutDiagnostic) -> bool;
		struct FStorage;
		std::shared_ptr<const FStorage> Storage;
	};

	// Compiles GUID-addressed Engine values into one layout's compact payload.
	// GUID lookup is confined to this Engine-side builder; Renderer consumes the
	// resulting representation through offsets and resource indices.
	class FMaterialRenderRepresentationBuilder final
	{
	public:
		ENGINE_API explicit FMaterialRenderRepresentationBuilder(
			const FMaterialRenderRepresentation& Source);

		ENGINE_API explicit FMaterialRenderRepresentationBuilder(const FMaterialRenderLayout& Layout);
		ENGINE_API auto SetParameter(const FMaterialFieldKey& Id, const FMaterialParameterValue& Value) -> bool;

		ENGINE_API auto SetScalar(const FMaterialFieldKey& ParameterId, float Value) -> bool;
		ENGINE_API auto SetVector(const FMaterialFieldKey& ParameterId, const FVector3& Value) -> bool;
		// Vector2 values occupy XY of a 16-byte render slot; authored values remain
		// two-dimensional.
		ENGINE_API auto SetVector4(const FMaterialFieldKey& ParameterId, const FVector4& Value) -> bool;
		ENGINE_API auto SetVector2(const FMaterialFieldKey& ParameterId, const FVector2& Value) -> bool;
		ENGINE_API auto SetTexture(
			const FMaterialFieldKey& ParameterId,
			const FRHITextureReferenceRef& Value,
			FMaterialSamplerState Sampler = {},
			EMaterialTextureFallback Fallback = EMaterialTextureFallback::White
		) -> bool;
		ENGINE_API auto Build(
			FMaterialRenderRepresentation& OutRepresentation,
			FMaterialRenderValidationDiagnostic& OutDiagnostic
		) -> bool;

	private:
		friend class FMaterialRenderProxy;
		explicit FMaterialRenderRepresentationBuilder(std::shared_ptr<const FMaterialRenderLayout> Layout);
		auto WriteBoundParameter(uint32 FieldIndex, const FMaterialParameterValue& Value) -> bool;
		ENGINE_API auto FindField(const FMaterialFieldKey& ParameterId) const
			-> const FMaterialRenderField*;
		ENGINE_API auto RejectField(const FMaterialFieldKey& ParameterId) -> bool;

		std::shared_ptr<const FMaterialRenderLayout> Layout;
		FMaterialRenderRepresentationInput Input;
		bool bInvalid = false;
		FMaterialFieldKey InvalidParameterId;
	};

	ENGINE_API auto MakeErrorMaterialRenderLayout() -> FMaterialRenderLayout;
	ENGINE_API auto ValidateMaterialRenderLayout(
		const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	struct FMaterialPlanningPassIdentity
	{
		FMeshMaterialShaderMapIdentity ShaderMap;
		bool bTwoSided = false;
		bool bIsSky = false;
		EMaterialDepthWritePolicy DepthWritePolicy = EMaterialDepthWritePolicy::Automatic;

		auto operator==(const FMaterialPlanningPassIdentity&) const -> bool = default;
	};

	struct FUniformExpressionCache
	{
		std::shared_ptr<FUniformBuffer> UniformBuffer;
		auto InvalidateUniformExpressionCache() -> void { CachedRepresentation = 0; ParameterCollections.clear(); }
		auto InvalidateUniformValues() -> void { CachedRepresentation = 0; }
		auto ReleaseRHI() -> void
		{
			if (UniformBuffer) UniformBuffer->ReleaseRHI();
			for (const auto& [Key, Buffer] : ParameterCollections) Buffer->ReleaseRHI();
			InvalidateUniformExpressionCache();
		}
		auto GetRetainedUniformRange() const -> FRHIUniformBufferRange
		{
			if (!UniformBuffer) return {};
			auto Range = UniformBuffer->GetRange();
			Range.ResourceOwner = UniformBuffer->GetRHI();
			return Range;
		}
		auto IsUpToDate(const FMaterialRenderRepresentation& Material) const -> bool
		{ return CachedRepresentation == Material.GetRecordId() && UniformBuffer && UniformBuffer->GetRHI(); }
		ENGINE_API auto UpdateUniformBuffer(FRHICommandListBase& Commands, const FMaterialRenderRepresentation& Material,
			std::span<FRHIResource* const> Resources = {}, std::span<const FRHIUniformBufferResource> Members = {}) -> bool;
		ENGINE_API auto GetParameterCollectionUniformBuffer(const std::shared_ptr<const FMaterialProgram>& Program,
			FGuid Collection) -> FUniformBuffer&;
	private:
		uint64 CachedRepresentation = 0;
		std::map<std::pair<std::shared_ptr<const FMaterialProgram>, FGuid>, std::shared_ptr<FUniformBuffer>> ParameterCollections;
	};
	struct FMaterialRenderData
	{
		// A proxy publishes its stable cache here. Independent sources own a cache shared by their copies.
		std::shared_ptr<FUniformExpressionCache> UniformExpressionCache = std::make_shared<FUniformExpressionCache>();
		FMaterialRenderRepresentation Representation;
		std::shared_ptr<const FMaterialProgram> CompiledProgram;
		bool bPending = false;
		ENGINE_API auto IsError() const -> bool;
		auto IsReadyForRendering() const -> bool
		{ return !bPending && Representation.GetLayout().Identity.Version == CompiledMaterialRenderLayoutVersion; }

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
