#include "Materials/MaterialExpressionBuild.h"
#include "MaterialFunctionPreview.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphExpressionState.h"

namespace Durin::Editor::Material
{
	auto BuildMaterialFunctionPreview(DMaterialFunctionInterface& Function,
		const FGuid& OutputId, DMaterial& Preview) -> FMaterialGraphCommandResult
	{
		using namespace GraphEditInternals;
		using Type = EMaterialProgramValueType;
		const auto& Signature = Function.GetFunctionSignature();
		const auto Output = std::ranges::find(Signature.Outputs, OutputId, &FMaterialFunctionPort::Id);
		if (Output == Signature.Outputs.end()) return MakeRejected("The preview output no longer exists.");
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure);
		if (!Validation) return MakeRejected("The function cannot be previewed.", std::move(Validation.Diagnostics));
		FOwnedGraphSnapshot State;
		const auto Add = [&]<typename Expression>() -> Expression* {
			auto* Value = NewObject<Expression>(nullptr, NAME_None);
			Value->Id = FGuid::NewGuid();
			State.Expressions.emplace_back(Value);
			return Value;
		};
		std::array<FMaterialExpressionInput, 6> RequiredValues;
		auto* Call = Add.operator()<DMaterialExpressionFunctionCall>();
		Call->Function = &Function;
		for (const auto& Port : Signature.Outputs) Call->Outputs.push_back({Port.Id, Port.Type});
		for (const auto& Port : Signature.Inputs)
			if (Port.bRequired)
			{
				auto& Source = RequiredValues[static_cast<size_t>(Port.Type)];
				if (!Source.ExpressionId.IsValid())
				{
					switch (Port.Type)
					{
					case Type::Float: Source = {Add.operator()<DMaterialExpressionScalarConstant>()->Id}; break;
					case Type::Float2: Source = {Add.operator()<DMaterialExpressionVector2Constant>()->Id}; break;
					case Type::Float3: Source = {Add.operator()<DMaterialExpressionVector3Constant>()->Id}; break;
					case Type::Float4: Source = {Add.operator()<DMaterialExpressionVector4Constant>()->Id}; break;
					case Type::Texture2D:
					{
						auto* Texture = Add.operator()<DMaterialExpressionTextureParameter>();
						Texture->Metadata.Id = FGuid::NewGuid();
						Texture->Metadata.Name = "PreviewTexture";
						Source = {Texture->Id}; break;
					}
					case Type::Surface:
					{
						auto* Surface = Add.operator()<DMaterialExpressionMakeSurface>();
						Surface->BaseColorDefault = {.5f, .5f, .5f}; Surface->NormalDefault = {0, 0, 1};
						Surface->MetallicDefault = {0}; Surface->RoughnessDefault = {.5f};
						Surface->AmbientOcclusionDefault = {1}; Surface->EmissiveDefault = {0, 0, 0};
						Surface->OpacityDefault = {1}; Surface->OpacityMaskDefault = {1};
						Source = {Surface->Id}; break;
					}
					default: return MakeRejected("The preview input type is unsupported.");
					}
				}
				Call->Inputs.push_back({Port.Id, Port.Type, Source});
			}
		FMaterialExpressionInput Value{Call->Id, 0, OutputId};
		FMaterialStaticProperties Properties;
		if (Output->Type == Type::Surface) State.Outputs.Surface = Value;
		else
		{
			Properties.ShadingModel = EMaterialShadingModel::Unlit;
			if (Output->Type == Type::Texture2D)
			{
				auto* Sample = Add.operator()<DMaterialExpressionTextureSample2D>();
				Sample->Texture = Value; Value = {Sample->Id};
			}
			if (Output->Type == Type::Float)
			{
				auto* Splat = Add.operator()<DMaterialExpressionSplat3>();
				Splat->Input = Value; Value = {Splat->Id};
			}
			else if (Output->Type == Type::Float2)
			{
				auto* X = Add.operator()<DMaterialExpressionSwizzle>(); X->Input = Value; X->Components = {0};
				auto* Y = Add.operator()<DMaterialExpressionSwizzle>(); Y->Input = Value; Y->Components = {1};
				auto* Vector = Add.operator()<DMaterialExpressionMakeVector3>();
				Vector->X = {X->Id}; Vector->Y = {Y->Id}; Vector->ZDefault = {0}; Value = {Vector->Id};
			}
			else if (Output->Type != Type::Float3)
			{
				auto* Truncate = Add.operator()<DMaterialExpressionTruncateToVector3>();
				Truncate->Input = Value; Value = {Truncate->Id};
			}
			State.Outputs.Emissive = Value;
		}
		for (size_t Index = 0; Index < State.Expressions.size(); ++Index)
			State.Presentation.Nodes.push_back({State.Expressions[Index]->Id, static_cast<int32>(Index % 4) * 320,
				static_cast<int32>(Index / 4) * 240});
		State.Presentation.bHasMaterialOutputPosition = true;
		State.Presentation.MaterialOutputX = 1280;
		auto Result = CommitOwnedExpressions(Preview, std::move(State), "Build Function Preview", nullptr);
		if (Result && !Preview.SetStaticProperties(Properties)) return MakeRejected("The preview properties are invalid.");
		return Result;
	}
}
