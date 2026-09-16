#pragma once

#include "Editor/PropertyView.h"
#include "Materials/MaterialTypes.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterialInstance;
	class DMaterial;
	struct FMaterialParameterReachability;
}

namespace Durin::Editor::Material
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
			::Durin::Editor::FPropertyView& PropertyView,
			const ::Durin::Editor::FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry,
			const FMaterialParameterValue& Value,
			bool bContinuous
		) const -> bool;
		auto SetOverrideEnabled(
			::Durin::Editor::FPropertyView& PropertyView,
			const ::Durin::Editor::FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry,
			bool bEnabled
		) const -> bool;
		auto RemoveOrphan(
			::Durin::Editor::FPropertyView& PropertyView,
			const ::Durin::Editor::FPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry
		) const -> bool;

	private:
		std::shared_ptr<bool> Invalidated = std::make_shared<bool>(true);
		TWeakObjectPtr<DMaterialInterface> ObservedMaterial;
		FDelegateHandle ChangeHandle;
		DMaterialInterface* Material = nullptr;
		DMaterialInstance* Instance = nullptr;
		// Values and presentation metadata do not invalidate graph reachability.
		DMaterial* DependencyMaterial = nullptr;
		std::vector<std::pair<FGuid, EMaterialParameterType>> DependencySchema;
		std::vector<FGuid> ParameterIds;
		std::shared_ptr<const FMaterialParameterReachability> DependencyReachability;
		bool bDependenciesInitialized = false;
		std::vector<FMaterialParameterPanelEntry> Entries;
	};
}
