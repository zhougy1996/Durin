#include "ContentBrowserFileProviderSubsystem.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserFileDataSource.h"
#include "DObject/ObjectLifecycle.h"
namespace Durin
{
	DContentBrowserFileProviderSubsystem::DContentBrowserFileProviderSubsystem(const FObjectInitializer& I) : Super(I) {}
	auto DContentBrowserFileProviderSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{
		auto* Data = Collection.InitializeDependency<DContentBrowserDataSubsystem>();
		if (!Data) return;
		Source = TStrongObjectPtr<DContentBrowserDataSource>(NewObject<DContentBrowserFileDataSource>(Data, {}));
		Source->Initialize();
		Data->ActivateDataSource(FName("File"));
	}
	auto DContentBrowserFileProviderSubsystem::Deinitialize() noexcept -> void { if (Source) Source->Shutdown(); Source.Reset(); }
}
