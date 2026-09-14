#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Materials/MaterialProgramTypes.h"

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
		bool bDecodeNormal = false;
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
		FMaterialProgram Program;
		FMaterialGraphPresentation Presentation;
		// Logical roles may share an owner. Consumers publish one override per ID.
		std::vector<FImportedSurfaceOwner> Owners;
	};
	ASSETFORGEBUILTINS_API auto MakeImportedSurfaceRecipe(
		const std::array<FImportedSurfaceRole, 8>& Roles) -> FImportedSurfaceRecipe;
}
