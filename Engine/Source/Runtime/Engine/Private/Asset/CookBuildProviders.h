#pragma once

#include "CoreMinimal.h"
#include "Asset/Cook.h"
namespace Durin::AssetPrivate
{
	// Families declare native recipe identity before cache lookup; the host pins providers.
	auto DeclareMaterialCookInputs(const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult;
	auto DeclareTexture2DCookInputs(const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult;
	auto DeclareTextureCubeCookInputs(const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult;
	auto DeclareVolumeTextureCookInputs(const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult;
	auto DeclareStaticMeshCookInputs(const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult;
}
