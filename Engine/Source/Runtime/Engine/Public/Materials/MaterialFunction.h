#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"

#include "Materials/MaterialFunctionInterface.h"

#include "Materials/MaterialGraphChanges.h"

#include "MaterialFunction.gen.h"

namespace Durin
{
	namespace MIR
	{
		struct FFunctionBody;
	}
	// Owns the editable function graph and presentation; rendering consumes detached expansion.
	DCLASS()
	class DMaterialFunction : public DMaterialFunctionInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialFunction(const FObjectInitializer& Initializer);
		ENGINE_API auto GetFunctionSignature() const -> const FMaterialFunctionSignature& override;
		ENGINE_API auto GetFunctionDependencies() const
			-> std::vector<TObjectPtr<DMaterialFunctionInterface>> override;
		auto GetFunctionRevision() const -> uint64 override { return Revision; }
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto PostEditUndo() -> void override;
		ENGINE_API auto PostTransacted(const FTransactionObjectEvent& Event) -> void override;
		auto GetExpressionCollection() const -> const FMaterialExpressionCollection& { return ExpressionCollection; }
		ENGINE_API auto GetExpressionBody() const -> MIR::FFunctionBody;
		[[nodiscard]] ENGINE_API auto SetFunctionExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		ENGINE_API auto GetFunctionPresentation() const -> FMaterialFunctionPresentation;
		// Position-only edits never advance the semantic dependency revision.
		ENGINE_API auto SetFunctionPresentation(FMaterialFunctionPresentation Candidate) -> bool;
#endif
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetGraphChanges() -> FMaterialGraphChangeSource& { return GraphChanges; }
#endif

	private:
		friend struct FMaterialExpressionEditing;
#if DURIN_WITH_EDITORONLY_DATA
		auto InvalidateSignature() -> bool;
		FMaterialGraphChangeSource GraphChanges;
		uint64 LastGraphReplayOperation = 0;
#endif

		// Read-only projection; never serialized or independently edited.
		mutable FMaterialFunctionSignature CachedSignature;
		mutable bool bSignatureCached = false;

#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly, AlwaysSerialize, MetaData="ValidateTransactionSource=true")
		FMaterialExpressionCollection ExpressionCollection;

		// Load-only field for packages authored before expression-owned layout.
		DPROPERTY(Deprecated)
		FMaterialFunctionPresentation Presentation_DEPRECATED;
#endif

		uint64 Revision = 1;
	};
}
