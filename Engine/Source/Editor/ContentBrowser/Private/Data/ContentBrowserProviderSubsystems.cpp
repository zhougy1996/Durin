#include "ContentBrowserProviderSubsystems.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserNativeDataSource.h"
#include "DObject/ObjectLifecycle.h"
namespace Durin
{
	DContentBrowserAssetProviderSubsystem::DContentBrowserAssetProviderSubsystem(const FObjectInitializer& I) : Super(I) {}
	DContentBrowserFileProviderSubsystem::DContentBrowserFileProviderSubsystem(const FObjectInitializer& I) : Super(I) {}
	auto DContentBrowserAssetProviderSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{
		auto* Data = Collection.InitializeDependency<DContentBrowserDataSubsystem>();
		if (!Data) return;
		Source = TStrongObjectPtr<DContentBrowserDataSource>(NewObject<DContentBrowserAssetDataSource>(Data, {}));
		Source->Initialize();
		Data->ActivateDataSource(FName("Asset"));
	}
	auto DContentBrowserFileProviderSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{
		auto* Data = Collection.InitializeDependency<DContentBrowserDataSubsystem>();
		if (!Data) return;
		Source = TStrongObjectPtr<DContentBrowserDataSource>(NewObject<DContentBrowserFileDataSource>(Data, {}));
		Source->Initialize();
		Data->ActivateDataSource(FName("File"));
	}
	auto DContentBrowserAssetProviderSubsystem::Deinitialize() noexcept -> void { if (Source) Source->Shutdown(); Source.Reset(); }
	auto DContentBrowserFileProviderSubsystem::Deinitialize() noexcept -> void { if (Source) Source->Shutdown(); Source.Reset(); }
}
