#pragma once

#include "Materials/MaterialFunctionInterface.h"

#include "MaterialFunction.gen.h"

namespace Durin
{
	// Owns the editable function graph and presentation; rendering consumes detached expansion.
	DCLASS()
	class DMaterialFunction : public DMaterialFunctionInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialFunction(const FObjectInitializer& Initializer);
		auto GetFunctionSignature() const -> const FMaterialFunctionSignature& override
			{ return Graph.Signature; }
		ENGINE_API auto GetFunctionDependencies() const
			-> std::vector<TObjectPtr<DMaterialFunctionInterface>> override;
		auto GetFunctionRevision() const -> uint64 override { return Revision; }
		ENGINE_API auto BuildFunctionSnapshot(FMaterialFunctionSnapshot& OutSnapshot) const
			-> FMaterialProgramValidationResult override;
		auto GetFunctionGraph() const -> const FMaterialFunctionGraph& { return Graph; }
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
	private:

		DPROPERTY(EditorOnly)
		FMaterialFunctionGraph Graph;

		DPROPERTY(EditorOnly)
		FMaterialFunctionPresentation Presentation;

		DPROPERTY(EditorOnly)
		std::string AuthoringSource;

		DPROPERTY(EditorOnly)
		uint32 AuthoringSourceVersion = 0;
		uint64 Revision = 1;
	};
}
