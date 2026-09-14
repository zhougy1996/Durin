#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Materials/MaterialFunction.h"

namespace Durin::AssetForge::Builtins
{
	// Values and slot numbers are persistent identities. Never derive them from labels.
	enum class EStandardMaterialFunction : uint32
	{
		UVTransform = 1, SampleNormal = 2, SampleORM = 3, StandardPBR = 4, StandardPBR_ORM = 5,
		ImportedSurfaceValues = 6, DecodeImportedNormalRG = 7,
	};
	inline constexpr uint32 StandardMaterialFunctionVersion = 2;
	constexpr auto StandardMaterialPortId(EStandardMaterialFunction Function, uint32 Slot) -> FGuid
	{
		return {0x78e431b9, 0x4afe4982, static_cast<uint32>(Function), Slot};
	}
	struct FStandardMaterialFunctions
	{
		TObjectPtr<DMaterialFunction> UVTransform, SampleNormal, SampleORM, StandardPBR, StandardPBR_ORM;
		TObjectPtr<DMaterialFunction> ImportedSurfaceValues, DecodeImportedNormalRG;
	};
	ASSETFORGEBUILTINS_API auto MakeStandardMaterialFunctionGraph(
		EStandardMaterialFunction Function, const FStandardMaterialFunctions& Dependencies)
		-> FMaterialFunctionGraph;
	// Saves dependencies first. Existing implementations are preserved; incompatible interfaces fail.
	ASSETFORGEBUILTINS_API auto EnsureStandardMaterialFunctions(
		FStandardMaterialFunctions& OutFunctions, std::string& OutError) -> bool;
}
