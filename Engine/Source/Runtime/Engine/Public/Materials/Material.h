#pragma once

#include "Asset/BulkData.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialExpressions.h"
#include "Texture/Texture2D.h"

#include "Material.gen.h"

namespace Durin
{
	// Owns the base-material graph and its parameter definitions.
	DCLASS()
	class DMaterial : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		ENGINE_API explicit DMaterial(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		auto GetExpressionCollection() const -> const FMaterialExpressionCollection& { return ExpressionCollection; }
		auto GetExpressionOutputs() const -> const FMaterialExpressionSurfaceOutputs& { return ExpressionOutputs; }
		[[nodiscard]] ENGINE_API auto SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions,
			FMaterialExpressionSurfaceOutputs Outputs) -> FMaterialProgramValidationResult;
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context, std::string& OutError) const -> bool override;
		auto GetMaterialGraphPresentation() const
			-> const FMaterialGraphPresentation&
		{
			return GraphPresentation;
		}
		auto GetMaterialProgramRevision() const -> uint64
		{
			return MaterialProgramRevision;
		}
		auto GetMaterialGraphPresentationRevision() const -> uint64
		{
			return MaterialGraphPresentationRevision;
		}
		auto GetParameterDefinitionSchemaRevision() const -> uint64
		{
			return ParameterDefinitionSchemaRevision;
		}
		ENGINE_API auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult> override;
		// Transient editor preference, inherited by loaded instances. Switching policy
		// reschedules unsubmitted edits without modifying authored or saved state.
		ENGINE_API auto SetEditCompileMode(EMaterialEditCompileMode Mode) -> void;
		auto GetEditCompileMode() const -> EMaterialEditCompileMode { return EditCompileMode; }
		// Explicitly submits the current root and all loaded dependent variants, using caches.
		ENGINE_API auto CompileEdits() -> bool;
		ENGINE_API auto SetMaterialGraphPresentation(
			FMaterialGraphPresentation InPresentation) -> bool;
		// Applies bounded graph-position edits without copying or sanitizing the
		// complete authored program/presentation. ExpectedAuthoredRevision keeps
		// an interactive edit from crossing a semantic material change.
		ENGINE_API auto ApplyMaterialGraphNodePositions(
			std::span<const FMaterialGraphNodePresentation> Positions,
			uint64 ExpectedAuthoredRevision) -> bool;
		ENGINE_API auto ApplyMaterialGraphOutputPosition(
			int32 X, int32 Y, uint64 ExpectedAuthoredRevision) -> bool;
		ENGINE_API auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		auto GetStaticProperties() const -> const FMaterialStaticProperties& override { return StaticProperties; }
		ENGINE_API auto GetRenderableStaticProperties() const
			-> FMaterialStaticProperties override;
		ENGINE_API auto SetStaticProperties(const FMaterialStaticProperties& InProperties) -> bool;

		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> bool;
		ENGINE_API auto SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> bool;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool;
		ENGINE_API auto SetParameterValue(
			const FGuid& Id, const FMaterialParameterValue& Value) -> bool;
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

	protected:
		ENGINE_API auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer override;

	private:
		DPROPERTY(AlwaysSerialize)
		uint32 GraphOwnershipVersion = 2;

		auto AdvanceAuthoredRevision() -> void;
		EMaterialEditCompileMode EditCompileMode = EMaterialEditCompileMode::Immediate;
		// These values are inherited by instances and will form shader and pipeline keys.
		DPROPERTY(Edit)
		FMaterialStaticProperties StaticProperties;

		// Read-only projection of graph owners, or generated metadata loaded from Cook.
		// Resources are retained by explicit reference collection.
		std::vector<FMaterialParameterDefinition> ParameterSchema;

		DPROPERTY(EditorOnly, AlwaysSerialize)
		FMaterialExpressionCollection ExpressionCollection;

		DPROPERTY(EditorOnly)
		FMaterialExpressionSurfaceOutputs ExpressionOutputs;

		static auto ValidateExpressionGraph(const FMaterialExpressionCollection& Collection,
			const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		static auto DeriveExpressionParameterSchema(const FMaterialExpressionCollection& Collection,
			std::vector<FMaterialParameterDefinition>& OutDefinitions) -> FMaterialProgramValidationResult;

		// Shared node positions are persisted for authoring but excluded from Cook and compilation.
		DPROPERTY(EditorOnly)
		FMaterialGraphPresentation GraphPresentation;


		// Detached code checkpoint classifies reflected default/metadata edits without retaining resources.
		FXxHash128 ObservedExpressionCode;

		// Transient monotonic revisions invalidate editor graph caches independently.
		uint64 MaterialProgramRevision = 1;
		uint64 MaterialGraphPresentationRevision = 1;
		uint64 ParameterDefinitionSchemaRevision = 1;


		friend struct Private::FMaterialCompilationLifecycle;
	};
}
