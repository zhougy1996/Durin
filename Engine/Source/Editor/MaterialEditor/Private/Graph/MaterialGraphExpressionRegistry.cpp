#include "MaterialGraphExpressionRegistry.h"
#include "DObject/Class.h"
#include "Misc/AssertionMacros.h"
#include <unordered_map>

namespace Durin::Editor::Material
{
	namespace
	{
		struct FPolicy
		{
			DClass* (*ExpressionClass)();
			const char* Category;
			EMaterialGraphCreationKind CreationKind = EMaterialGraphCreationKind::Direct;
			EMaterialGraphPaletteShape PaletteShape = EMaterialGraphPaletteShape::All;
		};
		constexpr FPolicy Policies[] = {
			{&DMaterialExpressionScalarConstant::StaticClass, "Inputs", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Scalar},
			{&DMaterialExpressionVector2Constant::StaticClass, "Inputs", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Scalar},
			{&DMaterialExpressionVector3Constant::StaticClass, "Inputs", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Scalar},
			{&DMaterialExpressionVector4Constant::StaticClass, "Inputs", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Scalar},
			{&DMaterialExpressionScalarParameter::StaticClass, "Parameters", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::ScalarOrVector4},
			{&DMaterialExpressionVector4Parameter::StaticClass, "Parameters", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::ScalarOrVector4},
			{&DMaterialExpressionTextureParameter::StaticClass, "Parameters"},
			{&DMaterialExpressionTextureSampleParameter2D::StaticClass, "Parameters"},
			{&DMaterialExpressionWorldPosition::StaticClass, "Inputs"},
			{&DMaterialExpressionTime::StaticClass, "Inputs"},
			{&DMaterialExpressionCollectionParameter::StaticClass, "Parameters", EMaterialGraphCreationKind::AssetBound, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionCameraPosition::StaticClass, "Inputs"},
			{&DMaterialExpressionCameraVector::StaticClass, "Inputs"},
			{&DMaterialExpressionObjectPosition::StaticClass, "Inputs"},
			{&DMaterialExpressionVertexInterpolator::StaticClass, "Vertex", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionVertexNormal::StaticClass, "Inputs"},
			{&DMaterialExpressionScreenPosition::StaticClass, "Inputs"},
			{&DMaterialExpressionViewSize::StaticClass, "Inputs"},
			{&DMaterialExpressionStaticBool::StaticClass, "Static Selection"},
			{&DMaterialExpressionStaticSwitch::StaticClass, "Static Selection"},
			{&DMaterialExpressionQualitySwitch::StaticClass, "Static Selection"},
			{&DMaterialExpressionFeatureLevelSwitch::StaticClass, "Static Selection"},
			{&DMaterialExpressionTransformPosition::StaticClass, "Transforms"},
			{&DMaterialExpressionTransformDirection::StaticClass, "Transforms"},
			{&DMaterialExpressionTransformNormal::StaticClass, "Transforms"},
			{&DMaterialExpressionTextureCoordinates::StaticClass, "Inputs",
				EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::All},
			{&DMaterialExpressionTextureSample2D::StaticClass, "Textures"},
			{&DMaterialExpressionAdd::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionSubtract::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionMultiply::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionDivide::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionMinimum::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionMaximum::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionNegate::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionOneMinus::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionAbsolute::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionSaturate::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionNormalize::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::AdaptiveVector},
			{&DMaterialExpressionClamp::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionLerp::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionMakeVector2::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionMakeVector3::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionMakeVector4::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionAppendVector::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Float2},
			{&DMaterialExpressionSwizzle::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Scalar},
			{&DMaterialExpressionSplat2::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionSplat3::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionSplat4::StaticClass, "Channels", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionBlendNormalsRNM::StaticClass, "Textures"},
			{&DMaterialExpressionSine::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionCosine::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionDot::StaticClass, "Math"},
			{&DMaterialExpressionCross::StaticClass, "Math"},
			{&DMaterialExpressionLength::StaticClass, "Math"},
			{&DMaterialExpressionDistance::StaticClass, "Math"},
			{&DMaterialExpressionPow::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionSqrt::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionExp::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionLog::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionFloor::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionCeil::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionRound::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionFrac::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionFmod::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionStep::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionSmoothStep::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionSign::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::Adaptive},
			{&DMaterialExpressionReflect::StaticClass, "Math", EMaterialGraphCreationKind::Direct, EMaterialGraphPaletteShape::AdaptiveVector},
			{&DMaterialExpressionMakeSurface::StaticClass, "Surface"},
			{&DMaterialExpressionFunctionInput::StaticClass, "Functions", EMaterialGraphCreationKind::AssetBound, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionFunctionOutput::StaticClass, "Functions", EMaterialGraphCreationKind::AssetBound, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionFunctionCall::StaticClass, "Functions", EMaterialGraphCreationKind::AssetBound, EMaterialGraphPaletteShape::InspectOnly},
			{&DMaterialExpressionGetSurfaceAttributes::StaticClass, "Surface"},
			{&DMaterialExpressionSetSurfaceAttributes::StaticClass, "Surface"},
		};
	}

	auto GetMaterialGraphExpressionRegistrations() -> std::span<const FMaterialGraphExpressionRegistration>
	{
		static const std::vector<FMaterialGraphExpressionRegistration> Registrations = [] {
			std::vector<FMaterialGraphExpressionRegistration> Result;
			std::unordered_map<const DClass*, bool> Classes;
			for (const auto& Policy : Policies)
			{
				const auto* Description = FindMaterialExpressionDescription(Policy.ExpressionClass());
				requiref(Description && Classes.emplace(Description->ExpressionClass, true).second,
					"Graph expression registration requires one description per class.");
				requiref(Policy.CreationKind != EMaterialGraphCreationKind::Direct
					|| (Description->SemanticOpcode && !Description->Shapes.empty()),
					"Direct graph expression registration for {} requires complete authoring shapes.", Description->ExpressionClass->GetName());
				Result.push_back({Description, Policy.Category, Policy.CreationKind, Policy.PaletteShape});
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
