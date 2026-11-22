#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"

#include "MaterialExpressionConstant.gen.h"

namespace Durin
{
	// Owns a concrete scalar constant value.
	DCLASS()
	class DMaterialExpressionScalarConstant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionScalarConstant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY(Edit)
		float Value = 0.0f;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns a concrete vector2 constant value.
	DCLASS()
	class DMaterialExpressionVector2Constant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector2Constant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY(Edit)
		FVector2 Value{0.0};

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns a concrete vector3 constant value.
	DCLASS()
	class DMaterialExpressionVector3Constant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector3Constant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY(Edit)
		FVector3 Value{0.0};

		DPROPERTY()
		bool bTangentSpaceNormal = false;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns a concrete vector4 constant value.
	DCLASS()
	class DMaterialExpressionVector4Constant : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVector4Constant(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY(Edit)
		FVector4 Value{0.0};

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
