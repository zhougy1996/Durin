#pragma once

#include "Materials/MaterialInterface.h"
#include "Texture/Texture2D.h"
#include "Materials/MaterialParameterOverrides.h"

#include "MaterialInstance.gen.h"

namespace Durin
{
	// Resolves inherited material parameters and stores local overrides by stable identifier.
	DCLASS()
	class DMaterialInstance : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialInstance(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto SetParent(DMaterialInterface* InParent) -> bool;
		// Validates and applies a related parent/configuration edit with one request.
		ENGINE_API auto SetParentAndPropertyOverrides(DMaterialInterface* InParent,
			const FMaterialPropertyOverrides& Overrides) -> bool;
		ENGINE_API auto GetParent() const -> DMaterialInterface* override;
		ENGINE_API auto GetStaticProperties() const -> const FMaterialStaticProperties& override;
		ENGINE_API auto GetRenderableStaticProperties() const
			-> FMaterialStaticProperties override;
		ENGINE_API auto GetMaterialProgram() const
			-> const FMaterialProgram* override;
		ENGINE_API auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult> override;
		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		// Read-only projection; storage edits and reference collection invalidate its spans.
		ENGINE_API auto GetParameterOverrides() const -> std::span<const FMaterialParameterOverride>;
		ENGINE_API auto SetPropertyOverrides(const FMaterialPropertyOverrides& Overrides) -> bool;
		auto GetPropertyOverrides() const -> const FMaterialPropertyOverrides& { return PropertyOverrides; }
		ENGINE_API auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		// Authored assets admit edits before compilation; cooked assets use the compiled contract.
		ENGINE_API auto SetParameterOverride(
			const FGuid& Id,
			EMaterialParameterType Type,
			const FMaterialParameterValue& Value
		) -> bool;
		ENGINE_API auto ClearParameterOverride(const FGuid& Id) -> bool;
		ENGINE_API auto HasLocalParameterOverride(const FGuid& Id) const -> bool;
		ENGINE_API auto IsParameterOverrideOrphan(const FGuid& Id) const -> bool;
		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> bool;
		ENGINE_API auto SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> bool;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool;
		ENGINE_API auto ClearScalarParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearVector2ParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearVectorParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearTextureParameterValue(FName Name) -> bool;
		ENGINE_API auto HasScalarParameterOverride(FName Name) const -> bool;
		ENGINE_API auto HasVector2ParameterOverride(FName Name) const -> bool;
		ENGINE_API auto HasVectorParameterOverride(FName Name) const -> bool;
		ENGINE_API auto HasTextureParameterOverride(FName Name) const -> bool;
		ENGINE_API auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		ENGINE_API auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool override;
		ENGINE_API auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		ENGINE_API auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	protected:
		ENGINE_API auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer override;

	private:
		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> Parent;

		DPROPERTY(AlwaysSerialize)
		uint32 OverrideStorageVersion = 1;

		DPROPERTY(Edit)
		std::vector<FMaterialScalarParameterOverride> ScalarParameterOverrides;

		DPROPERTY(Edit)
		std::vector<FMaterialVector2ParameterOverride> Vector2ParameterOverrides;

		DPROPERTY(Edit)
		std::vector<FMaterialVectorParameterOverride> VectorParameterOverrides;

		DPROPERTY(Edit)
		std::vector<FMaterialVector4ParameterOverride> Vector4ParameterOverrides;

		DPROPERTY(Edit)
		std::vector<FMaterialTextureParameterOverride> TextureParameterOverrides;

		// Read-only projection; expires on a storage mutation or reference rewrite.
		mutable std::vector<FMaterialParameterOverride> ParameterOverrides;
		mutable bool bOverrideProjectionDirty = true;
		auto RebuildOverrideProjection() const -> void;
		auto ValidateOverrideStorage(const FPropertyEditProposal* Proposal = nullptr) const -> bool;
		template<typename TVisitor> auto VisitOverrideArrays(TVisitor&& Visitor) -> void
		{
			Visitor(ScalarParameterOverrides);
			Visitor(Vector2ParameterOverrides);
			Visitor(VectorParameterOverrides);
			Visitor(Vector4ParameterOverrides);
			Visitor(TextureParameterOverrides);
		}
		template<typename TVisitor> auto VisitOverrideArrays(TVisitor&& Visitor) const -> void
		{
			Visitor(ScalarParameterOverrides);
			Visitor(Vector2ParameterOverrides);
			Visitor(VectorParameterOverrides);
			Visitor(Vector4ParameterOverrides);
			Visitor(TextureParameterOverrides);
		}

		DPROPERTY(Edit)
		FMaterialPropertyOverrides PropertyOverrides;

		mutable FMaterialStaticProperties ResolvedStaticProperties;
	};
}
