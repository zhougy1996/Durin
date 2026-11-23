#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "Asset/BulkData.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialProgramTypes.h"

#include "Materials/MaterialGraphChanges.h"

#include "Material.gen.h"

namespace Durin
{
	class DTexture;
	class IObjectReplacementParticipant;
	struct FMaterialConfiguration
	{
		EMaterialDomain Domain = EMaterialDomain::Surface;
		FMaterialStaticProperties Properties;
	};
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
		ENGINE_API auto PostEditUndo() -> void override;
		ENGINE_API auto PostTransacted(const FTransactionObjectEvent& Event) -> void override;
		auto GetExpressionCollection() const -> const FMaterialExpressionCollection& { return ExpressionCollection; }
		// Capture independent input values for whole-graph replacement and ownership rebinding.
		ENGINE_API auto CaptureRootInputs() const -> FMaterialRootInputs;
		auto UsesMaterialAttributes() const -> bool { return bUseMaterialAttributes; }
		auto CanEditMaterialAttributesMode() const -> bool { return Domain == EMaterialDomain::Surface; }
		ENGINE_API auto GetNumericInputForProperty(EMaterialProperty Property) const -> std::optional<FMaterialNumericInputView>;
		ENGINE_API auto GetExpressionInputForProperty(EMaterialProperty Property) const -> const FMaterialExpressionInput*;
		ENGINE_API auto IsRootInputVisible(EMaterialProperty Property) const -> bool;
		ENGINE_API auto GetExpressionInputDescription(EMaterialProperty Property) const -> std::optional<FMaterialInputDescription>;
		auto GetRootPosition() const -> std::pair<int32, int32> { return {EditorX, EditorY}; }
		ENGINE_API void SetRootPosition(int32 X, int32 Y);
#endif
		auto GetDomain() const -> EMaterialDomain { return Domain; }
		ENGINE_API auto SetMaterialConfiguration(const FMaterialConfiguration& Configuration) -> FMaterialProgramValidationResult;
#if DURIN_WITH_EDITORONLY_DATA
		[[nodiscard]] ENGINE_API auto SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		[[nodiscard]] ENGINE_API auto SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions,
			FMaterialRootInputs Outputs) -> FMaterialProgramValidationResult;
#endif
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto GetMaterialGraphPresentation() const -> FMaterialGraphPresentation;
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
		ENGINE_API auto SetStaticProperties(const FMaterialStaticProperties& InProperties) -> std::expected<void, std::string>;

		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> std::expected<void, std::string>;
		ENGINE_API auto SetVector2ParameterValue(FName Name, const FVector2& Value) -> std::expected<void, std::string>;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> std::expected<void, std::string>;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture* Value) -> std::expected<void, std::string>;
		ENGINE_API auto SetParameterValue(
			const FGuid& Id, const FMaterialParameterValue& Value) -> std::expected<void, std::string>;
		ENGINE_API auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		ENGINE_API auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool override;
		ENGINE_API auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		ENGINE_API auto GetTextureParameterValue(FName Name, DTexture*& OutValue) const -> bool override;
		ENGINE_API static auto AppendToClassSchema(FObjectCookContext& Context) -> void;
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
		uint64 LastGraphReplayOperation = 0;
		// The last published configuration survives reflected transaction loads.
		FMaterialConfiguration PublishedConfiguration;
#endif
		auto PublishMaterialEdit(const FMaterialConfiguration& Previous, bool bGraphChanged = false, bool bGraphCodeChanged = false) -> void;
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
		DPROPERTY(EditorOnly, AlwaysSerialize, MetaData="ValidateTransactionSource=true")
		FMaterialExpressionCollection ExpressionCollection;

		DPROPERTY(EditorOnly)
		FColorMaterialInput BaseColor;
		DPROPERTY(EditorOnly)
		FVectorMaterialInput Normal;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput Metallic;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput Roughness;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput AmbientOcclusion;
		DPROPERTY(EditorOnly)
		FColorMaterialInput EmissiveColor;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput Opacity;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput OpacityMask;
		DPROPERTY(EditorOnly)
		FVectorMaterialInput WorldPositionOffset;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput Specular;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput ClearCoat;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput ClearCoatRoughness;
		DPROPERTY(EditorOnly)
		FScalarMaterialInput Refraction;
		static void VisitNumericField(auto& Owner, EMaterialProperty Property, auto&& Visit);
		auto ApplyRootInputs(FMaterialRootInputs Inputs) -> void;
		DPROPERTY(EditorOnly)
		FMaterialExpressionInput MaterialAttributes;
		DPROPERTY(EditorOnly)
		bool bUseMaterialAttributes = false;
		DPROPERTY(EditorOnly)
		int32 EditorX = 1280;
		DPROPERTY(EditorOnly)
		int32 EditorY = 0;
#endif

		DPROPERTY()
		EMaterialDomain Domain = EMaterialDomain::Surface;


		auto ValidateExpressionGraph(FXxHash128* OutCodeFingerprint = nullptr) const -> FMaterialProgramValidationResult;
		static auto ValidateExpressionGraph(const FMaterialExpressionCollection& Collection,
			const FMaterialRootInputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		static auto DeriveExpressionParameterSchema(const FMaterialExpressionCollection& Collection,
			std::vector<FMaterialParameterDefinition>& OutDefinitions) -> FMaterialProgramValidationResult;

#if DURIN_WITH_EDITORONLY_DATA
		// Load-only field for packages authored before expression-owned layout.
		DPROPERTY(Deprecated)
		FMaterialGraphPresentation GraphPresentation_DEPRECATED;
#endif

		// Detached code checkpoint classifies reflected default/metadata edits without retaining resources.
		FXxHash128 ObservedExpressionCode;

		// Transient revision validates program-dependent caches and snapshots.
		uint64 MaterialProgramRevision = 1;


		friend struct Private::FMaterialCompilationLifecycle;
	};
}
