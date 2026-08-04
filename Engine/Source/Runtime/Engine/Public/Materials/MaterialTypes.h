#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"
#include "Misc/EnumClassFlags.h"
#include "Texture/Texture2D.h"

#include "MaterialTypes.gen.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
	class DMaterialInterface;

	// Selects the active storage field in a material parameter value.
	DENUM()
	enum class EMaterialParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
	};

	// Selects editor presentation without changing the parameter's runtime type.
	DENUM()
	enum class EMaterialParameterPresentation : uint8
	{
		Default,
		Drag,
		Color,
		AssetPicker,
	};

	// Selects the render-pass policy required by a material.
	DENUM()
	enum class EMaterialBlendMode : uint8
	{
		Opaque,
		Masked,
		Translucent,
	};

	// Selects the lighting contract implemented by a material shader map.
	DENUM()
	enum class EMaterialShadingModel : uint8
	{
		Lit,
		Unlit,
	};

	// Controls depth writes independently from the blend-mode default.
	DENUM()
	enum class EMaterialDepthWritePolicy : uint8
	{
		Automatic,
		Enabled,
		Disabled,
	};

	using FMaterialParameterSchemaVersion = uint32;
	inline constexpr FMaterialParameterSchemaVersion CurrentMaterialParameterSchemaVersion = 1;

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
	inline constexpr FMaterialRenderLayoutVersion CurrentMaterialRenderLayoutVersion = 1;
	inline constexpr uint32 MaterialRenderMaxFieldCount = 256;
	inline constexpr uint32 MaterialRenderMaxResourceCount = 64;
	inline constexpr uint32 MaterialRenderMaxUniformPayloadBytes = 16 * 1024;

	// The Id identifies the exact v1 field table and shader binding contract.
	inline constexpr FGuid MaterialRenderLayoutV1Id{
		0x4a6f4c01, 0x27d140b2, 0x8a52cc39, 0x6d4f9a77};

	struct FMaterialRenderLayoutIdentity
	{
		FMaterialRenderLayoutVersion Version = CurrentMaterialRenderLayoutVersion;
		FGuid Id = MaterialRenderLayoutV1Id;

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
		ENGINE_API auto IsFallback() const -> bool;

	private:
		FMaterialRenderRepresentation(
			FMaterialRenderLayout InLayout,
			std::vector<std::byte> InUniformPayload,
			std::vector<FRHITextureReferenceRef> InResources,
			bool bInFallback);

		FMaterialRenderLayout Layout;
		std::vector<std::byte> UniformPayload;
		std::vector<FRHITextureReferenceRef> Resources;
		bool bFallback = false;
	};

	// Decodes the supported v1 compact binding without exposing parameter GUIDs
	// or reflected objects to Renderer.
	struct FMaterialRenderV1Binding
	{
		FVector3 BaseColor{0.95, 0.62, 0.22};
		float Opacity = 1.0f;
		float SpecularStrength = 0.35f;
		float Shininess = 32.0f;
		FRHITextureReferenceRef BaseColorTexture;
	};

	ENGINE_API auto TryGetMaterialRenderV1Binding(
		const FMaterialRenderRepresentation& Representation,
		FMaterialRenderV1Binding& OutBinding,
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

		ENGINE_API auto SetScalar(const FGuid& ParameterId, float Value) -> bool;
		ENGINE_API auto SetVector(const FGuid& ParameterId, const FVector3& Value) -> bool;
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
	ENGINE_API auto ValidateMaterialRenderLayout(
		const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic
	) -> bool;
	ENGINE_API auto UpgradeMaterialParameterSchemaVersion(
		FMaterialParameterSchemaVersion& InOutVersion,
		std::string& OutWarning,
		std::string& OutError
	) -> bool;

	// Defines base-material properties that participate in shader and pipeline identity.
	DSTRUCT()
	struct FMaterialStaticProperties
	{
		GENERATED_BODY()

		DPROPERTY()
		EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque;

		DPROPERTY()
		EMaterialShadingModel ShadingModel = EMaterialShadingModel::Lit;

		DPROPERTY()
		bool bTwoSided = false;

		DPROPERTY()
		EMaterialDepthWritePolicy DepthWritePolicy = EMaterialDepthWritePolicy::Automatic;

		// Alpha values below this threshold are discarded by masked passes.
		DPROPERTY()
		float OpacityMaskThreshold = 0.333f;

		auto operator==(const FMaterialStaticProperties&) const -> bool = default;
	};

	// Stores the scalar, vector, and texture alternatives used by reflected material parameters.
	DSTRUCT()
	struct FMaterialParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		float ScalarValue = 0.0f;

		DPROPERTY()
		FVector3 VectorValue{0.0};

		DPROPERTY()
		TObjectPtr<DTexture2D> TextureValue;

		ENGINE_API static auto MakeScalar(float Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeVector(const FVector3& Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeTexture(DTexture2D* Value) -> FMaterialParameterValue;

		auto operator==(const FMaterialParameterValue&) const -> bool = default;
	};

	// Defines stable parameter identity, default value, and editor presentation metadata.
	DSTRUCT()
	struct FMaterialParameterDefinition
	{
		GENERATED_BODY()

		// Stable identity survives display-name and ordering changes.
		DPROPERTY()
		FGuid Id;

		DPROPERTY()
		FName Name;

		// Selects which field of Value is semantically active.
		DPROPERTY()
		EMaterialParameterType Type = EMaterialParameterType::Scalar;

		DPROPERTY()
		FMaterialParameterValue Value;

		DPROPERTY()
		std::string DisplayName;

		DPROPERTY()
		FName GroupName;

		DPROPERTY()
		int32 SortOrder = 0;

		DPROPERTY()
		EMaterialParameterPresentation Presentation = EMaterialParameterPresentation::Default;

		DPROPERTY()
		bool bHasRange = false;

		// Applies only when bHasRange is true and Type is Scalar.
		DPROPERTY()
		float MinimumValue = 0.0f;

		DPROPERTY()
		float MaximumValue = 0.0f;

		// Applies only to Texture parameters.
		DPROPERTY()
		ETextureUsage TextureUsage = ETextureUsage::Color;
	};

	// Overrides one parameter by stable identifier while retaining all value alternatives.
	DSTRUCT()
	struct FMaterialParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		EMaterialParameterType Type = EMaterialParameterType::Scalar;

		DPROPERTY()
		FMaterialParameterValue Value;
	};

	// Resolution results are transient views and intentionally are not reflected or serialized.
	struct FResolvedMaterialParameter
	{
		const FMaterialParameterDefinition* Definition = nullptr;
		FMaterialParameterValue Value;
		DMaterialInterface* Source = nullptr;
		bool bHasLocalOverride = false;
	};

	namespace MaterialParameters
	{
		inline constexpr FGuid BaseColorId{0x6c4d841a, 0x88e14c35, 0xa428910e, 0xa8338339};
		inline constexpr FGuid BaseColorTextureId{0xe0588f9f, 0x2cb64c17, 0x9e4823fd, 0xbc71e936};
		inline constexpr FGuid OpacityId{0x76c3ab5f, 0x5de94104, 0xaa6d0fb6, 0xb44ab8a1};
		inline constexpr FGuid SpecularStrengthId{0xcf101cf0, 0x3ac84098, 0x9408d956, 0xe0832827};
		inline constexpr FGuid ShininessId{0x6826b694, 0xb7e84b0b, 0xa38f6f29, 0xdb804d4c};

		// FName cannot be safely initialized before the name pool, so canonical names
		// are exposed as function-local constants rather than namespace globals.
		ENGINE_API auto BaseColorName() -> const FName&;
		ENGINE_API auto BaseColorTextureName() -> const FName&;
		ENGINE_API auto OpacityName() -> const FName&;
		ENGINE_API auto SpecularStrengthName() -> const FName&;
		ENGINE_API auto ShininessName() -> const FName&;
	}

	ENGINE_API auto GetCanonicalMaterialParameterDefinitions() -> std::span<const FMaterialParameterDefinition>;
	ENGINE_API auto MakeCanonicalMaterialParameterDefinitions() -> std::vector<FMaterialParameterDefinition>;
	ENGINE_API auto ValidateCanonicalMaterialParameterDefinitions(
		std::span<const FMaterialParameterDefinition> Definitions,
		std::string& OutError
	) -> bool;
	ENGINE_API auto ValidateMaterialStaticProperties(
		const FMaterialStaticProperties& Properties,
		std::string& OutError
	) -> bool;

	// Contains the renderer-ready subset of resolved material parameters.
	struct FMaterialShaderMapIdentity
	{
		FMaterialRenderLayoutIdentity RenderLayout;
		EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque;
		EMaterialShadingModel ShadingModel = EMaterialShadingModel::Lit;
		float OpacityMaskThreshold = 0.333f;

		auto operator==(const FMaterialShaderMapIdentity&) const -> bool = default;
	};

	struct FMaterialPipelineIdentity
	{
		FMaterialShaderMapIdentity ShaderMap;
		bool bTwoSided = false;
		EMaterialDepthWritePolicy DepthWritePolicy = EMaterialDepthWritePolicy::Automatic;

		auto operator==(const FMaterialPipelineIdentity&) const -> bool = default;
	};

	struct FMaterialRenderData
	{
		FMaterialRenderRepresentation Representation;
		FVector4f BaseColor{0.95f, 0.62f, 0.22f, 1.0f};
		// Scene proxies retain only the counted stable RHI indirection.
		FRHITextureReferenceRef BaseColorTexture;
		float SpecularStrength = 0.35f;
		float Shininess = 32.0f;
		FMaterialPipelineIdentity PipelineIdentity;
	};

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
