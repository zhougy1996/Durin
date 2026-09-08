#pragma once

#include "EngineAPI.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "Threading/Task.h"
#include <any>
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
		auto IsOpen() const -> bool { return !Cancellation.IsCancellationRequested(); }
		auto GetCancellationToken() const -> FTaskCancellationToken { return Cancellation.GetToken(); }
	private:
		FTaskCancellationSource Cancellation;
		std::shared_ptr<void> ProviderLease;
		std::shared_ptr<void> RuntimeLease;
		friend class FSubsystemCollection;
	};

	// Shared transient service contract; scope adapters coordinate host destruction.
	DCLASS(Abstract, NoClassDefaultObject)
	class DSubsystem : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSubsystem(const FObjectInitializer& Initializer);
		// Cleanup is paired even with failed Initialize and must not throw.
		virtual auto Initialize() -> FSubsystemResult { return {}; }
		virtual auto Deinitialize() noexcept -> void {}
		auto GetWorkGate() const -> std::shared_ptr<const FSubsystemWorkGate> { return WorkGate; }
	private:
		std::shared_ptr<FSubsystemWorkGate> WorkGate;
		// Keeps provider code resident through physical destruction, not just retirement.
		std::shared_ptr<void> ProviderLease;
		friend class FSubsystemCollection;
	};

	// Native identity and same-scope dependencies; callback policy belongs to adapters.
	struct FSubsystemDescriptor
	{
		DClass* Type = nullptr;
		FName Provider;
		std::vector<DClass*> Dependencies;
	};

	// Frozen native registration, with optional opaque policy interpreted only by its scope adapter.
	struct FSubsystemRegistrationSnapshot
	{
		FSubsystemDescriptor Descriptor;
		std::any Policy;
	};

	// Provider-owned publication token; mutation affects future host snapshots only.
	class FSubsystemRegistration
	{
	public:
		ENGINE_API FSubsystemRegistration(DClass* Scope, FSubsystemDescriptor Descriptor, std::any Policy = {});
		ENGINE_API ~FSubsystemRegistration();
		FSubsystemRegistration(const FSubsystemRegistration&) = delete;
		auto operator=(const FSubsystemRegistration&) -> FSubsystemRegistration& = delete;
		ENGINE_API static auto Snapshot(DClass* Scope) -> std::vector<FSubsystemRegistrationSnapshot>;
	private:
		uint64 Identity;
	};

	// Owns deterministic construction and retirement; hosts retain objects through enumeration.
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
		ENGINE_API auto Shutdown() -> void;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void;
		auto IsDispatching() const -> bool { return bInitializing; }
	protected:
		struct FEntry
		{
			FSubsystemDescriptor Descriptor;
			std::shared_ptr<void> Lease;
			TObjectPtr<DSubsystem> Object;
			bool bInitialized = false;
		};
		DObject& Host;
		DClass* Scope;
		ESubsystemState State = ESubsystemState::Uninitialized;
		std::vector<FEntry> Entries;
	private:
		bool bClosed = false;
		bool bInitializing = false;
	};
}
