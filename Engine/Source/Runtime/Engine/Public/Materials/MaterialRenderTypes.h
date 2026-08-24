#pragma once

#include "Materials/MaterialTypes.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Misc/EnumClassFlags.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
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
	};

	using FMaterialRenderLayoutVersion = uint32;
	inline constexpr FMaterialRenderLayoutVersion CurrentMaterialRenderLayoutVersion = 3;
	inline constexpr uint32 MaterialRenderMaxFieldCount = 256;
	inline constexpr uint32 MaterialRenderMaxResourceCount = 64;
	inline constexpr uint32 MaterialRenderMaxUniformPayloadBytes = 16 * 1024;

	// The Id identifies the exact v1 field table and shader binding contract.
	inline constexpr FGuid MaterialRenderLayoutV1Id{
		0x4a6f4c01, 0x27d140b2, 0x8a52cc39, 0x6d4f9a77};
	inline constexpr FGuid MaterialRenderLayoutV3Id{
		0xd71bc1d4, 0xa5894f47, 0x9b5c08b5, 0xf42d75b2};

	struct FMaterialRenderLayoutIdentity
	{
		FMaterialRenderLayoutVersion Version = CurrentMaterialRenderLayoutVersion;
		FGuid Id = MaterialRenderLayoutV3Id;

		auto operator==(const FMaterialRenderLayoutIdentity&) const -> bool = default;
	};

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
		std::vector<std::byte> UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
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
		ENGINE_API auto GetUniformPayload() const -> std::span<const std::byte>;
		ENGINE_API auto GetResources() const
			-> std::span<const FRHITextureReferenceRef>;
		ENGINE_API auto IsError() const -> bool;

	private:
		FMaterialRenderRepresentation(
			FMaterialRenderLayout InLayout,
			std::vector<std::byte> InUniformPayload,
			std::vector<FRHITextureReferenceRef> InResources,
			bool bInError);

		FMaterialRenderLayout Layout;
		std::vector<std::byte> UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
		bool bError = false;
	};

	// Decodes the supported v1 compact binding without exposing parameter GUIDs
	// or reflected objects to Renderer.
	struct FMaterialRenderV1Binding
	{
		FVector4f BaseColor{0.95f, 0.62f, 0.22f, 1.0f};
		float SpecularStrength = 0.35f;
		float Shininess = 32.0f;
		FRHITextureReferenceRef BaseColorTexture;
	};

	// glTF-compatible sampling state retained independently for every texture role.
	enum class EMaterialSamplerMinFilter : uint8
	{
		Nearest,
		Linear,
		NearestMipmapNearest,
		LinearMipmapNearest,
		NearestMipmapLinear,
		LinearMipmapLinear,
	};
	enum class EMaterialSamplerMagFilter : uint8 { Nearest, Linear };
	enum class EMaterialSamplerAddressMode : uint8
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge,
	};

	struct FMaterialSamplerState
	{
		EMaterialSamplerMinFilter MinFilter =
			EMaterialSamplerMinFilter::LinearMipmapLinear;
		EMaterialSamplerMagFilter MagFilter = EMaterialSamplerMagFilter::Linear;
		EMaterialSamplerAddressMode AddressU = EMaterialSamplerAddressMode::Repeat;
		EMaterialSamplerAddressMode AddressV = EMaterialSamplerAddressMode::Repeat;

		auto operator==(const FMaterialSamplerState&) const -> bool = default;
	};

	struct FMaterialRenderBinding
	{
		FVector4f BaseColor{0.95f, 0.62f, 0.22f, 1.0f};
		FVector3f Emissive{0.0f};
		FVector3f Normal{0.0f, 0.0f, 1.0f};
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float AmbientOcclusion = 1.0f;
		float OpacityMask = 1.0f;
		std::array<float, 8> UVChannels{};
		std::array<FVector2f, 8> UVScales{};
		std::array<FVector2f, 8> UVOffsets{};
		std::array<FRHITextureReferenceRef, 8> Textures{};
		std::array<float, 8> UVRotations{};
		std::array<FMaterialSamplerState, 8> Samplers{};

		FMaterialRenderBinding()
		{
			UVScales.fill(FVector2f(1.0f, 1.0f));
		}
	};

	ENGINE_API auto TryGetMaterialRenderV1Binding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderV1Binding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	ENGINE_API auto TryGetMaterialRenderBinding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderBinding& OutBinding,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	ENGINE_API auto EncodeMaterialSamplerState(
		const FMaterialSamplerState& State) -> float;
	ENGINE_API auto TryDecodeMaterialSamplerState(
		float Encoded,
		FMaterialSamplerState& OutState) -> bool;

	// Compiles GUID-addressed Engine values into one layout's compact payload.
	// GUID lookup is confined to this Engine-side builder; Renderer consumes the
	// resulting representation through offsets and resource indices.
	class FMaterialRenderRepresentationBuilder final
	{
	public:
		ENGINE_API explicit FMaterialRenderRepresentationBuilder(
			const FMaterialRenderRepresentation& Source);

		ENGINE_API auto SetScalar(const FGuid& ParameterId, float Value) -> bool;
		ENGINE_API auto SetVector(const FGuid& ParameterId, const FVector3& Value) -> bool;
		// Vector2 values occupy XY of a 16-byte render slot; authored values remain
		// two-dimensional.
		ENGINE_API auto SetVector2(const FGuid& ParameterId, const FVector2& Value) -> bool;
		ENGINE_API auto SetTexture(
			const FGuid& ParameterId,
			const FRHITextureReferenceRef& Value
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

	ENGINE_API auto MakeDefaultMaterialRenderLayout() -> FMaterialRenderLayout;
	ENGINE_API auto MakeMaterialRenderLayoutV1() -> FMaterialRenderLayout;
	ENGINE_API auto MakeCanonicalMaterialRenderRepresentation()
		-> FMaterialRenderRepresentation;
	ENGINE_API auto ValidateMaterialRenderLayout(
		const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	// Contains the renderer-ready subset of resolved material parameters.
	struct FMaterialShaderMapIdentity
	{
		FMaterialRenderLayoutIdentity RenderLayout;
		FMaterialProgramIdentity ProgramIdentity;
		EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque;
		EMaterialShadingModel ShadingModel = EMaterialShadingModel::Lit;
		float OpacityMaskThreshold = 0.333f;

		auto operator==(const FMaterialShaderMapIdentity&) const -> bool = default;
	};

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
				.RenderLayout = {},
				.BlendMode = EMaterialBlendMode::Opaque,
				.ShadingModel = EMaterialShadingModel::Unlit,
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
