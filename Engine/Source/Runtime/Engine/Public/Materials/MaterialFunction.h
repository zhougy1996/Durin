#pragma once

#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialExpressions.h"

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
		auto GetExpressionCollection() const -> const FMaterialExpressionCollection& { return ExpressionCollection; }
		ENGINE_API auto GetExpressionBody() const -> MIR::FFunctionBody;
		[[nodiscard]] ENGINE_API auto SetFunctionExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		// Bootstrap provenance is editor metadata, never part of compiler semantics.
		auto GetAuthoringSource() const -> const std::string& { return AuthoringSource; }
		auto GetAuthoringSourceVersion() const -> uint32 { return AuthoringSourceVersion; }
		ENGINE_API auto SetAuthoringSource(std::string Source, uint32 Version) -> void;
		auto GetFunctionPresentation() const -> const FMaterialFunctionPresentation&
			{ return Presentation; }
		// Position-only edits never advance the semantic dependency revision.
		ENGINE_API auto SetFunctionPresentation(FMaterialFunctionPresentation Candidate) -> bool;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> FObjectValidationResult override;
		auto GetGraphChanges() -> FMaterialGraphChangeSource& { return GraphChanges; }

	private:
		friend struct FMaterialExpressionEditing;
		FMaterialGraphChangeSource GraphChanges;

		// Read-only projection; never serialized or independently edited.
		mutable FMaterialFunctionSignature CachedSignature;
		mutable bool bSignatureCached = false;

		DPROPERTY(EditorOnly, AlwaysSerialize)
		FMaterialExpressionCollection ExpressionCollection;

		DPROPERTY(EditorOnly)
		FMaterialFunctionPresentation Presentation;

		DPROPERTY(EditorOnly)
		std::string AuthoringSource;

		DPROPERTY(EditorOnly)
		uint32 AuthoringSourceVersion = 0;
		uint64 Revision = 1;
	};
}
