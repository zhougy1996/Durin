#pragma once

#include "Asset/BulkData.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramTypes.h"
#include "Texture/Texture2D.h"

#include "Material.gen.h"

namespace Durin
{
	// Owns canonical base-material definitions and their editable default values.
	DCLASS()
	class DMaterial : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterial(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		auto GetMaterialProgram() const -> const FMaterialProgram* override
		{
			return &Program;
		}
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
		auto GetCookedProgramData() const -> const FBulkData&
		{
			return CookedProgramData;
		}
		auto GetMaterialCompileDiagnostics() const
			-> std::span<const FMaterialCompileDiagnostic>
		{
			return MaterialCompileDiagnostics;
		}
		auto GetMaterialCompileStatus() const -> const FMaterialCompileStatus&
		{
			return MaterialCompileStatus;
		}
		auto GetMaterialCookDiagnostic() const -> std::string_view
		{
			return MaterialCookDiagnostic;
		}
		ENGINE_API auto SetMaterialProgram(
			FMaterialProgram InProgram,
			FMaterialProgramValidationResult& OutValidation) -> bool;
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
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	public:
		ENGINE_API auto PostEditChangeProperty(
			const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto BeginDestroy() -> void override;

	protected:
		ENGINE_API auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer override;

	private:
		ENGINE_API auto RequestProgramCompile(
			const FMaterialProgram& CandidateProgram,
			const FMaterialStaticProperties& CandidateProperties,
			bool bForceRecompile = false) -> bool;
		auto AdvanceAuthoredRevision() -> void;
		// These values are inherited by instances and will form shader and pipeline keys.
		DPROPERTY(Edit)
		FMaterialStaticProperties StaticProperties;

		// Definition identity and metadata are canonical; only the nested Value fields are editable.
		DPROPERTY()
		std::vector<FMaterialParameterDefinition> ParameterDefinitions;

		// Missing legacy fields retain the canonical constructor value; malformed
		// present program data is rejected by PostLoad.
		DPROPERTY(EditorOnly)
		FMaterialProgram Program;

		// Shared node positions are persisted for authoring but excluded from Cook and compilation.
		DPROPERTY(EditorOnly)
		FMaterialGraphPresentation GraphPresentation;

		FBulkData CookedProgramData;

		std::shared_ptr<const FMaterialCompilerResult> AcceptedCompiledProgram;
		FMaterialStaticProperties AcceptedCompiledStaticProperties;
		FMaterialCompileStatus MaterialCompileStatus;
		std::vector<FMaterialCompileDiagnostic> MaterialCompileDiagnostics;
		std::string MaterialCookDiagnostic;
		// Parameter values change frequently, while reachability changes only with the authored program.
		mutable uint64 CachedParameterDependencyRevision = 0;
		mutable std::vector<FMaterialParameterDependency> CachedParameterDependencies;
		// Transient monotonic revisions invalidate editor graph caches independently.
		uint64 MaterialProgramRevision = 1;
		uint64 MaterialGraphPresentationRevision = 1;
		uint64 ParameterDefinitionSchemaRevision = 1;

		auto LoadCookedProgram(std::string& OutError) -> bool;

		friend struct Private::FMaterialCompilationLifecycle;
	};
}
