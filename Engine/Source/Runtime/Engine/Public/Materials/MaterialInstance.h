#pragma once

#include "Materials/MaterialInterface.h"
#include "Texture/Texture2D.h"

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
		ENGINE_API auto GetParent() const -> DMaterialInterface* override;
		ENGINE_API auto GetStaticProperties() const -> const FMaterialStaticProperties& override;
		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		ENGINE_API auto GetParameterOverrides() const -> std::span<const FMaterialParameterOverride>;
		auto IsImporterManaged() const -> bool { return ImportOwner.IsValid(); }
		auto GetImportOwner() const -> const FAssetPath& { return ImportOwner; }
		static constexpr std::string_view ImporterManagedPolicy =
			"Reimport replaces imported parameter values. Create a child material "
			"instance or component override for durable customization.";
		ENGINE_API auto SetImportOwner(const FAssetPath& InOwner) -> void;
		ENGINE_API auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		ENGINE_API auto SetParameterOverride(
			const FGuid& Id,
			EMaterialParameterType Type,
			const FMaterialParameterValue& Value
		) -> bool;
		ENGINE_API auto ClearParameterOverride(const FGuid& Id) -> bool;
		ENGINE_API auto HasLocalParameterOverride(const FGuid& Id) const -> bool;
		ENGINE_API auto IsParameterOverrideOrphan(const FGuid& Id) const -> bool;
		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> bool;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> bool;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool;
		ENGINE_API auto ClearScalarParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearVectorParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearTextureParameterValue(FName Name) -> bool;
		ENGINE_API auto HasScalarParameterOverride(FName Name) const -> bool;
		ENGINE_API auto HasVectorParameterOverride(FName Name) const -> bool;
		ENGINE_API auto HasTextureParameterOverride(FName Name) const -> bool;
		ENGINE_API auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		ENGINE_API auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		ENGINE_API auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		// Exchanges importer-owned parent and parameter state without changing
		// package or object identity. Used by atomic multi-asset reimport.
		ENGINE_API auto ExchangeImportedState(DMaterialInstance& Other) -> void;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> Parent;

		DPROPERTY(Edit)
		std::vector<FMaterialParameterOverride> ParameterOverrides;

		DPROPERTY()
		FAssetPath ImportOwner;
	};
}
