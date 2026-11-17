#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialTypes.h"

#include "MaterialExpressionParameter.gen.h"

namespace Durin
{
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
		ENGINE_API auto SetParameterDefinition(const FMaterialParameterDefinition& Definition) -> std::expected<void, std::string>;

	protected:
		ENGINE_API auto MakeDefinition(EMaterialParameterType Type, FMaterialParameterValue Value) const
			-> FMaterialParameterDefinition;
	};
}
