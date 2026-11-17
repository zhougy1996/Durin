#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionTransform.gen.h"

namespace Durin
{
	DCLASS()
	class DMaterialExpressionTransformPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTransformPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		DPROPERTY()
		FMaterialNumericInput Input{3};
		DPROPERTY()
		EMaterialCoordinateSpace Source = EMaterialCoordinateSpace::Object;
		DPROPERTY()
		EMaterialCoordinateSpace Destination = EMaterialCoordinateSpace::World;
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionTransformDirection : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTransformDirection(const FObjectInitializer& Initializer) : Super(Initializer) {}
		DPROPERTY()
		FMaterialNumericInput Input{3};
		DPROPERTY()
		EMaterialCoordinateSpace Source = EMaterialCoordinateSpace::Object;
		DPROPERTY()
		EMaterialCoordinateSpace Destination = EMaterialCoordinateSpace::World;
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionTransformNormal : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTransformNormal(const FObjectInitializer& Initializer) : Super(Initializer) {}
		DPROPERTY()
		FMaterialNumericInput Input{3};
		DPROPERTY()
		EMaterialCoordinateSpace Source = EMaterialCoordinateSpace::Object;
		DPROPERTY()
		EMaterialCoordinateSpace Destination = EMaterialCoordinateSpace::World;
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
