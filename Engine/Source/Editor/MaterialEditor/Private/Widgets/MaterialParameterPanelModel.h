#pragma once

#include "CoreMinimal.h"

#include "PropertyEditor/PropertyView.h"
#include "Materials/MaterialTypes.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterialInstance;
	class DMaterial;
	struct FMaterialParameterReachability;
}

namespace Durin::MaterialEditor
{

	// Selects the editor control appropriate for one material parameter schema.
	enum class EMaterialParameterControlKind : uint8
	{
		Scalar,
		RangedScalar,
		IntegerScalar,
		Vector,
		Color,
		AssetPicker,
		Unsupported,
	};

	// Captures one resolved material parameter and its override provenance.
	struct FMaterialParameterPanelEntry
	{
		// Rows own a schema snapshot because applying a reflected collection-root
		// edit may reallocate the material's live definition array.
		std::optional<FMaterialParameterDefinition> Definition;
		FGuid ParameterId;
		FMaterialParameterInfo ParameterInfo;
		FMaterialParameterValue Value;
		DMaterialInterface* Source = nullptr;
		EMaterialParameterControlKind Control = EMaterialParameterControlKind::Unsupported;
		bool bCanOverride = false;
		bool bHasLocalOverride = false;
		bool bOrphan = false;
	};

	// Translates the runtime-owned schema and resolution state into editor rows.
	// It deliberately contains no built-in parameter names or widget-specific state.
	class FMaterialParameterPanelModel
	{
	public:
		explicit FMaterialParameterPanelModel(DMaterialInterface* InMaterial);
		~FMaterialParameterPanelModel();
		FMaterialParameterPanelModel(const FMaterialParameterPanelModel&) = delete;
		auto operator=(const FMaterialParameterPanelModel&) -> FMaterialParameterPanelModel& = delete;
		auto NeedsRefresh() const -> bool { return *Invalidated; }
		// Refreshes row snapshots; returns whether the dependency structure was rebuilt.
		auto Refresh() -> bool;

		auto GetMaterial() const -> DMaterialInterface* { return Material; }
		auto GetInstance() const -> DMaterialInstance* { return Instance; }
		auto GetEntries() const -> std::span<const FMaterialParameterPanelEntry> { return Entries; }

		static auto SelectControl(const FMaterialParameterDefinition& Definition) -> EMaterialParameterControlKind;
		auto SubmitValueEdit(
			FPropertyView& PropertyView,
			const FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry,
			const FMaterialParameterValue& Value,
			bool bContinuous
		) const -> bool;
		auto UpdateValueEdit(FPropertyView& PropertyView, const FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry, const MonaImGui::PropertyEdit::FPropertyInteractionState& Interaction,
			const FMaterialParameterValue* ProposedValue = nullptr) const -> bool;
		auto SetOverrideEnabled(
			FPropertyView& PropertyView,
			const FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry,
			bool bEnabled
		) const -> bool;
		auto RemoveOrphan(
			FPropertyView& PropertyView,
			const FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry
		) const -> bool;

	private:
		auto PrepareValueEdit(const FMaterialParameterPanelEntry& Entry, const FMaterialParameterValue* Value,
			FPropertyEditTarget& Target, std::function<void(FProperty*, void*, uint32)>& Assign) const -> bool;
		std::shared_ptr<bool> Invalidated = std::make_shared<bool>(true);
		TWeakObjectPtr<DMaterialInterface> ObservedMaterial;
		FDelegateHandle ChangeHandle;
		DMaterialInterface* Material = nullptr;
		DMaterialInstance* Instance = nullptr;
		// Values and presentation metadata do not invalidate graph reachability.
		DMaterial* DependencyMaterial = nullptr;
		std::vector<std::pair<FMaterialParameterInfo, EMaterialParameterType>> DependencySchema;
		std::shared_ptr<const FMaterialParameterReachability> DependencyReachability;
		bool bDependenciesInitialized = false;
		std::vector<FMaterialParameterPanelEntry> Entries;
	};
}
