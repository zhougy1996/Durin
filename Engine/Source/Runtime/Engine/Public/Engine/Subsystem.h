#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "Subsystem.gen.h"

namespace Durin
{
	// Separates one-shot host service lifetime from repeatable play lifetimes.
	enum class ESubsystemState : uint8 { Uninitialized, Initializing, Ready, ShuttingDown, Shutdown };

	class FSubsystemCollection;

	// Shared transient service contract; hosts own initialization and cleanup.
	DCLASS(Abstract)
	class DSubsystem : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSubsystem(const FObjectInitializer& Initializer);
		// Creation-only CDO query; existing instances are not reevaluated during refresh.
		virtual auto ShouldCreateSubsystem(DObject* Outer) const -> bool { return true; }
		// Cleanup is paired even with failed Initialize and must not throw.
		virtual auto Initialize(FSubsystemCollection& Collection) -> void {}
		virtual auto Deinitialize() noexcept -> void {}
	};

	// Native identity and owning module; callback policy belongs to adapters.
	struct FSubsystemDescriptor
	{
		DClass* Type = nullptr;
		FName Provider;
	};

	// Provider-owned catalog publication; instance additions and removals are explicit.
	class FSubsystemRegistration
	{
	public:
		ENGINE_API FSubsystemRegistration(DClass* Scope, FSubsystemDescriptor Descriptor);
		ENGINE_API ~FSubsystemRegistration();
		FSubsystemRegistration(const FSubsystemRegistration&) = delete;
		auto operator=(const FSubsystemRegistration&) -> FSubsystemRegistration& = delete;
		ENGINE_API static auto GetRegistrations(DClass* Scope) -> std::vector<FSubsystemDescriptor>;
	private:
		uint64 Identity;
	};

	// Owns one-shot initialization and shutdown; hosts retain objects through enumeration.
	class FSubsystemCollection
	{
	public:
		ENGINE_API FSubsystemCollection(DObject& InHost, DClass* InScope);
		ENGINE_API virtual ~FSubsystemCollection();
		FSubsystemCollection(const FSubsystemCollection&) = delete;
		auto operator=(const FSubsystemCollection&) -> FSubsystemCollection& = delete;
		ENGINE_API auto Find(DClass* Type) const -> DSubsystem*;
		auto GetState() const -> ESubsystemState { return State; }
		// Initial population is one-shot; instances initialize their own dependencies.
		ENGINE_API auto Initialize(std::vector<FSubsystemDescriptor> Descriptors) -> void;
		ENGINE_API auto InitializeDependency(DClass* Type) -> DSubsystem*;
		template<typename T> requires std::is_base_of_v<DSubsystem, T>
		auto InitializeDependency() -> T* { return static_cast<T*>(InitializeDependency(T::StaticClass())); }
		ENGINE_API auto RequestDeinitialize() -> void;
		// Deinitializes and removes every subsystem; host shutdown remains the host's responsibility.
		ENGINE_API auto Deinitialize() -> void;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void;
		auto IsPopulating() const -> bool { return bPopulating; }
	protected:
		ENGINE_API auto AddAndInitializeSubsystems(std::vector<FSubsystemDescriptor> Descriptors) -> void;
		ENGINE_API auto AddAndInitializeSubsystem(const FSubsystemDescriptor& Descriptor) -> DSubsystem*;
		// Scope-specific configuration for one-shot initialization, after CDO selection.
		virtual auto ValidateSelectedDescriptor(const FSubsystemDescriptor&) const -> void {}
		struct FEntry
		{
			FSubsystemDescriptor Descriptor;
			TObjectPtr<DSubsystem> Object;
			bool bInitialized = false;
			bool bReferencesInvalidated = false;
		};
		DObject& Host;
		DClass* Scope;
		ESubsystemState State = ESubsystemState::Uninitialized;
		std::vector<FEntry> Entries;
	private:
		auto ValidateType(const FSubsystemDescriptor& Descriptor) const -> void;
		auto InvalidateEntryReferences(FEntry& Entry) -> void;
		// Callers invalidate references first, then remove entries in reverse dependency order.
		auto RemoveAndDeinitializeSubsystem(size_t Index) -> void;
		struct FInitialization
		{
			std::vector<FSubsystemDescriptor> Descriptors;
			std::vector<DClass*> Stack;
		};
		FInitialization* Initialization = nullptr;
		bool bClosed = false;
		bool bInvalidatingReferences = false;
		bool bDeinitializeRequested = false;
		bool bPopulating = false;
		// Dynamic membership reuses lifecycle primitives without exposing them to fixed scopes.
		friend class FDynamicSubsystemCollection;
	};
}
