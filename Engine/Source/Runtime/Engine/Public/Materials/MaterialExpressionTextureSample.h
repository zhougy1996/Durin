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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {Opcode, "Texture Sample 2D", "Samples a connected texture resource. For a standalone replaceable texture, use Texture Sample Parameter 2D.", "Textures"};
		}
		ENGINE_API auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetTextureSampleReadMode() -> EMaterialTextureReadMode* override { return &ReadMode; }
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	private:
		static constexpr auto Opcode = EMaterialProgramOpcode::TextureSample2D;
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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {Opcode, "Texture Sample Cube", "Samples a connected Cube texture object with a Float3 direction.", "Textures"};
		}
		ENGINE_API auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetTextureSampleReadMode() -> EMaterialTextureReadMode* override { return &ReadMode; }
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	private:
		static constexpr auto Opcode = EMaterialProgramOpcode::TextureSampleCube;
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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {Opcode, "Texture Sample Parameter 2D", "Samples a named texture parameter with mesh UV0 or a connected Float2 UV expression. Outputs share one fetch.", "Parameters"};
		}
		ENGINE_API auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetTextureSampleReadMode() -> EMaterialTextureReadMode* override { return &ReadMode; }
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	private:
		static constexpr auto Opcode = EMaterialProgramOpcode::TextureSampleParameter2D;
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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {Opcode, "Texture Sample Parameter Cube", "Samples a named Cube texture with a Float3 direction.", "Parameters"};
		}
		ENGINE_API auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetTextureSampleReadMode() -> EMaterialTextureReadMode* override { return &ReadMode; }
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	private:
		static constexpr auto Opcode = EMaterialProgramOpcode::TextureSampleParameterCube;
	};
}
