#include "Materials/MaterialExpressionDescription.h"
#include "Materials/MaterialExpression.h"
#include "DObject/Class.h"
#include "Misc/AssertionMacros.h"

namespace Durin
{
	namespace
	{
		auto GetCatalogInputName(const FMaterialExpressionInputInfo& Input) -> std::string_view
		{
			if (!Input.CatalogAlias.empty()) return Input.CatalogAlias;
			return Input.Name.empty() ? "Value" : Input.Name;
		}

		auto DescribeClass(DClass* Class, const DMaterialExpression& Default, const FMaterialExpressionCreationInfo& Info)
			-> FMaterialExpressionDescription
		{
			using Behavior = EMaterialExpressionOutputBehavior;
			const auto OutputBehavior = Info.bHasInstanceShape ? Behavior::Instance
				: Info.Opcode && IsMaterialAdaptiveNumeric(*Info.Opcode) ? Behavior::AdaptiveNumeric : Behavior::Fixed;
			FMaterialExpressionDescription Result{Class, Info.Opcode, Info.Category, Info.Name, Info.Description, Info.SearchKeywords,
				OutputBehavior, {}, Info.bRequiresAsset, Info.PaletteDefault, Info.bShowInPalette, Info.bRegisterForEditorCreation};
			if (!Info.Opcode)
			{
				auto Type = Info.FixedOutputType;
#if DURIN_WITH_EDITORONLY_DATA
				if (!Type) Type = Default.GetOutputValueType(0);
#endif
				if (!Type) return Result;
				FMaterialExpressionAuthoringShape Shape{*Type, {}, {}};
				for (uint32 Index = 0; Index < Default.GetAuthoredInputCount(); ++Index)
				{
					const auto Input = Default.GetInputInfo(static_cast<int32>(Index));
					if (!Input.Type) return Result;
					Shape.InputNames.emplace_back(GetCatalogInputName(Input));
					Shape.AcceptedInputTypes.push_back({*Input.Type});
					if (Input.bAllowImplicitCast) AppendMaterialValidCastSources(Shape.AcceptedInputTypes.back());
				}
				Result.Shapes.push_back(std::move(Shape));
				return Result;
			}
			const auto Opcode = *Info.Opcode;
			for (uint8 TypeValue = 0; TypeValue <= static_cast<uint8>(EMaterialProgramValueType::StaticBool); ++TypeValue)
			{
				const auto Type = static_cast<EMaterialProgramValueType>(TypeValue);
				if (Info.FixedOutputType && Type != *Info.FixedOutputType) continue;
				const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
				if (!Signature) continue;
				FMaterialExpressionAuthoringShape Shape{Type, {}, {}};
				for (uint8 Index = 0; Index < Signature->InputCount; ++Index)
				{
					Shape.InputNames.emplace_back(GetCatalogInputName(Default.GetInputInfo(Index)));
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
					Shape.InputNames.emplace_back(GetCatalogInputName(Input));
					Shape.AcceptedInputTypes.push_back({*Input.Type});
				}
				Result.Shapes.push_back(std::move(Shape));
			}
			return Result;
		}

		struct FExpressionDescriptions
		{
			std::vector<FMaterialExpressionDescription> Descriptions;
			std::unordered_map<const DClass*, size_t> ByClass;
		};

		auto GetExpressionDescriptions() -> const FExpressionDescriptions&
		{
			// Reflection and its class defaults must be initialized before the first call.
			static const auto Cache = [] {
				FExpressionDescriptions Result;
				for (auto* Class : GetDerivedClasses(DMaterialExpression::StaticClass()))
				{
					if (Class->HasAnyClassFlags(EClassFlags::Abstract)) continue;
					const auto* Default = Cast<DMaterialExpression>(Class->GetDefaultObject());
					if (!Default) continue;
					const auto Info = Default->GetCreationInfo();
					if (Info.Name.empty()) continue;
					const bool bInserted = Result.ByClass.emplace(Class, Result.Descriptions.size()).second;
					requiref(bInserted, "Material expression descriptions require unique reflected classes.");
					Result.Descriptions.push_back(DescribeClass(Class, *Default, Info));
				}
				return Result;
			}();
			return Cache;
		}
	}

	auto GetMaterialExpressionDescriptions() -> std::span<const FMaterialExpressionDescription>
	{
		return GetExpressionDescriptions().Descriptions;
	}

	auto FindMaterialExpressionDescription(const DClass* Class) -> const FMaterialExpressionDescription*
	{
		const auto& Cache = GetExpressionDescriptions();
		const auto It = Cache.ByClass.find(Class);
		return It == Cache.ByClass.end() ? nullptr : &Cache.Descriptions[It->second];
	}
}
