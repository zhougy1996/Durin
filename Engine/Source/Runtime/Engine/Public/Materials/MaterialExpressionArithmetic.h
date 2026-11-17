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
