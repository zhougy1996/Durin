#pragma once

#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/Builtins/MaterialExpressionRecipe.h"

namespace Durin::AssetForge::Builtins
{
	// ResourceIdentity includes derivation and color-space identity. It is used only
	// to discover equal fetches, never serialized into a structural key.
	struct FImportedSurfaceSample
	{
		std::string ResourceIdentity;
		ETextureUsage Usage = ETextureUsage::Color;
		FMaterialSamplerState Sampler;
		FMaterialProgramLiteral UVChannel;
		FMaterialProgramLiteral UVScale{1, 1};
		FMaterialProgramLiteral UVOffset;
		FMaterialProgramLiteral UVRotation;
		uint8 OutputIndex = 1;
	};
	struct FImportedSurfaceRole
	{
		FMaterialProgramLiteral Value;
		std::optional<FImportedSurfaceSample> Sample;
	};
	struct FImportedSurfaceOwner
	{
		EMaterialSurfaceOutput Role;
		MaterialParameters::EMaterialBuiltinParameterKind Kind;
		FGuid ParameterId;
	};
	struct FImportedSurfaceRecipe
	{
		std::string CanonicalKey;
		FMaterialExpressionRecipe Graph;
		// Logical roles may share an owner. Consumers publish one override per ID.
		std::vector<FImportedSurfaceOwner> Owners;
	};
	ASSETFORGEBUILTINS_API auto MakeImportedSurfaceRecipe(
		const std::array<FImportedSurfaceRole, 8>& Roles) -> FImportedSurfaceRecipe;
}
