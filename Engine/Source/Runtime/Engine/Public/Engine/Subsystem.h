#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"

#include "EngineAPI.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "Threading/TaskRuntime.h"
#include "Subsystem.gen.h"

namespace Durin
{
	// Separates one-shot host service lifetime from repeatable play lifetimes.
	enum class ESubsystemState : uint8 { Uninitialized, Initializing, Ready, ShuttingDown, Shutdown, Failed };
	enum class ESubsystemError : uint8 { None, InvalidState, InvalidDescriptor, DuplicateType, MissingDependency, DependencyCycle, ProviderUnavailable, InitializationFailed, Aborted };
	struct FSubsystemResult
	{
		ESubsystemError Error = ESubsystemError::None;
		std::string Message;
		explicit operator bool() const { return Error == ESubsystemError::None; }
	};

	// A closed gate survives the service; detached completions must test it on the game thread.
	class FSubsystemWorkGate
	{
	public:
		auto IsOpen() const -> bool { return !Cancellation.IsCanceled(); }
		auto GetCancellationToken() const -> const Tasks::FCancellationToken& { return Cancellation; }
	private:
		Tasks::FCancellationToken Cancellation;
		bool bDeinitialized = false;
		friend class FSubsystemCollection;
		friend class DSubsystem;
	};

	// Shared transient service contract; scope adapters coordinate host destruction.
	DCLASS(Abstract)
	class DSubsystem : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSubsystem(const FObjectInitializer& Initializer);
		// Creation-only CDO query; existing instances are not reevaluated during refresh.
		virtual auto ShouldCreateSubsystem(DObject* Outer) const -> bool { return true; }
		// Cleanup is paired even with failed Initialize and must not throw.
		virtual auto Initialize() -> FSubsystemResult { return {}; }
		virtual auto Deinitialize() noexcept -> void {}
		auto GetWorkGate() const -> std::shared_ptr<const FSubsystemWorkGate> { return WorkGate; }
	protected:
		auto HasCompletedDeinitialization() const -> bool { return WorkGate && WorkGate->bDeinitialized; }
	private:
		std::shared_ptr<FSubsystemWorkGate> WorkGate;
		friend class FSubsystemCollection;
	};

	// Native identity and same-scope dependencies; callback policy belongs to adapters.
	struct FSubsystemDescriptor
	{
		DClass* Type = nullptr;
		FName Provider;
		std::vector<DClass*> Dependencies;
		// Assigned by a publication token; distinguishes removal/replacement of the same type.
		uint64 RegistrationIdentity = 0;
	};

	// Provider-owned publication token; dynamic scope adapters reconcile changes at safe points.
	class FSubsystemRegistration
	{
	public:
		ENGINE_API FSubsystemRegistration(DClass* Scope, FSubsystemDescriptor Descriptor);
		ENGINE_API ~FSubsystemRegistration();
		FSubsystemRegistration(const FSubsystemRegistration&) = delete;
		auto operator=(const FSubsystemRegistration&) -> FSubsystemRegistration& = delete;
		ENGINE_API static auto Snapshot(DClass* Scope) -> std::vector<FSubsystemDescriptor>;
		ENGINE_API static auto GetRevision() -> uint64;
	private:
		uint64 Identity;
	};

	// Owns deterministic initialization and removal; hosts retain objects through enumeration.
	class FSubsystemCollection
	{
	public:
		ENGINE_API FSubsystemCollection(DObject& InHost, DClass* InScope);
		ENGINE_API virtual ~FSubsystemCollection();
		FSubsystemCollection(const FSubsystemCollection&) = delete;
		auto operator=(const FSubsystemCollection&) -> FSubsystemCollection& = delete;
		ENGINE_API auto Find(DClass* Type) const -> DSubsystem*;
		auto GetState() const -> ESubsystemState { return State; }
		// Membership is frozen before any user factory or Initialize callback runs.
		ENGINE_API auto Initialize(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult;
		ENGINE_API auto CloseWork() -> void;
		// Deinitializes and removes every subsystem; host shutdown remains the host's responsibility.
		ENGINE_API auto Deinitialize() -> void;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void;
		auto IsPopulating() const -> bool { return bPopulating; }
	protected:
		// Scope-specific configuration for one-shot initialization, after CDO selection.
		virtual auto ValidateSelectedDescriptor(const FSubsystemDescriptor&) const -> FSubsystemResult { return {}; }
		// Explicit opt-in for dynamic scope adapters; Engine/World retain frozen membership.
		ENGINE_API auto Reconcile(std::vector<FSubsystemDescriptor> Descriptors) -> FSubsystemResult;
		struct FEntry
		{
			FSubsystemDescriptor Descriptor;
			TObjectPtr<DSubsystem> Object;
			bool bInitialized = false;
		};
		DObject& Host;
		DClass* Scope;
		ESubsystemState State = ESubsystemState::Uninitialized;
		std::vector<FEntry> Entries;
	private:
		auto ValidateType(const FSubsystemDescriptor& Descriptor) const -> FSubsystemResult;
		// Input types have passed scope/concreteness and CDO eligibility selection.
		// Returns the first diagnostic and the valid dependency-closed subset in construction order.
		// Initialize rejects diagnostics; Reconcile can retain unrelated services.
		auto OrderSelectedDescriptors(std::vector<FSubsystemDescriptor> Descriptors,
			std::vector<FSubsystemDescriptor>& Ordered) const -> FSubsystemResult;
		// Ordered descriptors are already selected and validated; failed objects remain owned for rollback.
		auto AddAndInitializeSubsystem(const FSubsystemDescriptor& Descriptor) -> FSubsystemResult;
		auto CloseEntryWork(FEntry& Entry) -> void;
		// Callers close all selected entries first, then remove them in reverse dependency order.
		auto RemoveAndDeinitializeSubsystem(size_t Index) -> void;
		bool bClosed = false;
		bool bClosingWork = false;
		bool bDeinitializeRequested = false;
		bool bPopulating = false;
	};
}
