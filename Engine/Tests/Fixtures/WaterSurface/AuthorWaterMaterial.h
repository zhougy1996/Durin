#pragma once

#include "WaterMaterialRecipe.h"

#if DURIN_WITH_EDITOR
#include "Asset/PackageSerialization.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "DObject/DurinPropertyTypes.h"
#include "Misc/FileHelper.h"

// Explicit source-asset authoring utility. Cook only reads the saved graph.
inline auto AuthorWaterMaterial(const std::filesystem::path& Destination) -> void
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode;
	using Type = EMaterialProgramValueType;
	FPackagePath Path;
	if (!FPackagePath::TryCreate("/Plugins/Water/Materials/DefaultWater", Path)) throw std::runtime_error("Invalid material package path.");
	auto* Package = CreatePackage(Path);
	auto* Material = NewObject<DMaterial>(Package, "DefaultWater", EObjectFlags::Public);
	const auto Recipe = WaterMaterialRecipe::BuildInput();
	std::vector<TStrongObjectPtr<DMaterialExpression>> Owners;
	std::vector<DMaterialExpression*> Nodes;
	std::vector<FMaterialExpressionInput> Links;
	uint32 NextId = 1;
	auto Own = [&](DMaterialExpression* E) {
		E->Id = {0x57415452u, 0x4d41544cu, 0x00000001u, NextId++};
		Owners.emplace_back(E); Nodes.push_back(E);
		return FMaterialExpressionInput{E->Id};
	};
	for (const auto& N : Recipe.IR.Nodes)
	{
		DMaterialExpression* E = nullptr;
		if (N.Opcode == Op::Constant)
		{
			const auto V = N.GetLiteral();
			switch (N.ResultType)
			{
			case Type::Float: { auto* C = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); C->Value = V.X; E = C; break; }
			case Type::Float2: { auto* C = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); C->Value = {V.X, V.Y}; E = C; break; }
			case Type::Float3: { auto* C = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None); C->Value = {V.X, V.Y, V.Z}; E = C; break; }
			default: throw std::runtime_error("Unsupported Water constant.");
			}
		}
		else switch (N.Opcode)
		{
		case Op::Add: E = NewObject<DMaterialExpressionAdd>(nullptr, NAME_None); break;
		case Op::Subtract: E = NewObject<DMaterialExpressionSubtract>(nullptr, NAME_None); break;
		case Op::Multiply: E = NewObject<DMaterialExpressionMultiply>(nullptr, NAME_None); break;
		case Op::Divide: E = NewObject<DMaterialExpressionDivide>(nullptr, NAME_None); break;
		case Op::Sqrt: E = NewObject<DMaterialExpressionSqrt>(nullptr, NAME_None); break;
		case Op::Sine: E = NewObject<DMaterialExpressionSine>(nullptr, NAME_None); break;
		case Op::Cosine: E = NewObject<DMaterialExpressionCosine>(nullptr, NAME_None); break;
		case Op::Cross: E = NewObject<DMaterialExpressionCross>(nullptr, NAME_None); break;
		case Op::Normalize: E = NewObject<DMaterialExpressionNormalize>(nullptr, NAME_None); break;
		case Op::Splat3: E = NewObject<DMaterialExpressionSplat3>(nullptr, NAME_None); break;
		case Op::MakeFloat3: E = NewObject<DMaterialExpressionMakeVector3>(nullptr, NAME_None); break;
		case Op::DecodeNormalRG: {
			// The authored graph can classify the already normalized analytic
			// normal directly; the detached reference uses a normal decoder.
			const auto& Encoded = Recipe.IR.Nodes.at(N.Inputs.at(0));
			const auto& Scaled = Recipe.IR.Nodes.at(Encoded.Inputs.at(0));
			const auto& XY = Recipe.IR.Nodes.at(Scaled.Inputs.at(0));
			auto* C = NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None);
			C->Input = Links.at(XY.Inputs.at(0)); C->Components = {0, 1, 2};
			C->OutputSpatialKind = EMaterialSpatialKind::Normal; C->OutputCoordinateSpace = EMaterialCoordinateSpace::Tangent;
			Links.push_back(Own(C)); continue;
		}
		case Op::Time: E = NewObject<DMaterialExpressionTime>(nullptr, NAME_None); break;
		case Op::UVChannel: E = NewObject<DMaterialExpressionTextureCoordinates>(nullptr, NAME_None); break;
		case Op::PrimitiveCustomData: {
			auto* C = NewObject<DMaterialExpressionPrimitiveCustomData>(nullptr, NAME_None);
			C->DataIndex = std::get<MIR::FCustomDataRead>(N.Payload).Index; E = C; break;
		}
		case Op::Swizzle: {
			auto* C = NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None);
			const auto S = N.GetSwizzle(); C->Components.assign(S.Components.begin(), S.Components.begin() + S.Length); E = C; break;
		}
		case Op::TransformDirection: {
			auto* C = NewObject<DMaterialExpressionTransformDirection>(nullptr, NAME_None);
			const auto P = std::get<FMaterialTransformPayload>(N.Payload); C->Source = P.Source; C->Destination = P.Destination; E = C; break;
		}
		default: throw std::runtime_error("Unsupported Water expression.");
		}
		if (auto* Width = E->GetClass()->FindPropertyByName("ResultType"))
			*static_cast<Type*>(Width->GetValuePtr(E)) = N.ResultType;
		uint32 Slot = 0;
		E->GetClass()->ForEachProperty([&](FProperty* P) {
			if (P->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
				&& static_cast<FStructProperty*>(P)->GetStruct() == FMaterialNumericInput::StaticStruct())
			{
				if (Slot < N.Inputs.size())
				{
					auto Source = N.Inputs[Slot];
					if (N.ScalarBroadcastMask & (1u << Slot)) Source = Recipe.IR.Nodes.at(Source).Inputs.at(0);
					*static_cast<FMaterialNumericInput*>(P->GetValuePtr(E)) = Links.at(Source);
				}
				++Slot;
			}
		});
		auto Link = Own(E);
		if (N.Opcode == Op::Constant && N.SpatialKind != EMaterialSpatialKind::None)
		{
			auto* C = NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None);
			C->Input = Link; C->Components = {0, 1, 2}; C->OutputSpatialKind = N.SpatialKind; C->OutputCoordinateSpace = N.CoordinateSpace;
			Link = Own(C);
		}
		Links.push_back(Link);
	}
	FMaterialExpressionSurfaceOutputs Outputs;
	std::array<FMaterialNumericInput*, 8> Pins{&Outputs.BaseColor, &Outputs.Normal, &Outputs.Metallic, &Outputs.Roughness,
		&Outputs.AmbientOcclusion, &Outputs.Emissive, &Outputs.Opacity, &Outputs.OpacityMask};
	for (uint32 I = 0; I < Pins.size(); ++I)
	{
		const auto& Root = Recipe.IR.SurfaceRoot.Inputs[I];
		if (Root.bExpression) *Pins[I] = Links.at(Root.ExpressionIndex);
		else {
			const std::array Values{Root.Literal.X, Root.Literal.Y, Root.Literal.Z, Root.Literal.W};
			Pins[I]->SetConstant(std::vector<float>(Values.begin(), Values.begin() + uint32(Root.Type) + 1));
		}
	}
	Outputs.WorldPositionOffset = Links.at(Recipe.IR.WorldPositionOffset.ExpressionIndex);
	if (auto R = Material->SetStaticProperties(Recipe.StaticProperties); !R) throw std::runtime_error(R.error());
	if (auto R = Material->SetMaterialExpressions(Nodes, Outputs); !R)
		throw std::runtime_error(R.Diagnostics.empty() ? "Water source material graph validation failed." : R.Diagnostics.front().Message);
	FByteBuffer Bytes;
	if (!SerializeAssetPackageBytes(Package, Bytes) || !FFileHelper::SaveArrayToFileAtomically(Bytes, Destination))
		throw std::runtime_error("Water source material save failed.");
}
#endif
