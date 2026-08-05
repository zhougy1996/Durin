#pragma once

#include "Editor/ReflectedPropertyView.h"
#include "Materials/MaterialTypes.h"

namespace Durin
{
	class DMaterialInterface;
	class DMaterialInstance;

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

		auto GetMaterial() const -> DMaterialInterface* { return Material; }
		auto GetInstance() const -> DMaterialInstance* { return Instance; }
		auto GetEntries() const -> std::span<const FMaterialParameterPanelEntry> { return Entries; }

		static auto SelectControl(const FMaterialParameterDefinition& Definition) -> EMaterialParameterControlKind;
		auto SubmitValueEdit(
			FReflectedPropertyView& PropertyView,
			const FReflectedPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry,
			const FMaterialParameterValue& Value,
			bool bContinuous
		) const -> bool;
		auto SetOverrideEnabled(
			FReflectedPropertyView& PropertyView,
			const FReflectedPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry,
			bool bEnabled
		) const -> bool;
		auto RemoveOrphan(
			FReflectedPropertyView& PropertyView,
			const FReflectedPropertyViewContext& Context,
			const FMaterialParameterPanelEntry& Entry
		) const -> bool;

	private:
		DMaterialInterface* Material = nullptr;
		DMaterialInstance* Instance = nullptr;
		std::vector<FMaterialParameterPanelEntry> Entries;
	};
}
