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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::TransformPosition, .Name = "Transform Position",
				.Description = "Transforms a spatial position between explicit coordinate spaces; World is relative to the current view origin.",
				.Category = "Transforms", .FixedOutputType = EMaterialProgramValueType::Float3};
		}

		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = "Input", .Type = EMaterialProgramValueType::Float3};
			return {};
		}
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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::TransformDirection, .Name = "Transform Direction",
				.Description = "Transforms a spatial direction between explicit coordinate spaces.",
				.Category = "Transforms", .FixedOutputType = EMaterialProgramValueType::Float3};
		}

		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = "Input", .Type = EMaterialProgramValueType::Float3};
			return {};
		}
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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::TransformNormal, .Name = "Transform Normal",
				.Description = "Transforms and normalizes a spatial normal with inverse-transpose semantics.",
				.Category = "Transforms", .FixedOutputType = EMaterialProgramValueType::Float3};
		}

		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = "Input", .Type = EMaterialProgramValueType::Float3};
			return {};
		}
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
