#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"

#include "MaterialExpressionTextureSample.gen.h"

namespace Durin
{
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
		FMaterialNumericInput UV{2};

		// Inactive operands retain authored intent but are not lowered or evaluated.
		DPROPERTY(Edit)
		EMaterialTextureReadMode ReadMode = EMaterialTextureReadMode::Implicit;

		DPROPERTY()
		FMaterialNumericInput MipValue{1};

		DPROPERTY()
		FMaterialNumericInput DDX{2};

		DPROPERTY()
		FMaterialNumericInput DDY{2};

		auto GetAuthoredInputCount() const -> uint32 override { return 5; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Samples a connected resource using a Float3 direction source.
	DCLASS()
	class DMaterialExpressionTextureSampleCube : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureSampleCube(const FObjectInitializer& Initializer) : Super(Initializer) { Direction.SetConstant({1.f, 0.f, 0.f}); }

		DPROPERTY()
		FMaterialExpressionInput Texture;

		DPROPERTY()
		FMaterialNumericInput Direction{3};

		// Inactive operands retain authored intent but are not lowered or evaluated.
		DPROPERTY(Edit)
		EMaterialTextureReadMode ReadMode = EMaterialTextureReadMode::Implicit;

		DPROPERTY()
		FMaterialNumericInput MipValue{1};

		DPROPERTY()
		FMaterialNumericInput DDX{3};

		DPROPERTY()
		FMaterialNumericInput DDY{3};

		auto GetAuthoredInputCount() const -> uint32 override { return 5; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// A combined resource owner adds only UV sampling state.
	DCLASS()
	class DMaterialExpressionTextureSampleParameter2D : public DMaterialExpressionTextureParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureSampleParameter2D(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialNumericInput UV{2};

		// Inactive operands retain authored intent but are not lowered or evaluated.
		DPROPERTY(Edit)
		EMaterialTextureReadMode ReadMode = EMaterialTextureReadMode::Implicit;

		DPROPERTY()
		FMaterialNumericInput MipValue{1};

		DPROPERTY()
		FMaterialNumericInput DDX{2};

		DPROPERTY()
		FMaterialNumericInput DDY{2};

		auto GetAuthoredInputCount() const -> uint32 override { return 4; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// A combined resource owner owns Cube direction sampling state.
	DCLASS()
	class DMaterialExpressionTextureSampleParameterCube : public DMaterialExpressionTextureParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureSampleParameterCube(const FObjectInitializer& Initializer) : Super(Initializer) { DefaultValue.Dimension = EMaterialTextureDimension::TextureCube; Direction.SetConstant({1.f, 0.f, 0.f}); }

		DPROPERTY()
		FMaterialNumericInput Direction{3};

		// Inactive operands retain authored intent but are not lowered or evaluated.
		DPROPERTY(Edit)
		EMaterialTextureReadMode ReadMode = EMaterialTextureReadMode::Implicit;

		DPROPERTY()
		FMaterialNumericInput MipValue{1};

		DPROPERTY()
		FMaterialNumericInput DDX{3};

		DPROPERTY()
		FMaterialNumericInput DDY{3};

		auto GetAuthoredInputCount() const -> uint32 override { return 4; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
