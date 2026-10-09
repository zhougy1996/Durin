#pragma once

#include "CoreMinimal.h"

#include "Asset/BulkData.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialExpressions.h"

#include "Materials/MaterialGraphChanges.h"

#include "Material.gen.h"

namespace Durin
{
	class DTexture2D;
	class IObjectReplacementParticipant;
	// Rebinds native parameter-schema texture references alongside reflected graph owners.
	ENGINE_API auto MakeMaterialReferenceReplacementParticipant() -> std::shared_ptr<IObjectReplacementParticipant>;
	// Accepted writes distinguish unchanged sanitized state from an actual edit.
	enum class EMaterialGraphPresentationResult : uint8
	{
		Rejected, NoChange, Changed,
	};

	// Owns the base-material graph and its parameter definitions.
	DCLASS()
	class DMaterial : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		ENGINE_API explicit DMaterial(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetExpressionCollection() const -> const FMaterialExpressionCollection& { return ExpressionCollection; }
		ENGINE_API auto GetExpressionOutputs() const -> const FMaterialExpressionSurfaceOutputs&;
		ENGINE_API auto GetOutputNode() const -> const DMaterialExpressionMaterialOutput*;
#endif
		auto GetDomain() const -> EMaterialDomain { return Domain; }
#if DURIN_WITH_EDITORONLY_DATA
		[[nodiscard]] ENGINE_API auto SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		[[nodiscard]] ENGINE_API auto SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions,
			FMaterialExpressionSurfaceOutputs Outputs) -> FMaterialProgramValidationResult;
#endif
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetMaterialGraphPresentation() const
			-> const FMaterialGraphPresentation&
		{
			return GraphPresentation;
		}
#endif
		auto GetMaterialProgramRevision() const -> uint64
		{
			return MaterialProgramRevision;
		}
		// Transient editor preference, inherited by loaded instances. Switching policy
		// reschedules unsubmitted edits without modifying authored or saved state.
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto SetEditCompileMode(EMaterialEditCompileMode Mode) -> void;
		auto GetEditCompileMode() const -> EMaterialEditCompileMode { return EditCompileMode; }
		// Explicitly submits the current root and all loaded dependent variants, using caches.
		ENGINE_API auto CompileEdits() -> bool;
		ENGINE_API auto SetMaterialGraphPresentation(
			FMaterialGraphPresentation InPresentation) -> EMaterialGraphPresentationResult;
		// Applies bounded graph-position edits without copying or sanitizing the
		// complete authored program/presentation. ExpectedAuthoredRevision keeps
		// an interactive edit from crossing a semantic material change.
		ENGINE_API auto ApplyMaterialGraphNodePositions(
			std::span<const FMaterialGraphNodePresentation> Positions,
			uint64 ExpectedAuthoredRevision) -> EMaterialGraphPresentationResult;
#endif
		ENGINE_API auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		auto GetStaticProperties() const -> const FMaterialStaticProperties& override { return StaticProperties; }
		ENGINE_API auto SetStaticProperties(const FMaterialStaticProperties& InProperties) -> FMaterialOperationResult;

		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> FMaterialOperationResult;
		ENGINE_API auto SetVector2ParameterValue(FName Name, const FVector2& Value) -> FMaterialOperationResult;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> FMaterialOperationResult;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> FMaterialOperationResult;
		ENGINE_API auto SetParameterValue(
			const FGuid& Id, const FMaterialParameterValue& Value) -> FMaterialOperationResult;
		ENGINE_API auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		ENGINE_API auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool override;
		ENGINE_API auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		ENGINE_API auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
		ENGINE_API auto PostLoad() -> void override;
	public:
		ENGINE_API auto PostEditChangeProperty(
			const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto BeginDestroy() -> void override;

#if DURIN_WITH_EDITORONLY_DATA
		auto GetGraphChanges() -> FMaterialGraphChangeSource& { return GraphChanges; }
#endif

	private:
		friend struct FMaterialExpressionEditing;
		friend class FMaterialReferenceReplacementParticipant;
#if DURIN_WITH_EDITORONLY_DATA
		FMaterialGraphChangeSource GraphChanges;
#endif
		auto AdvanceAuthoredRevision(FObjectCacheContext* Context = nullptr) -> void;
#if DURIN_WITH_EDITORONLY_DATA
		EMaterialEditCompileMode EditCompileMode = EMaterialEditCompileMode::Immediate;
#endif
		// These values are inherited by instances and will form shader and pipeline keys.
		DPROPERTY(Edit)
		FMaterialStaticProperties StaticProperties;

		// Read-only projection of graph owners, or generated metadata loaded from Cook.
		// Resources are retained by explicit reference collection.
		std::vector<FMaterialParameterDefinition> ParameterSchema;

#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly, AlwaysSerialize)
		FMaterialExpressionCollection ExpressionCollection;
#endif

		DPROPERTY()
		EMaterialDomain Domain = EMaterialDomain::Surface;


		static auto ValidateExpressionGraph(const FMaterialExpressionCollection& Collection,
			const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		static auto DeriveExpressionParameterSchema(const FMaterialExpressionCollection& Collection,
			std::vector<FMaterialParameterDefinition>& OutDefinitions) -> FMaterialProgramValidationResult;

		// Shared node positions are persisted for authoring but excluded from Cook and compilation.
#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		FMaterialGraphPresentation GraphPresentation;
#endif

		// Detached code checkpoint classifies reflected default/metadata edits without retaining resources.
		FXxHash128 ObservedExpressionCode;

		// Transient revision validates program-dependent caches and snapshots.
		uint64 MaterialProgramRevision = 1;


		friend struct Private::FMaterialCompilationLifecycle;
	};
}
