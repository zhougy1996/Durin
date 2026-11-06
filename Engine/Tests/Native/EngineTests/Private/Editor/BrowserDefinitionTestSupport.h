#pragma once

#include "CoreMinimal.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/Class.h"

namespace Durin
{
	class FBrowserTestDefinition : public DAssetDefinition
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FBrowserTestDefinition, DAssetDefinition, FBrowserTestDefinition::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FBrowserTestDefinition)
	public:
		explicit FBrowserTestDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetClassPath() const -> std::string override { return ClassPath; }
		auto GetAssetDisplayName() const -> std::string override { return DisplayName; }
		auto GetAssetIcon() const -> std::string override { return Icon; }
		auto GetAssetColor() const -> std::array<float, 4> override { return {0.1f, 0.2f, 0.3f, 1.0f}; }
		auto GetAssetCategories() const -> std::vector<std::vector<std::string>> override { return Categories; }
		std::string ClassPath = "Project::DCategoryFixture", DisplayName = "Project fixture", Icon = "project-icon";
		std::vector<std::vector<std::string>> Categories{{"Project", "Simulation", "Inputs"}, {"Tools"}};
		auto CanRegisterStatically() const -> bool override { return false; }
	};
}
