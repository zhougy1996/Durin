#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionNumeric.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionVector.gen.h"

namespace Durin
{
	// Owns only the inputs and width required by Normalize.
	DCLASS()
	class DMaterialExpressionNormalize : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionNormalize(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Normalize, .Name = "Normalize",
				.Description = "Returns a unit-length vector.",
				.Category = "Math", .PaletteDefault = EMaterialProgramValueType::Float2};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	DCLASS()
	class DMaterialExpressionDot : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionDot(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Dot, .Name = "Dot Product",
				.Description = "Returns the scalar dot product of equal-width vectors.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		DPROPERTY()
		FMaterialNumericInput A{2};
		DPROPERTY()
		FMaterialNumericInput B{2};
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionCross : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCross(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Cross, .Name = "Cross Product",
				.Description = "Returns the cross product of two Float3 values.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		DPROPERTY()
		FMaterialNumericInput A{3};
		DPROPERTY()
		FMaterialNumericInput B{3};
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionLength : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionLength(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Length, .Name = "Length",
				.Description = "Returns the scalar length of a vector.",
				.Category = "Math"};
		}
		DPROPERTY()
		FMaterialNumericInput Input{2};
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionDistance : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionDistance(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Distance, .Name = "Distance",
				.Description = "Returns the scalar distance between equal-width values.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		DPROPERTY()
		FMaterialNumericInput A{2};
		DPROPERTY()
		FMaterialNumericInput B{2};
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	// Owns only the inputs and width required by MakeFloat2.
	DCLASS()
	class DMaterialExpressionMakeVector2 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeVector2(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::MakeFloat2, .Name = "Make Vector",
				.Description = "Combines scalar inputs into a vector.",
				.Category = "Channels", .InputNames = {"X", "Y"}, .bShowInPalette = false};
		}

		DPROPERTY()
		FMaterialNumericInput X;

		DPROPERTY()
		FMaterialNumericInput Y;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by MakeFloat3.
	DCLASS()
	class DMaterialExpressionMakeVector3 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeVector3(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::MakeFloat3, .Name = "Make Vector",
				.Description = "Combines scalar inputs into a vector.",
				.Category = "Channels", .InputNames = {"X", "Y", "Z"}, .bShowInPalette = false};
		}

		DPROPERTY()
		FMaterialNumericInput X;

		DPROPERTY()
		FMaterialNumericInput Y;

		DPROPERTY()
		FMaterialNumericInput Z;

		auto GetAuthoredInputCount() const -> uint32 override { return 3; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by MakeFloat4.
	DCLASS()
	class DMaterialExpressionMakeVector4 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMakeVector4(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::MakeFloat4, .Name = "Make Vector",
				.Description = "Combines scalar inputs into a vector.",
				.Category = "Channels", .InputNames = {"X", "Y", "Z", "W"}, .bShowInPalette = false};
		}

		DPROPERTY()
		FMaterialNumericInput X;

		DPROPERTY()
		FMaterialNumericInput Y;

		DPROPERTY()
		FMaterialNumericInput Z;

		DPROPERTY()
		FMaterialNumericInput W;

		auto GetAuthoredInputCount() const -> uint32 override { return 4; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Splat2.
	DCLASS()
	class DMaterialExpressionSplat2 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSplat2(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Splat2, .Name = "Splat",
				.Description = "Replicates a scalar across vector components.",
				.Category = "Channels", .bShowInPalette = false};
		}

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Splat3.
	DCLASS()
	class DMaterialExpressionSplat3 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSplat3(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Splat3, .Name = "Splat",
				.Description = "Replicates a scalar across vector components.",
				.Category = "Channels", .bShowInPalette = false};
		}

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Splat4.
	DCLASS()
	class DMaterialExpressionSplat4 : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSplat4(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Splat4, .Name = "Splat",
				.Description = "Replicates a scalar across vector components.",
				.Category = "Channels", .bShowInPalette = false};
		}

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by BlendNormalsRNM.
	DCLASS()
	class DMaterialExpressionBlendNormalsRNM : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionBlendNormalsRNM(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::BlendNormalsRNM, .Name = "Blend Normals RNM",
				.Description = "Blends two tangent-space normals with RNM.",
				.Category = "Textures", .InputNames = {"Base", "Detail"}};
		}

		DPROPERTY()
		FMaterialNumericInput Base{3};

		DPROPERTY()
		FMaterialNumericInput Detail{3};

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Concatenates numeric inputs; the output width follows their combined widths.
	DCLASS()
	class DMaterialExpressionAppendVector : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionAppendVector(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::AppendVector, .Name = "Append Vector",
				.Description = "Concatenates A and B; output width follows the inputs (up to four components).",
				.Category = "Channels", .InputNames = {"A", "B"}, .PaletteDefault = EMaterialProgramValueType::Float2,
				.bHasInstanceShape = true};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		FMaterialNumericInput A;
		DPROPERTY()
		FMaterialNumericInput B;
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float2;
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Component selection supplies the swizzle's output width.
	DCLASS()
	class DMaterialExpressionSwizzle : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSwizzle(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Swizzle, .Name = "Component Mask",
				.Description = "Selects, repeats or reorders channels (Component Mask / Truncate).",
				.Category = "Channels", .PaletteDefault = EMaterialProgramValueType::Float, .bHasInstanceShape = true};
		}

		DPROPERTY()
		FMaterialNumericInput Input;

		DPROPERTY()
		std::vector<uint8> Components{0};

		// An identity-width swizzle may explicitly classify an authored numeric
		// value. Non-identity swizzles cannot manufacture spatial meaning.
		DPROPERTY()
		EMaterialSpatialKind OutputSpatialKind = EMaterialSpatialKind::None;

		DPROPERTY()
		EMaterialCoordinateSpace OutputCoordinateSpace = EMaterialCoordinateSpace::None;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	protected:
		ENGINE_API auto GetNumericResultType(EMaterialProgramOpcode Opcode) const -> std::optional<EMaterialProgramValueType> override;
	};
}
