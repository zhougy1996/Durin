#pragma once

#include "Materials/MaterialDiagnostic.h"

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "DObject/ObjectPtr.h"
#include "Misc/Guid.h"
#include "EngineAPI.h"
#include "Texture/Texture2D.h"

#include "MaterialTypes.gen.h"

#include <variant>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Durin
{
	class DMaterialInterface;

	// Logical parameter type used in declarations and serialized records.
	DENUM()
	enum class EMaterialParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
		// Appended to preserve the serialized values of the original alternatives.
		Vector2,
		Vector4,
	};

	// Selects editor presentation without changing the parameter's runtime type.
	DENUM()
	enum class EMaterialParameterPresentation : uint8
	{
		Default,
		Drag,
		Integer,
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

	// Stores independent authored intent; disabled fields retain their values for later edits.
	DSTRUCT()
	struct FMaterialPropertyOverrides
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		bool bOverrideBlendMode = false;

		DPROPERTY(Edit)
		bool bOverrideShadingModel = false;

		DPROPERTY(Edit)
		bool bOverrideOpacityMaskThreshold = false;

		DPROPERTY(Edit)
		bool bOverrideTwoSided = false;

		DPROPERTY(Edit)
		bool bOverrideDepthWritePolicy = false;

		DPROPERTY(Edit)
		FMaterialStaticProperties Values;

		auto HasAnyOverride() const -> bool
		{
			return bOverrideBlendMode || bOverrideShadingModel || bOverrideOpacityMaskThreshold
				|| bOverrideTwoSided || bOverrideDepthWritePolicy;
		}
		ENGINE_API auto ApplyTo(FMaterialStaticProperties& Properties) const -> void;
		auto operator==(const FMaterialPropertyOverrides&) const -> bool = default;
	};

	// Validate authored values first. This projection never changes authored inactive values.
	ENGINE_API auto CanonicalizeMaterialShaderProperties(FMaterialStaticProperties Properties)
		-> FMaterialStaticProperties;

	// Bounded sampling state belongs to texture values and instance overrides.
	DENUM()
	enum class EMaterialSamplerMinFilter : uint8
	{
		Nearest,
		Linear,
		NearestMipmapNearest,
		LinearMipmapNearest,
		NearestMipmapLinear,
		LinearMipmapLinear,
	};
	DENUM()
	enum class EMaterialSamplerMagFilter : uint8 { Nearest, Linear };
	DENUM()
	enum class EMaterialSamplerAddressMode : uint8
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge,
	};

	DSTRUCT()
	struct FMaterialSamplerState
	{
		GENERATED_BODY()
		DPROPERTY()
		EMaterialSamplerMinFilter MinFilter = EMaterialSamplerMinFilter::LinearMipmapLinear;
		DPROPERTY()
		EMaterialSamplerMagFilter MagFilter = EMaterialSamplerMagFilter::Linear;
		DPROPERTY()
		EMaterialSamplerAddressMode AddressU = EMaterialSamplerAddressMode::Repeat;
		DPROPERTY()
		EMaterialSamplerAddressMode AddressV = EMaterialSamplerAddressMode::Repeat;

		auto operator==(const FMaterialSamplerState&) const -> bool = default;
	};

	DENUM()
	enum class EMaterialTextureFallback : uint8
	{
		White,
		Black,
		FlatRGNormal,
	};

	ENGINE_API auto IsValidMaterialSampling(FMaterialSamplerState State, EMaterialTextureFallback Fallback) -> bool;

	// A texture override owns its complete sampling policy.
	DSTRUCT()
	struct FMaterialTextureValue
	{
		GENERATED_BODY()

		DPROPERTY()
		TObjectPtr<DTexture2D> Texture;

		DPROPERTY()
		FMaterialSamplerState SamplerState;

		DPROPERTY()
		EMaterialTextureFallback TextureFallback = EMaterialTextureFallback::White;

		auto operator==(const FMaterialTextureValue&) const -> bool = default;
	};

	// Transient selected value. Accessors require the matching alternative.
	class FMaterialParameterValue
	{
	public:
		ENGINE_API auto GetType() const -> EMaterialParameterType;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void;
		auto GetScalar() const -> const float& { require(std::holds_alternative<float>(Value)); return std::get<float>(Value); }
		auto GetScalar() -> float& { require(std::holds_alternative<float>(Value)); return std::get<float>(Value); }
		auto GetVector2() const -> const FVector2& { require(std::holds_alternative<FVector2>(Value)); return std::get<FVector2>(Value); }
		auto GetVector2() -> FVector2& { require(std::holds_alternative<FVector2>(Value)); return std::get<FVector2>(Value); }
		auto GetVector() const -> const FVector3& { require(std::holds_alternative<FVector3>(Value)); return std::get<FVector3>(Value); }
		auto GetVector() -> FVector3& { require(std::holds_alternative<FVector3>(Value)); return std::get<FVector3>(Value); }
		auto GetVector4() const -> const FVector4& { require(std::holds_alternative<FVector4>(Value)); return std::get<FVector4>(Value); }
		auto GetVector4() -> FVector4& { require(std::holds_alternative<FVector4>(Value)); return std::get<FVector4>(Value); }
		auto GetTexture() const -> const FMaterialTextureValue& { require(std::holds_alternative<FMaterialTextureValue>(Value)); return std::get<FMaterialTextureValue>(Value); }
		auto GetTexture() -> FMaterialTextureValue& { require(std::holds_alternative<FMaterialTextureValue>(Value)); return std::get<FMaterialTextureValue>(Value); }

		ENGINE_API static auto MakeScalar(float Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeVector2(const FVector2& Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeVector(const FVector3& Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeVector4(const FVector4& Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeTexture(DTexture2D* Value, FMaterialSamplerState Sampler = {},
			EMaterialTextureFallback Fallback = EMaterialTextureFallback::White) -> FMaterialParameterValue;
		auto operator==(const FMaterialParameterValue&) const -> bool = default;
	private:
		std::variant<float, FVector2, FVector3, FVector4, FMaterialTextureValue> Value = 0.0f;
	};

	// Derived parameter view; pointers expire when the owner revision changes.
	struct FMaterialParameterDefinition
	{

		// Stable identity survives display-name and ordering changes.
		FGuid Id;

		FName Name;

		// Derived declaration type; validation requires agreement with Value.GetType().
		EMaterialParameterType Type = EMaterialParameterType::Scalar;

		FMaterialParameterValue Value;

		std::string DisplayName;

		FName GroupName;

		int32 SortOrder = 0;

		EMaterialParameterPresentation Presentation = EMaterialParameterPresentation::Default;

		bool bHasRange = false;

		// Applies only when bHasRange is true and Type is Scalar.
		float MinimumValue = 0.0f;

		float MaximumValue = 0.0f;

		// Applies only to Texture parameters.
		ETextureUsage TextureUsage = ETextureUsage::Color;

		auto operator==(const FMaterialParameterDefinition&) const -> bool = default;
	};

	// Editor-only generated recipe and source/output identity; contains no value history.
	DSTRUCT()
	struct FMaterialImportProvenance
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string RecipeId;

		DPROPERTY()
		uint32 RecipeVersion = 0;

		DPROPERTY()
		std::string StructuralKey;

		DPROPERTY()
		std::string SourceIdentity;

		DPROPERTY()
		std::string OutputIdentity;

		auto operator==(const FMaterialImportProvenance&) const -> bool = default;
	};

	// Resolution results are transient views and intentionally are not reflected or serialized.
	struct FResolvedMaterialParameter
	{
		const FMaterialParameterDefinition* Definition = nullptr;
		FMaterialParameterValue Value;
		DMaterialInterface* Source = nullptr;
		bool bHasLocalOverride = false;
	};

	inline constexpr uint32 MaterialMaxParameterDefinitionCount = 128;
	inline constexpr uint32 MaterialMaxParameterTextBytes = 128;

	enum class EMaterialParameterError : uint8
	{
		None,
		TooManyDefinitions,
		InvalidId,
		DuplicateId,
		InvalidName,
		DuplicateName,
		InvalidText,
		InvalidType,
		InvalidDefault,
		InvalidMetadata,
		NotFound,
		TypeConflict,
		OwnerMissing,
		OwnerMismatch,
		Unreachable,
		OverrideType,
		UnresolvedValue,
		UnsupportedProgramSchema,
		InvalidProgram,
	};

	struct FMaterialParameterValidationResult
	{
		EMaterialParameterError Error = EMaterialParameterError::None;
		FGuid ParameterId;
		explicit operator bool() const { return Error == EMaterialParameterError::None; }
	};

	// Format only at a diagnostic presentation or logging boundary.
	ENGINE_API auto GetMaterialParameterErrorText(EMaterialParameterError Error) -> std::string_view;
	// Validates material-owned authored declarations.
	ENGINE_API auto ValidateMaterialParameterDefinitions(
		std::span<const FMaterialParameterDefinition> Definitions) -> FMaterialParameterValidationResult;
	[[nodiscard]] ENGINE_API auto ValidateMaterialStaticProperties(
		const FMaterialStaticProperties& Properties) -> FMaterialOperationResult;

}
