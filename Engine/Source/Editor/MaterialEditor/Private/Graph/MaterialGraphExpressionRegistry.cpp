#include "MaterialGraphExpressionRegistry.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionCustomData.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "DObject/Class.h"
#include "Misc/AssertionMacros.h"

namespace Durin::MaterialEditor
{
	auto GetMaterialGraphExpressionRegistrations() -> std::span<const FMaterialGraphExpressionRegistration>
	{
		static const std::vector<FMaterialGraphExpressionRegistration> Registrations = [] {
			using Kind = EMaterialGraphCreationKind;
			using Shape = EMaterialGraphPaletteShape;
			struct FPolicy { Kind CreationKind; Shape PaletteShape; };
			const std::unordered_map<const DClass*, FPolicy> Overrides{
				{DMaterialExpressionScalarConstant::StaticClass(), {Kind::Direct, Shape::Scalar}},
				{DMaterialExpressionVector2Constant::StaticClass(), {Kind::Direct, Shape::Scalar}},
				{DMaterialExpressionVector3Constant::StaticClass(), {Kind::Direct, Shape::Scalar}},
				{DMaterialExpressionVector4Constant::StaticClass(), {Kind::Direct, Shape::Scalar}},
				{DMaterialExpressionScalarParameter::StaticClass(), {Kind::Direct, Shape::ScalarOrVector4}},
				{DMaterialExpressionVector4Parameter::StaticClass(), {Kind::Direct, Shape::ScalarOrVector4}},
				{DMaterialExpressionCollectionParameter::StaticClass(), {Kind::AssetBound, Shape::InspectOnly}},
				{DMaterialExpressionNormalize::StaticClass(), {Kind::Direct, Shape::AdaptiveVector}},
				{DMaterialExpressionMakeVector2::StaticClass(), {Kind::Direct, Shape::InspectOnly}},
				{DMaterialExpressionMakeVector3::StaticClass(), {Kind::Direct, Shape::InspectOnly}},
				{DMaterialExpressionMakeVector4::StaticClass(), {Kind::Direct, Shape::InspectOnly}},
				{DMaterialExpressionAppendVector::StaticClass(), {Kind::Direct, Shape::Float2}},
				{DMaterialExpressionSwizzle::StaticClass(), {Kind::Direct, Shape::Scalar}},
				{DMaterialExpressionSplat2::StaticClass(), {Kind::Direct, Shape::InspectOnly}},
				{DMaterialExpressionSplat3::StaticClass(), {Kind::Direct, Shape::InspectOnly}},
				{DMaterialExpressionSplat4::StaticClass(), {Kind::Direct, Shape::InspectOnly}},
				{DMaterialExpressionReflect::StaticClass(), {Kind::Direct, Shape::AdaptiveVector}},
				{DMaterialExpressionFunctionInput::StaticClass(), {Kind::AssetBound, Shape::InspectOnly}},
				{DMaterialExpressionFunctionOutput::StaticClass(), {Kind::AssetBound, Shape::InspectOnly}},
				{DMaterialExpressionFunctionCall::StaticClass(), {Kind::AssetBound, Shape::InspectOnly}},
			};
			std::vector<FMaterialGraphExpressionRegistration> Result;
			for (const auto& Description : GetMaterialExpressionDescriptions())
			{
				const auto* Class = Description.ExpressionClass;
				if (Class == DMaterialExpressionPrimitiveCustomData::StaticClass()
					|| Class == DMaterialExpressionInstanceCustomData::StaticClass()) continue;

				FPolicy Policy{Kind::Direct, Description.OutputBehavior == EMaterialExpressionOutputBehavior::AdaptiveNumeric
					? Shape::Adaptive : Shape::All};
				if (const auto It = Overrides.find(Class); It != Overrides.end()) Policy = It->second;
				requiref(Policy.CreationKind != Kind::Direct || (Description.SemanticOpcode && !Description.Shapes.empty()),
					"Direct graph expression registration for {} requires complete authoring shapes.", Class->GetName());
				Result.push_back({&Description, Policy.CreationKind, Policy.PaletteShape});
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
