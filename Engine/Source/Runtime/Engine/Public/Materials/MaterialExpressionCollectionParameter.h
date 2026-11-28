#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionCollectionParameter.gen.h"

namespace Durin
{
	// Reads one numeric declaration from the current world's collection snapshot.
	DCLASS()
	class DMaterialExpressionCollectionParameter : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCollectionParameter(
			const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::CollectionParameter, .Name = "Collection Parameter",
				.Description = "Reads a numeric value from a material parameter collection in the current world.",
				.Category = "Parameters", .bRequiresAsset = true, .bHasInstanceShape = true};
		}
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
#endif

		DPROPERTY(Edit)
		TObjectPtr<DMaterialParameterCollection> Collection;

		DPROPERTY(Edit)
		FGuid ParameterId;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
