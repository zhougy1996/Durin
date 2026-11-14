#include "ContentBrowserAssetProviderSubsystem.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserAssetDataSource.h"
#include "DObject/ObjectLifecycle.h"
namespace Durin
{
	DContentBrowserAssetProviderSubsystem::DContentBrowserAssetProviderSubsystem(const FObjectInitializer& I) : Super(I) {}
	auto DContentBrowserAssetProviderSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{
		auto* Data = Collection.InitializeDependency<DContentBrowserDataSubsystem>();
		if (!Data) return;
		Source = TStrongObjectPtr<DContentBrowserDataSource>(NewObject<DContentBrowserAssetDataSource>(Data, {}));
		Source->Initialize();
		Data->ActivateDataSource(FName("Asset"));
	}
	auto DContentBrowserAssetProviderSubsystem::Deinitialize() noexcept -> void { if (Source) Source->Shutdown(); Source.Reset(); }
}
