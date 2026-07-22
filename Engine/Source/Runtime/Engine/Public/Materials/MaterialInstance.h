#pragma once

#include "Materials/MaterialInterface.h"
#include "Texture/Texture2D.h"

#include "MaterialInstance.gen.h"

namespace Durin
{
	DCLASS()
	class ENGINE_API DMaterialInstance : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		explicit DMaterialInstance(const FObjectInitializer& ObjectInitializer);

		auto SetParent(DMaterialInterface* InParent) -> bool;
		auto GetParent() const -> DMaterialInterface* override;
		auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		auto SetScalarParameterValue(FName Name, float Value) -> bool;
		auto SetVectorParameterValue(FName Name, const FVector3& Value) -> bool;
		auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool;
		auto ClearScalarParameterValue(FName Name) -> bool;
		auto ClearVectorParameterValue(FName Name) -> bool;
		auto ClearTextureParameterValue(FName Name) -> bool;
		auto HasScalarParameterOverride(FName Name) const -> bool;
		auto HasVectorParameterOverride(FName Name) const -> bool;
		auto HasTextureParameterOverride(FName Name) const -> bool;
		auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		auto BeginDestroy() -> void override;
		auto PostLoad(std::string& OutError) -> bool override;
		auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		auto OnParentRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void;
		auto ReconcileParentDependency() -> void;

		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> Parent;
		TObjectPtr<DMaterialInterface> RegisteredParent;

		DPROPERTY(Edit)
		std::unordered_map<std::string, float> ScalarParameterOverrides;

		DPROPERTY(Edit)
		std::unordered_map<std::string, FVector3> VectorParameterOverrides;

		DPROPERTY(Edit)
		std::unordered_map<std::string, TObjectPtr<DTexture2D>> TextureParameterOverrides;

		friend class DMaterialInterface;
	};
}
