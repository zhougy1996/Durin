#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"
#include "Misc/EnumClassFlags.h"
#include "Texture/Texture2D.h"

#include "MaterialTypes.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class FTexture2DRenderResource;

	DENUM()
	enum class EMaterialParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
	};

	DENUM()
	enum class EMaterialParameterPresentation : uint8
	{
		Default,
		Drag,
		Color,
		AssetPicker,
	};

	DSTRUCT()
	struct ENGINE_API FMaterialParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		float ScalarValue = 0.0f;

		DPROPERTY()
		FVector3 VectorValue{0.0};

		DPROPERTY()
		TObjectPtr<DTexture2D> TextureValue;

		static auto MakeScalar(float Value) -> FMaterialParameterValue;
		static auto MakeVector(const FVector3& Value) -> FMaterialParameterValue;
		static auto MakeTexture(DTexture2D* Value) -> FMaterialParameterValue;

		auto operator==(const FMaterialParameterValue&) const -> bool = default;
	};

	DSTRUCT()
	struct ENGINE_API FMaterialParameterDefinition
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid Id;

		DPROPERTY()
		FName Name;

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

		DPROPERTY()
		float MinimumValue = 0.0f;

		DPROPERTY()
		float MaximumValue = 0.0f;

		DPROPERTY()
		ETextureUsage TextureUsage = ETextureUsage::Color;
	};

	DSTRUCT()
	struct ENGINE_API FMaterialParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

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

	struct FMaterialRenderData
	{
		FVector4f BaseColor{0.95f, 0.62f, 0.22f, 1.0f};
		// Scene proxies retain only the thread-safe resource proxy; reflected assets stay on the game thread.
		std::shared_ptr<FTexture2DRenderResource> BaseColorTexture;
		float SpecularStrength = 0.35f;
		float Shininess = 32.0f;
	};

	enum class EMaterialRenderDirtyFlags : uint8
	{
		None = 0,
		ParameterValues = 1 << 0,
		ParentChain = 1 << 1
	};
	ENUM_CLASS_FLAGS(EMaterialRenderDirtyFlags);

	struct FMaterialRenderUpdate
	{
		uint32 SlotIndex = 0;
		FMaterialRenderData RenderData;
		uint64 MaterialVersion = 0;
		uint64 ComponentRevision = 0;
		EMaterialRenderDirtyFlags DirtyFlags = EMaterialRenderDirtyFlags::None;
	};
}
