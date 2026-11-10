#include "Editor/EditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"

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
		if (Found == Registered.end())
		{
			// Even an empty addition validates the collection's mutation boundary.
			AddAndInitializeSubsystems({});
			return nullptr;
		}
		AddAndInitializeSubsystems({*Found});
		return static_cast<DEditorSubsystem*>(Find(Type));
	}
	auto FEditorSubsystemCollection::RemoveSubsystem(DClass* Type) -> void
	{ RemoveAndDeinitializeSubsystems(std::span<DClass* const>(&Type, 1)); }
	auto FEditorSubsystemCollection::RemoveSubsystems(FName Provider) -> void
	{
		require(!Provider.IsNone());
		std::vector<DClass*> Types;
		for (const auto& Entry : Entries)
			if (Entry.Descriptor.Provider == Provider) Types.push_back(Entry.Descriptor.Type);
		RemoveAndDeinitializeSubsystems(Types);
	}
}
