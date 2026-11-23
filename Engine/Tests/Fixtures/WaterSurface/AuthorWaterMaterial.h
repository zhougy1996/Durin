#pragma once

#include "CoreMinimal.h"

#if DURIN_WITH_EDITOR
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Scan.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSingleLayerWaterOutput.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Misc/FileHelper.h"

// Explicit source-asset authoring utility. Cook only reads the saved graph.
inline auto AuthorWaterMaterial(const std::filesystem::path& Destination) -> void
{
	using namespace Durin;
	FSoftObjectPath WavePath;
	if (!FSoftObjectPath::TryCreate("/Plugins/Water/Materials/GerstnerWaves.GerstnerWaves", WavePath)
		|| !RefreshAssetRegistry()) throw std::runtime_error("Could not resolve the Water wave function.");
	auto* Waves = LoadObject<DMaterialFunction>(WavePath);
	if (!Waves) throw std::runtime_error("Could not load GerstnerWaves.");
	FPackagePath Path;
	if (!FPackagePath::TryCreate("/Plugins/Water/Materials/DefaultWater", Path)) throw std::runtime_error("Invalid material package path.");
	auto* Package = CreatePackage(Path);
	auto* Material = NewObject<DMaterial>(Package, "DefaultWater", EObjectFlags::Public);
	std::vector<TStrongObjectPtr<DMaterialExpression>> Owners;
	std::vector<DMaterialExpression*> Nodes;
	uint32 NextId = 1;
	auto Own = [&](DMaterialExpression* E) {
		E->Id = {0x57415452u, 0x4d41544cu, 0x00000001u, NextId++};
		Owners.emplace_back(E); Nodes.push_back(E);
		return FMaterialExpressionInput{E};
	};
	auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
	Call->SetMaterialFunction(Waves);
	Own(Call);
	Call->EditorLayout = {true, -400, 0};
	FMaterialRootInputs Outputs;
	Outputs.Get(EMaterialProperty::MP_BaseColor).SetConstant({.02f, .18f, .24f});
	Outputs.Get(EMaterialProperty::MP_Roughness).SetConstant({.22f});
	Outputs.Get(EMaterialProperty::MP_Opacity).SetConstant({0.f});
	constexpr FGuid WorldDisplacementPort{0x57415452u, 0x504f5254u, 1, 1};
	constexpr FGuid TangentNormalPort{0x57415452u, 0x504f5254u, 1, 2};
	Outputs.Get(EMaterialProperty::MP_WorldPositionOffset) = FMaterialExpressionInput{Call, 0, WorldDisplacementPort};
	Outputs.Get(EMaterialProperty::MP_Normal) = FMaterialExpressionInput{Call, 0, TangentNormalPort};

	auto* Water = NewObject<DMaterialExpressionSingleLayerWaterOutput>(nullptr, NAME_None);
	const auto Color = [&](FVector3 Value, int32 Y) {
		auto* Constant = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None);
		Constant->Value = Value;
		Constant->EditorLayout = {true, -700, Y};
		return Own(Constant);
	};
	Water->ScatteringCoefficients = Color({.00035f, .00065f, .0009f}, 450);
	Water->AbsorptionCoefficients = Color({.004f, .0012f, .00065f}, 600);
	auto* Phase = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None);
	Phase->Value = .2f;
	Phase->EditorLayout = {true, -700, 750};
	Water->PhaseG = Own(Phase);
	Water->ColorScaleBehindWater = Color({1.f, 1.f, 1.f}, 900);
	Water->EditorLayout = {true, -360, 460};
	Own(Water);
	FMaterialStaticProperties Properties;
	Properties.bTwoSided = true;
	Properties.ShadingModel = EMaterialShadingModel::SingleLayerWater;
	Properties.RefractionMethod = EMaterialRefractionMethod::IndexOfRefractionFromF0;
	if (auto R = Material->SetStaticProperties(Properties); !R) throw std::runtime_error(R.error());
	if (auto R = Material->SetMaterialExpressions(Nodes, Outputs); !R)
		throw std::runtime_error(R.Diagnostics.empty() ? "Water source material graph validation failed." : R.Diagnostics.front().Message);
	FByteBuffer Bytes;
	if (!SerializeAssetPackageBytes(Package, Bytes) || !FFileHelper::SaveArrayToFileAtomically(Bytes, Destination))
		throw std::runtime_error("Water source material save failed.");
}
#endif
