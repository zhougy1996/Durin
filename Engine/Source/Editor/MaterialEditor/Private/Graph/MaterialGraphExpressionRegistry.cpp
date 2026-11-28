#include "MaterialGraphExpressionRegistry.h"
#include "Materials/MaterialExpression.h"
#include "DObject/Class.h"
#include "Misc/AssertionMacros.h"

namespace Durin::MaterialEditor
{
	auto GetMaterialGraphExpressionRegistrations() -> std::span<const FMaterialGraphExpressionRegistration>
	{
		static const std::vector<FMaterialGraphExpressionRegistration> Registrations = [] {
			using Kind = EMaterialGraphCreationKind;
			using Shape = EMaterialGraphPaletteShape;
			std::vector<FMaterialGraphExpressionRegistration> Result;
			for (const auto& Description : GetMaterialExpressionDescriptions())
			{
				const auto* Class = Description.ExpressionClass;
				if (!Description.bRegisterForEditorCreation) continue;

				const auto CreationKind = Description.bRequiresAsset ? Kind::AssetBound : Kind::Direct;
				const auto PaletteShape = Description.bRequiresAsset || !Description.bShowInPalette ? Shape::InspectOnly
					: Description.OutputBehavior == EMaterialExpressionOutputBehavior::AdaptiveNumeric ? Shape::Adaptive : Shape::All;
				requiref(CreationKind != Kind::Direct || (Description.SemanticOpcode && !Description.Shapes.empty()),
					"Direct graph expression registration for {} requires complete authoring shapes.", Class->GetName());
				Result.push_back({&Description, CreationKind, PaletteShape});
			}
			return Result;
		}();
		return Registrations;
	}

	auto FindMaterialGraphExpressionRegistration(const DClass* Class) -> const FMaterialGraphExpressionRegistration*
	{
		static const auto ByClass = [] {
			std::unordered_map<const DClass*, const FMaterialGraphExpressionRegistration*> Result;
			for (const auto& Registration : GetMaterialGraphExpressionRegistrations())
				Result.emplace(Registration.Description->ExpressionClass, &Registration);
			return Result;
		}();
		const auto It = ByClass.find(Class);
		return It == ByClass.end() ? nullptr : It->second;
	}

	auto NewRegisteredMaterialExpression(DClass* Class) -> DMaterialExpression*
	{
		if (!FindMaterialGraphExpressionRegistration(Class)) return nullptr;
		return NewObject<DMaterialExpression>(Class, nullptr, NAME_None);
	}
}
