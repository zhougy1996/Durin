#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionNumeric.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionArithmetic.gen.h"

namespace Durin
{
	// Owns only the inputs and width required by Add.
	DCLASS()
	class DMaterialExpressionAdd : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionAdd(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Add, .Name = "Add",
				.Description = "Adds two values component by component.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Subtract.
	DCLASS()
	class DMaterialExpressionSubtract : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSubtract(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Subtract, .Name = "Subtract",
				.Description = "Subtracts the second value from the first.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Multiply.
	DCLASS()
	class DMaterialExpressionMultiply : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMultiply(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Multiply, .Name = "Multiply",
				.Description = "Multiplies two values component by component.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Divide.
	DCLASS()
	class DMaterialExpressionDivide : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionDivide(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Divide, .Name = "Divide",
				.Description = "Divides the first value by the second.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Minimum.
	DCLASS()
	class DMaterialExpressionMinimum : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMinimum(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Minimum, .Name = "Minimum",
				.Description = "Returns the component-wise minimum.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Maximum.
	DCLASS()
	class DMaterialExpressionMaximum : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMaximum(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Maximum, .Name = "Maximum",
				.Description = "Returns the component-wise maximum.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		auto GetAuthoredInputCount() const -> uint32 override { return 2; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Negate.
	DCLASS()
	class DMaterialExpressionNegate : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionNegate(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Negate, .Name = "Negate",
				.Description = "Reverses the sign of a value.",
				.Category = "Math"};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by OneMinus.
	DCLASS()
	class DMaterialExpressionOneMinus : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionOneMinus(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::OneMinus, .Name = "One Minus",
				.Description = "Subtracts a value from one.",
				.Category = "Math"};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Absolute.
	DCLASS()
	class DMaterialExpressionAbsolute : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionAbsolute(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Absolute, .Name = "Absolute",
				.Description = "Returns the absolute value.",
				.Category = "Math"};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Saturate.
	DCLASS()
	class DMaterialExpressionSaturate : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSaturate(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Saturate, .Name = "Saturate",
				.Description = "Clamps a value to the zero-to-one range.",
				.Category = "Math"};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Sine.
	DCLASS()
	class DMaterialExpressionSine : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSine(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Sine, .Name = "Sine",
				.Description = "Returns the component-wise sine in radians.",
				.Category = "Math"};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Cosine.
	DCLASS()
	class DMaterialExpressionCosine : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCosine(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Cosine, .Name = "Cosine",
				.Description = "Returns the component-wise cosine in radians.",
				.Category = "Math"};
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
	class DMaterialExpressionPow : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionPow(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Pow, .Name = "Power",
				.Description = "Raises each base component to its exponent.",
				.Category = "Math", .InputNames = {"Base", "Exponent"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput Base;
		DPROPERTY()
		FMaterialNumericInput Exponent;
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionSqrt : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSqrt(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Sqrt, .Name = "Square Root",
				.Description = "Returns the component-wise square root.",
				.Category = "Math"};
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
	class DMaterialExpressionExp : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionExp(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Exp, .Name = "Exponential",
				.Description = "Returns the component-wise natural exponential.",
				.Category = "Math"};
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
	class DMaterialExpressionLog : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionLog(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Log, .Name = "Natural Log",
				.Description = "Returns the component-wise natural logarithm.",
				.Category = "Math"};
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
	class DMaterialExpressionFloor : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFloor(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Floor, .Name = "Floor",
				.Description = "Rounds each component down.",
				.Category = "Math"};
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
	class DMaterialExpressionCeil : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCeil(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Ceil, .Name = "Ceil",
				.Description = "Rounds each component up.",
				.Category = "Math"};
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
	class DMaterialExpressionRound : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionRound(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Round, .Name = "Round",
				.Description = "Rounds each component to the nearest integer.",
				.Category = "Math"};
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
	class DMaterialExpressionFrac : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFrac(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Frac, .Name = "Fraction",
				.Description = "Returns each component's fractional part.",
				.Category = "Math"};
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
	class DMaterialExpressionFmod : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFmod(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Fmod, .Name = "Fmod",
				.Description = "Returns the component-wise floating-point remainder.",
				.Category = "Math", .InputNames = {"A", "B"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput A;
		DPROPERTY()
		FMaterialNumericInput B;
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionStep : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionStep(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Step, .Name = "Step",
				.Description = "Returns zero below Edge and one otherwise.",
				.Category = "Math", .InputNames = {"Edge", "Value"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput Edge;
		DPROPERTY()
		FMaterialNumericInput Value;
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionSmoothStep : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSmoothStep(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::SmoothStep, .Name = "Smooth Step",
				.Description = "Returns smooth Hermite interpolation between Min and Max.",
				.Category = "Math", .InputNames = {"Min", "Max", "Value"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput Minimum;
		DPROPERTY()
		FMaterialNumericInput Maximum;
		DPROPERTY()
		FMaterialNumericInput Value;
		auto GetAuthoredInputCount() const -> uint32 override { return 3; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionSign : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSign(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Sign, .Name = "Sign",
				.Description = "Returns the sign of each component.",
				.Category = "Math"};
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
	class DMaterialExpressionReflect : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionReflect(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Reflect, .Name = "Reflect",
				.Description = "Reflects an incident vector around a normal of equal width.",
				.Category = "Math", .InputNames = {"Incident", "Normal"}, .PaletteDefault = EMaterialProgramValueType::Float2};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float2;
		DPROPERTY()
		FMaterialNumericInput Incident;
		DPROPERTY()
		FMaterialNumericInput Normal;
		auto GetAuthoredInputCount() const -> uint32 override { return 2; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	// Owns only the inputs and width required by Clamp.
	DCLASS()
	class DMaterialExpressionClamp : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionClamp(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Clamp, .Name = "Clamp",
				.Description = "Constrains a value between minimum and maximum inputs.",
				.Category = "Math", .InputNames = {"Value", "Min", "Max"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput Input;

		DPROPERTY()
		FMaterialNumericInput Minimum;

		DPROPERTY()
		FMaterialNumericInput Maximum;

		auto GetAuthoredInputCount() const -> uint32 override { return 3; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns only the inputs and width required by Lerp.
	DCLASS()
	class DMaterialExpressionLerp : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionLerp(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Lerp, .Name = "Lerp",
				.Description = "Interpolates between two values.",
				.Category = "Math", .InputNames = {"A", "B", "Alpha"}};
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }

		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialNumericInput A;

		DPROPERTY()
		FMaterialNumericInput B;

		DPROPERTY()
		FMaterialNumericInput Alpha;

		auto GetAuthoredInputCount() const -> uint32 override { return 3; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
