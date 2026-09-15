#pragma once

#include "DObject/Object.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialTypes.h"

#include "MaterialExpressions.gen.h"

namespace Durin
{
	class FMaterialExpressionBuildContext;
	struct FMaterialExpressionBuildValue;
	// Nodes sharing a parameter ID must retain identical parameter definitions.
	DSTRUCT()
	struct FMaterialParameterMetadata
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid Id;

		DPROPERTY()
		FName Name;

		DPROPERTY()
		std::string DisplayName;

		DPROPERTY()
		FName GroupName;

		DPROPERTY()
		int32 SortOrder = 0;

		DPROPERTY()
		EMaterialParameterPresentation Presentation = EMaterialParameterPresentation::Default;

		auto operator==(const FMaterialParameterMetadata&) const -> bool = default;
	};

	// Package-internal expression identity. Family-specific state lives in derived types.
	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialExpression : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpression(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		virtual auto GetAuthoredInputCount() const -> uint32 { return 0; }

		DPROPERTY()
		FGuid Id;

		// Emits detached IR directly through the owning-thread build context.
		ENGINE_API virtual auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue = 0;

	};

	// The asset owns every expression referenced by this collection.
	DSTRUCT()
	struct FMaterialExpressionCollection
	{
		GENERATED_BODY()

		DPROPERTY()
		std::vector<TObjectPtr<DMaterialExpression>> Expressions;
	};

	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialExpressionParameter : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionParameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialParameterMetadata Metadata;

		virtual auto GetParameterDefinition() const -> FMaterialParameterDefinition = 0;

		// Applies compatible shared fields without changing node identity or local sampling inputs.
		ENGINE_API auto SetParameterDefinition(const FMaterialParameterDefinition& Definition) -> bool;

	protected:
		ENGINE_API auto MakeDefinition(EMaterialParameterType Type, FMaterialParameterValue Value) const
			-> FMaterialParameterDefinition;
	};

	// Owns a concrete scalar constant value.
	DCLASS()
	class DMaterialExpressionScalarConstant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionScalarConstant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		float Value = 0.0f;

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns a concrete scalar parameter value.
	DCLASS()
	class DMaterialExpressionScalarParameter : public DMaterialExpressionParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionScalarParameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		float DefaultValue = 0.0f;

		DPROPERTY()
		bool bHasRange = false;

		DPROPERTY()
		float MinimumValue = 0.0f;

		DPROPERTY()
		float MaximumValue = 0.0f;

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns a concrete vector2 constant value.
	DCLASS()
	class DMaterialExpressionVector2Constant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector2Constant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FVector2 Value{0.0};

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns a concrete vector3 constant value.
	DCLASS()
	class DMaterialExpressionVector3Constant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector3Constant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FVector3 Value{0.0};

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns a concrete vector4 constant value.
	DCLASS()
	class DMaterialExpressionVector4Constant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector4Constant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FVector4 Value{0.0};

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns a concrete vector4 parameter value.
	DCLASS()
	class DMaterialExpressionVector4Parameter : public DMaterialExpressionParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector4Parameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FVector4 DefaultValue{0.0};

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// A resource parameter owns texture sampling policy and its applicable usage hint.
	DCLASS()
	class DMaterialExpressionTextureParameter : public DMaterialExpressionParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureParameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialTextureValue DefaultValue;

		DPROPERTY()
		ETextureUsage TextureUsage = ETextureUsage::Color;

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Graph connections carry identity only; numeric defaults belong to their expression.
	DSTRUCT()
	struct FMaterialExpressionInput
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ExpressionId;

		DPROPERTY()
		uint8 OutputIndex = 0;

		DPROPERTY()
		FGuid OutputId;

		auto operator==(const FMaterialExpressionInput&) const -> bool = default;
	};

	// Material terminal pins retain concrete defaults independently of connections.
	DSTRUCT()
	struct FMaterialExpressionSurfaceOutputs
	{
		GENERATED_BODY()

		DPROPERTY()
		FMaterialExpressionInput Surface;

		DPROPERTY()
		FMaterialExpressionInput BaseColor;

		DPROPERTY()
		FMaterialExpressionInput Normal;

		DPROPERTY()
		FMaterialExpressionInput Metallic;

		DPROPERTY()
		FMaterialExpressionInput Roughness;

		DPROPERTY()
		FMaterialExpressionInput AmbientOcclusion;

		DPROPERTY()
		FMaterialExpressionInput Emissive;

		DPROPERTY()
		FMaterialExpressionInput Opacity;

		DPROPERTY()
		FMaterialExpressionInput OpacityMask;

		DPROPERTY()
		FVector3 BaseColorDefault{0.5};

		DPROPERTY()
		FVector3 NormalDefault{0.0, 0.0, 1.0};

		DPROPERTY()
		float MetallicDefault = 0.0f;

		DPROPERTY()
		float RoughnessDefault = 0.5f;

		DPROPERTY()
		float AmbientOcclusionDefault = 1.0f;

		DPROPERTY()
		FVector3 EmissiveDefault{0.0};

		DPROPERTY()
		float OpacityDefault = 1.0f;

		DPROPERTY()
		float OpacityMaskDefault = 1.0f;

		auto operator==(const FMaterialExpressionSurfaceOutputs&) const -> bool = default;
	};

	// Groups numeric expressions; concrete families own their applicable pins and defaults.
	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialExpressionNumeric : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionNumeric(const FObjectInitializer& Initializer) : Super(Initializer) {}
	};

	// Owns only the inputs and width required by Add.
	DCLASS()
	class DMaterialExpressionAdd : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionAdd(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Subtract.
	DCLASS()
	class DMaterialExpressionSubtract : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSubtract(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Multiply.
	DCLASS()
	class DMaterialExpressionMultiply : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMultiply(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Divide.
	DCLASS()
	class DMaterialExpressionDivide : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionDivide(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Minimum.
	DCLASS()
	class DMaterialExpressionMinimum : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMinimum(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Maximum.
	DCLASS()
	class DMaterialExpressionMaximum : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMaximum(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Negate.
	DCLASS()
	class DMaterialExpressionNegate : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionNegate(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by OneMinus.
	DCLASS()
	class DMaterialExpressionOneMinus : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionOneMinus(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Absolute.
	DCLASS()
	class DMaterialExpressionAbsolute : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionAbsolute(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Saturate.
	DCLASS()
	class DMaterialExpressionSaturate : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSaturate(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Normalize.
	DCLASS()
	class DMaterialExpressionNormalize : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionNormalize(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Sine.
	DCLASS()
	class DMaterialExpressionSine : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSine(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Cosine.
	DCLASS()
	class DMaterialExpressionCosine : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCosine(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Clamp.
	DCLASS()
	class DMaterialExpressionClamp : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionClamp(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		DPROPERTY()
		FMaterialExpressionInput Minimum;

		DPROPERTY()
		std::vector<float> MinimumDefault;

		DPROPERTY()
		FMaterialExpressionInput Maximum;

		DPROPERTY()
		std::vector<float> MaximumDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 3; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Lerp.
	DCLASS()
	class DMaterialExpressionLerp : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionLerp(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput A;

		DPROPERTY()
		std::vector<float> ADefault;

		DPROPERTY()
		FMaterialExpressionInput B;

		DPROPERTY()
		std::vector<float> BDefault;

		DPROPERTY()
		FMaterialExpressionInput Alpha;

		DPROPERTY()
		std::vector<float> AlphaDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 3; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by MakeFloat2.
	DCLASS()
	class DMaterialExpressionMakeVector2 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeVector2(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput X;

		DPROPERTY()
		std::vector<float> XDefault;

		DPROPERTY()
		FMaterialExpressionInput Y;

		DPROPERTY()
		std::vector<float> YDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by MakeFloat3.
	DCLASS()
	class DMaterialExpressionMakeVector3 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeVector3(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput X;

		DPROPERTY()
		std::vector<float> XDefault;

		DPROPERTY()
		FMaterialExpressionInput Y;

		DPROPERTY()
		std::vector<float> YDefault;

		DPROPERTY()
		FMaterialExpressionInput Z;

		DPROPERTY()
		std::vector<float> ZDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 3; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by MakeFloat4.
	DCLASS()
	class DMaterialExpressionMakeVector4 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeVector4(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput X;

		DPROPERTY()
		std::vector<float> XDefault;

		DPROPERTY()
		FMaterialExpressionInput Y;

		DPROPERTY()
		std::vector<float> YDefault;

		DPROPERTY()
		FMaterialExpressionInput Z;

		DPROPERTY()
		std::vector<float> ZDefault;

		DPROPERTY()
		FMaterialExpressionInput W;

		DPROPERTY()
		std::vector<float> WDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 4; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Splat2.
	DCLASS()
	class DMaterialExpressionSplat2 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSplat2(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Splat3.
	DCLASS()
	class DMaterialExpressionSplat3 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSplat3(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by Splat4.
	DCLASS()
	class DMaterialExpressionSplat4 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSplat4(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by DecodeNormalRG.
	DCLASS()
	class DMaterialExpressionDecodeNormalRG : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionDecodeNormalRG(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by BlendNormalsRNM.
	DCLASS()
	class DMaterialExpressionBlendNormalsRNM : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionBlendNormalsRNM(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Base;

		DPROPERTY()
		std::vector<float> BaseDefault;

		DPROPERTY()
		FMaterialExpressionInput Detail;

		DPROPERTY()
		std::vector<float> DetailDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns only the inputs and width required by UVChannel.
	DCLASS()
	class DMaterialExpressionUVChannel : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionUVChannel(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Channel;

		DPROPERTY()
		std::vector<float> ChannelDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Concatenates numeric inputs; the output width follows their combined widths.
	DCLASS()
	class DMaterialExpressionAppendVector : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionAppendVector(const FObjectInitializer& Initializer) : Super(Initializer) {}
		DPROPERTY()
		FMaterialExpressionInput A;
		DPROPERTY()
		std::vector<float> ADefault{0.f};
		DPROPERTY()
		FMaterialExpressionInput B;
		DPROPERTY()
		std::vector<float> BDefault{0.f};
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float2;
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;
	};

	// Component selection supplies the swizzle's output width.
	DCLASS()
	class DMaterialExpressionSwizzle : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSwizzle(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;

		DPROPERTY()
		std::vector<uint8> Components{0};

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// A call binding retains a numeric default only when present.
	DSTRUCT()
	struct FMaterialExpressionFunctionInputBinding
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid InputId;

		DPROPERTY()
		EMaterialProgramValueType ExpectedType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;
	};

	// Samples a connected resource using a Float2 UV source or mesh UV0.
	DCLASS()
	class DMaterialExpressionTextureSample2D : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureSample2D(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Texture;

		DPROPERTY()
		FMaterialExpressionInput UV;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// A combined resource owner adds only UV sampling state.
	DCLASS()
	class DMaterialExpressionTextureSampleParameter2D : public DMaterialExpressionTextureParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureSampleParameter2D(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput UV;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Reads one mesh UV channel as a Float2; transforms belong to upstream math nodes.
	DCLASS()
	class DMaterialExpressionTextureCoordinates : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureCoordinates(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Channel;

		DPROPERTY()
		std::vector<float> ChannelDefault{0.f};

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Builds eight surface attributes from their applicable numeric inputs.
	DCLASS()
	class DMaterialExpressionMakeSurface : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeSurface(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput BaseColor;

		DPROPERTY()
		std::vector<float> BaseColorDefault;

		DPROPERTY()
		FMaterialExpressionInput Normal;

		DPROPERTY()
		std::vector<float> NormalDefault;

		DPROPERTY()
		FMaterialExpressionInput Metallic;

		DPROPERTY()
		std::vector<float> MetallicDefault;

		DPROPERTY()
		FMaterialExpressionInput Roughness;

		DPROPERTY()
		std::vector<float> RoughnessDefault;

		DPROPERTY()
		FMaterialExpressionInput AmbientOcclusion;

		DPROPERTY()
		std::vector<float> AmbientOcclusionDefault;

		DPROPERTY()
		FMaterialExpressionInput Emissive;

		DPROPERTY()
		std::vector<float> EmissiveDefault;

		DPROPERTY()
		FMaterialExpressionInput Opacity;

		DPROPERTY()
		std::vector<float> OpacityDefault;

		DPROPERTY()
		FMaterialExpressionInput OpacityMask;

		DPROPERTY()
		std::vector<float> OpacityMaskDefault;

		auto GetAuthoredInputCount() const -> uint32 override { return 8; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Selects stable output attributes from one aggregate surface.
	DCLASS()
	class DMaterialExpressionGetSurfaceAttributes : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionGetSurfaceAttributes(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Surface;

		DPROPERTY()
		uint8 AttributeMask = 0xff;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	DSTRUCT()
	struct FMaterialExpressionSurfaceAttributeBinding
	{
		GENERATED_BODY()

		DPROPERTY()
		EMaterialSurfaceOutput Attribute = EMaterialSurfaceOutput::BaseColor;

		DPROPERTY()
		FMaterialExpressionInput Source;

		auto operator==(const FMaterialExpressionSurfaceAttributeBinding&) const -> bool = default;
	};

	// Overrides only selected attributes on one aggregate surface.
	DCLASS()
	class DMaterialExpressionSetSurfaceAttributes : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSetSurfaceAttributes(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialExpressionInput Surface;

		DPROPERTY()
		std::vector<FMaterialExpressionSurfaceAttributeBinding> Attributes;

		auto GetAuthoredInputCount() const -> uint32 override { return 1 + static_cast<uint32>(Attributes.size()); }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// The owning function signature supplies terminal type and presentation.
	DCLASS()
	class DMaterialExpressionFunctionInput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFunctionInput(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FGuid PortId;

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// The owning function signature supplies terminal type and presentation.
	DCLASS()
	class DMaterialExpressionFunctionOutput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFunctionOutput(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FGuid PortId;

		DPROPERTY()
		FMaterialExpressionInput Source;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

	// Owns its callee and GUID-keyed port bindings inside the graph.
	DCLASS()
	class DMaterialExpressionFunctionCall : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFunctionCall(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY(EditorOnly)
		TObjectPtr<DMaterialFunctionInterface> Function;

		DPROPERTY()
		std::vector<FMaterialExpressionFunctionInputBinding> Inputs;

		DPROPERTY()
		std::vector<FMaterialFunctionOutputBinding> Outputs;

		auto GetAuthoredInputCount() const -> uint32 override { return static_cast<uint32>(Inputs.size()); }

		ENGINE_API auto Build(FMaterialExpressionBuildContext& Context,
			uint8 OutputIndex = 0, FGuid OutputId = {}) const -> FMaterialExpressionBuildValue override;

	};

}
