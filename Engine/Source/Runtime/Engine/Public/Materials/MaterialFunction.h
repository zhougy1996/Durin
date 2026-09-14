#pragma once

#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialExpressions.h"

#include "MaterialFunction.gen.h"

namespace Durin
{
	struct FMaterialExpressionFunctionBody;
	// Owns the editable function graph and presentation; rendering consumes detached expansion.
	DCLASS()
	class DMaterialFunction : public DMaterialFunctionInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialFunction(const FObjectInitializer& Initializer);
		auto GetFunctionSignature() const -> const FMaterialFunctionSignature& override
			{ return Signature; }
		ENGINE_API auto GetFunctionDependencies() const
			-> std::vector<TObjectPtr<DMaterialFunctionInterface>> override;
		auto GetFunctionRevision() const -> uint64 override { return Revision; }
		ENGINE_API auto BuildFunctionSnapshot(FMaterialFunctionSnapshot& OutSnapshot) const
			-> FMaterialProgramValidationResult override;
		ENGINE_API auto GetFunctionGraph() const -> const FMaterialFunctionGraph&;
		auto GetExpressionCollection() const -> const FMaterialExpressionCollection& { return ExpressionCollection; }
		ENGINE_API auto GetExpressionBody() const -> FMaterialExpressionFunctionBody;
		[[nodiscard]] ENGINE_API auto SetFunctionExpressions(FMaterialFunctionSignature InSignature,
			std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		// Bootstrap provenance is editor metadata, never part of compiler semantics.
		auto GetAuthoringSource() const -> const std::string& { return AuthoringSource; }
		auto GetAuthoringSourceVersion() const -> uint32 { return AuthoringSourceVersion; }
		ENGINE_API auto SetAuthoringSource(std::string Source, uint32 Version) -> void;
		// Owning-thread atomic edit. Invalid candidates leave the authored graph unchanged.
		[[nodiscard]] ENGINE_API auto SetFunctionGraph(FMaterialFunctionGraph Candidate)
			-> FMaterialProgramValidationResult;
		auto GetFunctionPresentation() const -> const FMaterialFunctionPresentation&
			{ return Presentation; }
		// Position-only edits never advance the semantic dependency revision.
		ENGINE_API auto SetFunctionPresentation(FMaterialFunctionPresentation Candidate) -> bool;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context, std::string& OutError) const -> bool override;
	private:
		DPROPERTY(AlwaysSerialize)
		uint32 GraphOwnershipVersion = 2;


		DPROPERTY(EditorOnly, AlwaysSerialize)
		FMaterialFunctionSignature Signature;

		DPROPERTY(EditorOnly, AlwaysSerialize)
		FMaterialExpressionCollection ExpressionCollection;

		// Temporary read-only projection for native/editor callers awaiting migration.
		mutable FMaterialFunctionGraph Graph;
		auto ProjectExpressions(const FMaterialFunctionSignature& InSignature,
			const FMaterialExpressionCollection& Collection, FMaterialFunctionGraph& OutGraph) const -> bool;

		DPROPERTY(EditorOnly)
		FMaterialFunctionPresentation Presentation;

		DPROPERTY(EditorOnly)
		std::string AuthoringSource;

		DPROPERTY(EditorOnly)
		uint32 AuthoringSourceVersion = 0;
		uint64 Revision = 1;
	};
}
