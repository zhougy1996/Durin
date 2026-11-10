#include "Editor/EditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	DEditorSubsystem::DEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorSubsystem::GetEditor() const -> DEditorEngine* { return Cast<DEditorEngine>(GetOuter()); }
	FEditorSubsystemCollection::FEditorSubsystemCollection(DEditorEngine& Host) : FDynamicSubsystemCollection(Host, DEditorSubsystem::StaticClass()) {}
	FEditorSubsystemCollection::~FEditorSubsystemCollection() = default;
	auto FEditorSubsystemCollection::Initialize() -> void
	{ FSubsystemCollection::Initialize(FSubsystemRegistration::GetRegistrations(DEditorSubsystem::StaticClass())); }
	auto FEditorSubsystemCollection::AddRegisteredSubsystems() -> void
	{ AddAndInitializeSubsystems(FSubsystemRegistration::GetRegistrations(DEditorSubsystem::StaticClass())); }
	auto FEditorSubsystemCollection::AddSubsystem(DClass* Type) -> DEditorSubsystem*
	{
		auto Registered = FSubsystemRegistration::GetRegistrations(DEditorSubsystem::StaticClass());
		const auto Found = std::ranges::find(Registered, Type, &FSubsystemDescriptor::Type);
		if (Found == Registered.end()) return nullptr;
		return static_cast<DEditorSubsystem*>(AddAndInitializeSubsystem(*Found));
	}
	auto FEditorSubsystemCollection::RemoveSubsystem(DClass* Type) -> void
	{ RemoveAndDeinitializeSubsystems(std::span<DClass* const>(&Type, 1)); }
	auto FEditorSubsystemCollection::CanUpdateActiveEditor() -> bool
	{
		if (IsGarbageCollecting() || FModularFeatureRegistry::Get().IsDispatching()) return false;
		if (!GEditor) return true;
		const auto& Collection = GEditor->EditorSubsystems;
		if (Collection.IsPopulating() || Collection.GetState() == ESubsystemState::Initializing || Collection.GetState() == ESubsystemState::ShuttingDown) return false;
		return Collection.GetState() != ESubsystemState::Ready || GEditor->CanUpdateEditorSubsystems();
	}
	auto FEditorSubsystemCollection::RemoveAllInstances(FName Provider) -> void
	{
		requiref(!Provider.IsNone() && CanUpdateActiveEditor(), "Editor provider removal requires a host safe point.");
		if (!GEditor || GEditor->GetEditorSubsystemState() != ESubsystemState::Ready) return;
		std::vector<DClass*> Types;
		for (const auto& Entry : FSubsystemRegistration::GetRegistrations(DEditorSubsystem::StaticClass()))
			if (Entry.Provider == Provider) Types.push_back(Entry.Type);
		GEditor->DispatchSubsystemCallback([&] {
			GEditor->EditorSubsystems.RemoveAndDeinitializeSubsystems(Types);
		});
	}
}
