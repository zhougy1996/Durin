#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "DObject/StrongObjectPtr.h"
#include "Materials/MaterialFunction.h"

namespace Durin::AssetForge::Builtins
{
	// Values and slot numbers are persistent identities. Never derive them from labels.
	enum class EStandardMaterialFunction : uint32
	{
		UVTransform = 1, SampleNormal = 2, SampleORM = 3, StandardPBR = 4, StandardPBR_ORM = 5,
		ImportedSurfaceValues = 6,
	};
	constexpr auto StandardMaterialPortId(EStandardMaterialFunction Function, uint32 Slot) -> FGuid
	{
		return {0x78e431b9, 0x4afe4982, static_cast<uint32>(Function), Slot};
	}
	struct FStandardMaterialFunctions
	{
		TObjectPtr<DMaterialFunction> UVTransform, SampleNormal, SampleORM, StandardPBR, StandardPBR_ORM;
		TObjectPtr<DMaterialFunction> ImportedSurfaceValues;
	};
	// Explicit shipped-asset interface contract. Creates no expression objects and
	// is shared by authoring recipes and asset admission.
	ASSETFORGEBUILTINS_API auto GetStandardMaterialFunctionInterface(EStandardMaterialFunction Function)
		-> FMaterialFunctionSignature;
	// Owning-thread recipe; keeps concrete children alive until validated publication.
	struct FStandardMaterialFunctionExpressions
	{
		ASSETFORGEBUILTINS_API auto GetSignature() const -> FMaterialFunctionSignature;
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		ASSETFORGEBUILTINS_API auto Apply(DMaterialFunction& Function) const -> FMaterialProgramValidationResult;
		ASSETFORGEBUILTINS_API auto Matches(const DMaterialFunction& Function) const -> bool;
	};
	ASSETFORGEBUILTINS_API auto MakeStandardMaterialFunctionExpressions(
		EStandardMaterialFunction Function, const FStandardMaterialFunctions& Dependencies)
		-> FStandardMaterialFunctionExpressions;
	// Loads the shipped Engine assets without creating or saving packages.
	// Existing implementations are preserved; missing assets or incompatible interfaces fail.
	ASSETFORGEBUILTINS_API auto LoadStandardMaterialFunctions(
		FStandardMaterialFunctions& OutFunctions, std::string& OutError) -> bool;
}
