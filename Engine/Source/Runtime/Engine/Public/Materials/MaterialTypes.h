#pragma once

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "DObject/ObjectPtr.h"
#include "Misc/Guid.h"
#include "EngineAPI.h"
#include "Texture/Texture2D.h"

#include "MaterialTypes.gen.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
	struct FMaterialProgram;
	class DMaterialInterface;

	// Selects the active storage field in a material parameter value.
	DENUM()
	enum class EMaterialParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
		// Appended to preserve the serialized values of the original alternatives.
		Vector2,
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

	// Stores the reflected alternatives used by material parameters. Type selects
	// the only semantically active field.
	DSTRUCT()
	struct FMaterialParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		float ScalarValue = 0.0f;

		DPROPERTY()
		FVector3 VectorValue{0.0};

		DPROPERTY()
		FVector2 Vector2Value{0.0};

		DPROPERTY()
		TObjectPtr<DTexture2D> TextureValue;

		ENGINE_API static auto MakeScalar(float Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeVector(const FVector3& Value) -> FMaterialParameterValue;
		ENGINE_API static auto MakeVector2(const FVector2& Value) -> FMaterialParameterValue;
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

	// Describes one reachable graph parameter declaration in deterministic first-use order.
	struct FMaterialParameterDependency
	{
		FGuid SourceNodeId;
		FGuid ParameterId;
		EMaterialParameterType Type = EMaterialParameterType::Scalar;
		uint32 FirstUseOrder = 0;
		bool bImplicitTextureRole = false;
		FName Name;
		std::string DisplayName;
		FName GroupName;
		int32 SortOrder = 0;

		auto operator==(const FMaterialParameterDependency&) const
			-> bool = default;
	};

	namespace MaterialParameters
	{
		inline constexpr FGuid BaseColorId{0x6c4d841a, 0x88e14c35, 0xa428910e, 0xa8338339};
		inline constexpr FGuid BaseColorTextureId{0xe0588f9f, 0x2cb64c17, 0x9e4823fd, 0xbc71e936};
		inline constexpr FGuid OpacityId{0x76c3ab5f, 0x5de94104, 0xaa6d0fb6, 0xb44ab8a1};
		inline constexpr FGuid SpecularStrengthId{0xcf101cf0, 0x3ac84098, 0x9408d956, 0xe0832827};
		inline constexpr FGuid ShininessId{0x6826b694, 0xb7e84b0b, 0xa38f6f29, 0xdb804d4c};
		inline constexpr FGuid NormalId{0xa21d2ef5,0x01bc40af,0x8912265f,0x401a1013};
		inline constexpr FGuid NormalTextureId{0x5555fb8e,0xc41041d9,0xb9090b80,0x60e5897d};
		inline constexpr FGuid MetallicId{0x86355aae,0x5820462d,0xbc3690b8,0x402a06d4};
		inline constexpr FGuid MetallicTextureId{0xef53c105,0x25e141e2,0x97cc521f,0xffaa7c62};
		inline constexpr FGuid RoughnessId{0xec8d8285,0xab4549b3,0xaf1321be,0xcd490348};
		inline constexpr FGuid RoughnessTextureId{0xb2a36b19,0xefbd433d,0xa4ff5687,0x02ebc864};
		inline constexpr FGuid AmbientOcclusionId{0x4ff53bc5,0x0c1c47a8,0x8453e4b2,0xa6be893c};
		inline constexpr FGuid AmbientOcclusionTextureId{0x88e38c97,0x44ac4b15,0xa203c18f,0x4e2b7d6e};
		inline constexpr FGuid EmissiveId{0x0f059660,0xe75b4f74,0x934095ee,0x0dad4764};
		inline constexpr FGuid EmissiveTextureId{0xe544e53b,0x699b4f83,0x8f0b29f7,0x18aa9d02};
		inline constexpr FGuid OpacityTextureId{0xd6e3072e,0xc97146de,0xb699855e,0x6f96c767};
		inline constexpr FGuid OpacityMaskId{0x6cd33852,0x373a4803,0xb2847fd3,0x319add4d};
		inline constexpr FGuid OpacityMaskTextureId{0xc4b39494,0xac194da8,0xb9b9beef,0x69c94797};

		inline constexpr std::array<FGuid, 8> TextureIds{BaseColorTextureId, NormalTextureId, MetallicTextureId, RoughnessTextureId, AmbientOcclusionTextureId, EmissiveTextureId, OpacityTextureId, OpacityMaskTextureId};
		inline constexpr std::array<FGuid, 8> UVChannelIds{
			FGuid{0x672ac603,0xe3b849d3,0xa0bd40a7,0x808f40d9}, FGuid{0x5bd333c3,0x4f7b4794,0x8719fbbc,0xf6c55aa5}, FGuid{0x4555094e,0x5e2146f8,0x8fa5461c,0x2855e779}, FGuid{0x5f31c554,0x120d438c,0xac871567,0xea3dfb2c},
			FGuid{0x22268e45,0x22ea4186,0x8c8032ae,0xbf3563a6}, FGuid{0xe3da1eb1,0xb9374251,0xb671d414,0x3589b22a}, FGuid{0x9390003c,0x799e47e6,0x8aa7085f,0x02682928}, FGuid{0xfe9b13ed,0x48be4534,0xae1d9f02,0x4e56aed0}};
		inline constexpr std::array<FGuid, 8> UVScaleIds{
			FGuid{0xefb7f324,0x0be949d7,0xb5e2aaf5,0xfb4e6805}, FGuid{0xa5a9c83b,0x4eb44263,0x83a69589,0xbc5c51fa}, FGuid{0xd24b6330,0xa6b94232,0xb929e02e,0xee5eb8cb}, FGuid{0x52b3dde0,0x3355417b,0xbf05eb11,0xa4d57d74},
			FGuid{0x8cd74420,0x60764ea4,0x88fb76ac,0xc08804d4}, FGuid{0xb9e82178,0x3fcd43e7,0x94aa7826,0x15b81866}, FGuid{0x15e6d53d,0x890241ca,0x915eb4d1,0x32b0caa0}, FGuid{0x89485eda,0xbf1d448a,0x8142d9b3,0xcc7705d1}};
		inline constexpr std::array<FGuid, 8> UVOffsetIds{
			FGuid{0x7f06899b,0x33f5416d,0x9d07e4b0,0x86d9f512}, FGuid{0xd8f1ff6d,0x0da845d3,0xb263bf33,0x6b268992}, FGuid{0x823917fc,0x577e4492,0xaee15bf5,0x1f7f99c9}, FGuid{0xe8c9892e,0xfe2c471b,0xb76eeef3,0xd38a0eab},
			FGuid{0xfcc40232,0xb6604de4,0x95123d02,0xe05dde5e}, FGuid{0x165e8be8,0x46a44106,0xb22d3a0f,0x25bd23cb}, FGuid{0xad888dbb,0x10934047,0x82901991,0x3f0ea763}, FGuid{0xefb2320e,0x8b514460,0xb3e92d3a,0x973d358a}};
		inline constexpr std::array<FGuid, 8> UVRotationIds{
			FGuid{0x35f1f695,0xc8bb4c59,0x89f55c74,0x8e297b22}, FGuid{0xef2664e3,0xf45b4f20,0xaad6baa4,0x6486f63f}, FGuid{0x3c598714,0x16174535,0x936eb4db,0xe5a210cd}, FGuid{0x682ca789,0x18bd4ec4,0xa00f271a,0xd7527e59},
			FGuid{0x82b3fdc3,0x0f8840ad,0xbe12abdb,0xbc5732b1}, FGuid{0x76c5afe3,0xd08148cb,0x86c9125d,0x8accbce1}, FGuid{0x5751ef57,0xf71d45e4,0x906ce613,0x9d22b2c4}, FGuid{0x4a40ca6b,0xa7fe48ae,0xb2af647a,0x7027f949}};
		inline constexpr std::array<FGuid, 8> SamplerStateIds{
			FGuid{0xc907df56,0x6d4c440b,0xa6dd53c9,0x6333f11d}, FGuid{0xd378b044,0x9d8b439d,0xaf82bac2,0x3476970b}, FGuid{0x44ef5b74,0xbdcb4fae,0xb233f499,0xcc99814e}, FGuid{0x71086444,0x963c4638,0x98d929c1,0xaaab3dc2},
			FGuid{0x6075e231,0x6c8647db,0x815a4575,0xa72a06ca}, FGuid{0xa89564f2,0xd936422a,0xa48871f7,0x21873f76}, FGuid{0xc98a80bb,0x03cb4ed9,0x80fe2dfc,0xb6a89821}, FGuid{0x666771a5,0x71034a00,0xaf2a479d,0x097947b8}};

		// FName cannot be safely initialized before the name pool, so canonical names
		// are exposed as function-local constants rather than namespace globals.
		ENGINE_API auto BaseColorName() -> const FName&;
		ENGINE_API auto BaseColorTextureName() -> const FName&;
		ENGINE_API auto OpacityName() -> const FName&;
		ENGINE_API auto NormalName() -> const FName&;
		ENGINE_API auto NormalTextureName() -> const FName&;
		ENGINE_API auto MetallicName() -> const FName&;
		ENGINE_API auto MetallicTextureName() -> const FName&;
		ENGINE_API auto RoughnessName() -> const FName&;
		ENGINE_API auto RoughnessTextureName() -> const FName&;
		ENGINE_API auto AmbientOcclusionName() -> const FName&;
		ENGINE_API auto AmbientOcclusionTextureName() -> const FName&;
		ENGINE_API auto EmissiveName() -> const FName&;
		ENGINE_API auto EmissiveTextureName() -> const FName&;
		ENGINE_API auto OpacityTextureName() -> const FName&;
		ENGINE_API auto OpacityMaskName() -> const FName&;
		ENGINE_API auto OpacityMaskTextureName() -> const FName&;
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
	ENGINE_API auto InspectMaterialParameterDependencies(
		const FMaterialProgram& Program,
		std::span<const FMaterialParameterDefinition> Definitions)
		-> std::vector<FMaterialParameterDependency>;

}
