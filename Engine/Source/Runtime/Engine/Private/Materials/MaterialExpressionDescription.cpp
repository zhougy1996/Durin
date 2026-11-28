#include "Materials/MaterialExpressionDescription.h"
#include "Materials/MaterialExpression.h"
#include "DObject/Class.h"
#include "Misc/AssertionMacros.h"

namespace Durin
{
	namespace
	{
		auto DescribeClass(DClass* Class, const DMaterialExpression& Default, const FMaterialExpressionCreationInfo& Info)
			-> FMaterialExpressionDescription
		{
			using Behavior = EMaterialExpressionOutputBehavior;
			const auto Opcode = *Info.Opcode;
			const auto OutputBehavior = Info.bHasInstanceShape ? Behavior::Instance
				: IsMaterialAdaptiveNumeric(Opcode) ? Behavior::AdaptiveNumeric : Behavior::Fixed;
			FMaterialExpressionDescription Result{Class, Opcode, Info.Category, Info.Name, Info.Description, Info.SearchKeywords,
				OutputBehavior, {}, Info.bRequiresAsset, Info.PaletteDefault, Info.bShowInPalette, Info.bRegisterForEditorCreation};
			for (uint8 TypeValue = 0; TypeValue <= static_cast<uint8>(EMaterialProgramValueType::StaticBool); ++TypeValue)
			{
				const auto Type = static_cast<EMaterialProgramValueType>(TypeValue);
				if (Info.FixedOutputType && Type != *Info.FixedOutputType) continue;
				const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
				if (!Signature) continue;
				FMaterialExpressionAuthoringShape Shape{Type, {}, {}};
				for (uint8 Index = 0; Index < Signature->InputCount; ++Index)
				{
					const auto Name = Index < Info.InputNames.size() && !Info.InputNames[Index].empty()
						? Info.InputNames[Index] : Default.GetInputName(Index);
					Shape.InputNames.emplace_back(Name.empty() ? "Value" : Name);
					Shape.AcceptedInputTypes.emplace_back(Signature->Inputs[Index].begin(), Signature->Inputs[Index].end());
					if (!MaterialInputAllowsValidCast(Opcode, Index)) continue;
					auto& Accepted = Shape.AcceptedInputTypes.back();
					if (!IsMaterialAdaptiveNumeric(Opcode))
						AppendMaterialValidCastSources(Accepted);
					else if (Accepted.size() == 1 && Accepted.front() != EMaterialProgramValueType::Float
						&& CanValidCast(EMaterialProgramValueType::Float, Accepted.front()))
						Accepted.push_back(EMaterialProgramValueType::Float);
				}
				// An authored node can retain operands omitted from its default MIR signature.
				for (uint32 Index = Signature->InputCount; Index < Default.GetAuthoredInputCount(); ++Index)
				{
					const auto Input = Default.GetInputInfo(static_cast<int32>(Index));
					if (!Input.Type) break;
					Shape.InputNames.emplace_back(Input.Name);
					Shape.AcceptedInputTypes.push_back({*Input.Type});
				}
				Result.Shapes.push_back(std::move(Shape));
			}
			return Result;
		}
	}

	auto GetMaterialExpressionDescriptions() -> std::span<const FMaterialExpressionDescription>
	{
		// Reflection and its class defaults must be initialized before the first call.
		static const std::vector<FMaterialExpressionDescription> Descriptions = [] {
			std::vector<FMaterialExpressionDescription> Result;
			for (auto* Class : GetDerivedClasses(DMaterialExpression::StaticClass()))
			{
				if (Class->HasAnyClassFlags(EClassFlags::Abstract)) continue;
				const auto* Default = Cast<DMaterialExpression>(Class->GetDefaultObject());
				if (!Default) continue;
				const auto Info = Default->GetCreationInfo();
				if (!Info.Opcode) continue;
				Result.push_back(DescribeClass(Class, *Default, Info));
			}
			std::unordered_map<const DClass*, bool> Classes;
			for (const auto& Description : Result)
				requiref(Description.ExpressionClass && Classes.emplace(Description.ExpressionClass, true).second,
					"Material expression descriptions require unique reflected classes.");
			return Result;
		}();
		return Descriptions;
	}

	auto FindMaterialExpressionDescription(const DClass* Class) -> const FMaterialExpressionDescription*
	{
		static const auto ByClass = [] {
			std::unordered_map<const DClass*, const FMaterialExpressionDescription*> Result;
			for (const auto& Description : GetMaterialExpressionDescriptions())
				Result.emplace(Description.ExpressionClass, &Description);
			return Result;
		}();
		const auto It = ByClass.find(Class);
		return It == ByClass.end() ? nullptr : It->second;
	}
}
