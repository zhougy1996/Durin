#include "Editor/EditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"

namespace Durin
{
	DEditorSubsystem::DEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DEditorSubsystem::BeginDestroy() -> void
	{
		if (auto* Host = GetEditor()) Host->PrepareForShutdown();
		Super::BeginDestroy();
	}
	auto DEditorSubsystem::IsReadyForFinishDestroy() -> bool
	{
		auto* Host = GetEditor();
		return !Host || Host->IsReadyForFinishDestroy();
	}
	auto DEditorSubsystem::GetEditor() const -> DEditorEngine* { return Cast<DEditorEngine>(GetOuter()); }
	FEditorSubsystemCollection::FEditorSubsystemCollection(DEditorEngine& Host) : FSubsystemCollection(Host, DEditorSubsystem::StaticClass()) {}
	auto FEditorSubsystemCollection::Initialize() -> FSubsystemResult
	{
		std::vector<FSubsystemDescriptor> Selected;
		for (const auto& Entry : FSubsystemRegistration::Snapshot(DEditorSubsystem::StaticClass())) Selected.push_back(Entry.Descriptor);
		return FSubsystemCollection::Initialize(std::move(Selected));
	}
}
