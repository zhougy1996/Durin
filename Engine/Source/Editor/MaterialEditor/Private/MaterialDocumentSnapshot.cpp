#include "MaterialDocumentSnapshot.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Property.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"

namespace Durin::Editor::Material
{
	namespace
	{
		auto GetAuthoredPropertyNames(const DMaterialInterface& Material)
			-> std::span<const char* const>
		{
			static constexpr std::array BaseMaterialProperties{
				"StaticProperties",
				"ParameterDefinitions",
				"Program",
				"GraphPresentation",
			};
			static constexpr std::array InstanceProperties{
				"Parent",
				"ParameterOverrides",
				"bOverrideStaticProperties",
				"StaticPropertiesOverride",
			};
			if (Material.IsA<DMaterial>()) return BaseMaterialProperties;
			if (Material.IsA<DMaterialInstance>()) return InstanceProperties;
			return {};
		}
	}

	auto FMaterialDocumentSnapshot::Capture(
		DMaterialInterface& Material, std::string& OutError) -> bool
	{
		OutError.clear();
		const std::span PropertyNames = GetAuthoredPropertyNames(Material);
		if (PropertyNames.empty())
		{
			OutError = "The material type does not support editable document snapshots.";
			return false;
		}

		std::vector<FEntry> Captured;
		for (const char* PropertyName : PropertyNames)
		{
			FProperty* Property = Material.GetClass()->FindPropertyByName(
				FName(PropertyName));
			if (!Property)
			{
				OutError = std::format(
					"The material snapshot property '{}' is unavailable.", PropertyName);
				return false;
			}
			for (uint32 ArrayIndex = 0;
				ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				FEntry Entry{.Property = Property, .ArrayIndex = ArrayIndex};
				if (!CapturePropertyValue(Property, &Material,
					ArrayIndex, Entry.Value, &OutError)) return false;
				Captured.push_back(std::move(Entry));
			}
		}

		MaterialClass = Material.GetClass();
		Entries = std::move(Captured);
		return true;
	}

	auto FMaterialDocumentSnapshot::Restore(
		DMaterialInterface& Material, std::string& OutError) const -> bool
	{
		OutError.clear();
		if (!MaterialClass || Material.GetClass() != MaterialClass || Entries.empty())
		{
			OutError = "The material document snapshot does not match the open asset.";
			return false;
		}

		std::vector<FEntry> Rollback;
		Rollback.reserve(Entries.size());
		for (const FEntry& Entry : Entries)
		{
			FEntry Current{.Property = Entry.Property, .ArrayIndex = Entry.ArrayIndex};
			if (!CapturePropertyValue(Entry.Property, &Material,
				Entry.ArrayIndex, Current.Value, &OutError)) return false;
			Rollback.push_back(std::move(Current));
		}

		auto RestoreEntries = [&Material](std::span<const FEntry> Values,
			std::string& Error) {
			for (const FEntry& Entry : Values)
				if (!RestorePropertyValue(Entry.Property, &Material,
					Entry.ArrayIndex, Entry.Value, &Error)) return false;
			return true;
		};
		auto RollBack = [&]() {
			std::string IgnoredError;
			(void)RestoreEntries(Rollback, IgnoredError);
			(void)Material.PostLoad(IgnoredError);
		};
		if (!RestoreEntries(Entries, OutError))
		{
			RollBack();
			return false;
		}
		if (!Material.PostLoad(OutError))
		{
			RollBack();
			return false;
		}
		if (auto* BaseMaterial = Cast<DMaterial>(&Material))
		{
			(void)FAssetCompilingManager::Get().FinishCompilationForObject(*BaseMaterial);
			if (!BaseMaterial->GetAcceptedCompiledProgram())
			{
				OutError = "The discarded material state did not produce a renderable program.";
				RollBack();
				return false;
			}
		}
		return true;
	}
}
