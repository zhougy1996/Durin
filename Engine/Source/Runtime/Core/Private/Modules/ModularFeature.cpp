#include "Modules/ModularFeature.h"

#include "Modules/ModuleOwnerState.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace Durin
{
	namespace Detail
	{
		enum class EEntryState : uint8
		{
			Published,
			Retiring,
			Retired,
		};

		struct FModularFeatureEntryState
		{
			uint64 Identity = 0;
			std::shared_ptr<FModuleOwnerState> Owner;
			FModularFeatureIdentity FeatureIdentity;
			IModularFeature* Implementation = nullptr;
			EEntryState State = EEntryState::Published;
			uint32 InFlightCount = 0;
			uint32 RetainedResourceCount = 0;
		};
	}

	namespace
	{
		struct FRegistryState
		{
			// Lock order: the module-map lock may precede this mutex. This mutex is
			// never held while entering module or feature callbacks.
			std::mutex Mutex;
			std::condition_variable Quiescence;
			std::vector<std::shared_ptr<Detail::FModularFeatureEntryState>> Entries;
			uint64 NextEntryIdentity = 1;
		};

		auto GetRegistryState() -> FRegistryState&
		{
			static FRegistryState State;
			return State;
		}

		thread_local std::vector<const Detail::FModuleOwnerState*> GActiveModuleOwners;

		auto IsOwnerActiveOnThisThread(const Detail::FModuleOwnerState* Owner) -> bool
		{
			return std::ranges::find(GActiveModuleOwners, Owner) != GActiveModuleOwners.end();
		}

		auto MakeSnapshotLocked(
			const FRegistryState& State,
			const std::shared_ptr<Detail::FModuleOwnerState>& Owner
		) -> FModularFeatureRetirementSnapshot
		{
			FModularFeatureRetirementSnapshot Snapshot;
			if (!Owner) return Snapshot;
			Snapshot.OwnerName = Owner->Name;
			Snapshot.OwnerGeneration = Owner->Generation;
			for (const auto& Entry : State.Entries)
			{
				if (Entry->Owner != Owner) continue;
				++Snapshot.RegistrationCount;
				Snapshot.InFlightInvocationCount += Entry->InFlightCount;
				Snapshot.RetainedResourceCount += Entry->RetainedResourceCount;
				if (Entry->State == Detail::EEntryState::Published) ++Snapshot.PublishedCount;
				if (Entry->State == Detail::EEntryState::Retiring) ++Snapshot.RetiringCount;
			}
			return Snapshot;
		}

		auto MakeEntrySnapshotLocked(
			const FRegistryState& State,
			const std::shared_ptr<Detail::FModularFeatureEntryState>& Selected
		) -> FModularFeatureRetirementSnapshot
		{
			FModularFeatureRetirementSnapshot Snapshot;
			if (!Selected || !Selected->Owner) return Snapshot;
			Snapshot.OwnerName = Selected->Owner->Name;
			Snapshot.OwnerGeneration = Selected->Owner->Generation;
			const auto Entry = std::ranges::find(State.Entries, Selected);
			if (Entry == State.Entries.end()) return Snapshot;
			Snapshot.RegistrationCount = 1;
			Snapshot.InFlightInvocationCount = Selected->InFlightCount;
			Snapshot.RetainedResourceCount = Selected->RetainedResourceCount;
			Snapshot.PublishedCount = Selected->State == Detail::EEntryState::Published ? 1u : 0u;
			Snapshot.RetiringCount = Selected->State == Detail::EEntryState::Retiring ? 1u : 0u;
			return Snapshot;
		}

		auto RetireCompletedEntriesLocked(FRegistryState& State) -> void
		{
			for (const auto& Entry : State.Entries)
			{
				if (Entry->State == Detail::EEntryState::Retiring
					&& Entry->InFlightCount == 0 && Entry->RetainedResourceCount == 0)
				{
					Entry->State = Detail::EEntryState::Retired;
					Entry->Implementation = nullptr;
				}
			}
			std::erase_if(State.Entries, [](const auto& Entry) {
				return Entry->State == Detail::EEntryState::Retired;
			});
		}
	}

	Detail::FModularFeatureInvocation::FModularFeatureInvocation(
		std::shared_ptr<FModularFeatureEntryState> InEntry)
		: Entry(std::move(InEntry))
	{
	}

	Detail::FModularFeatureInvocation::FModularFeatureInvocation(FModularFeatureInvocation&& Other) noexcept
		: Entry(std::move(Other.Entry))
		, bEntered(std::exchange(Other.bEntered, false))
	{
	}

	auto Detail::FModularFeatureInvocation::operator=(FModularFeatureInvocation&& Other) noexcept -> FModularFeatureInvocation&
	{
		if (this == &Other) return *this;
		Leave();
		Entry = std::move(Other.Entry);
		bEntered = std::exchange(Other.bEntered, false);
		return *this;
	}

	Detail::FModularFeatureInvocation::~FModularFeatureInvocation()
	{
		Leave();
		if (!Entry) return;
		auto& State = GetRegistryState();
		std::lock_guard Lock(State.Mutex);
		check(Entry->InFlightCount > 0);
		--Entry->InFlightCount;
		RetireCompletedEntriesLocked(State);
		State.Quiescence.notify_all();
	}

	auto Detail::FModularFeatureInvocation::GetImplementation() const -> IModularFeature*
	{
		return Entry ? Entry->Implementation : nullptr;
	}

	auto Detail::FModularFeatureInvocation::Enter() -> void
	{
		if (!Entry || bEntered) return;
		GActiveModuleOwners.push_back(Entry->Owner.get());
		bEntered = true;
	}

	auto Detail::FModularFeatureInvocation::Leave() -> void
	{
		if (!bEntered) return;
		check(!GActiveModuleOwners.empty());
		check(GActiveModuleOwners.back() == Entry->Owner.get());
		GActiveModuleOwners.pop_back();
		bEntered = false;
	}

	FModuleOwnedResourceLease::~FModuleOwnedResourceLease() { Release(); }

	FModuleOwnedResourceLease::FModuleOwnedResourceLease(
		FModuleOwnedResourceLease&& Other) noexcept
		: Entry(std::move(Other.Entry))
	{
	}

	auto FModuleOwnedResourceLease::operator=(FModuleOwnedResourceLease&& Other) noexcept
		-> FModuleOwnedResourceLease&
	{
		if (this == &Other) return *this;
		Release();
		Entry = std::move(Other.Entry);
		return *this;
	}

	auto FModuleOwnedResourceLease::Release() -> void
	{
		if (!Entry) return;
		auto& State = GetRegistryState();
		{
			std::lock_guard Lock(State.Mutex);
			check(Entry->RetainedResourceCount > 0);
			--Entry->RetainedResourceCount;
			RetireCompletedEntriesLocked(State);
			State.Quiescence.notify_all();
		}
		Entry.reset();
	}

	auto FModuleOwnedCallbackGate::IsValid() const -> bool
	{
		return Entry != nullptr;
	}

	auto FModuleOwnedCallbackGate::TryEnter() const -> FModuleOwnedCallbackInvocation
	{
		auto Invocation = FModularFeatureRegistry::Get().BeginInvokeEntry(Entry);
		return Invocation.IsValid()
			? FModuleOwnedCallbackInvocation(std::move(Invocation))
			: FModuleOwnedCallbackInvocation{};
	}

	auto FModuleOwnedCallbackGate::RetainResource() const -> FModuleOwnedResourceLease
	{
		return FModularFeatureRegistry::Get().RetainEntryResource(Entry);
	}

	FModuleOwnedCallbackRegistration::~FModuleOwnedCallbackRegistration() { Retire(); }

	FModuleOwnedCallbackRegistration::FModuleOwnedCallbackRegistration(
		FModuleOwnedCallbackRegistration&& Other) noexcept
		: Entry(std::move(Other.Entry))
	{
	}

	auto FModuleOwnedCallbackRegistration::operator=(
		FModuleOwnedCallbackRegistration&& Other) noexcept
		-> FModuleOwnedCallbackRegistration&
	{
		if (this == &Other) return *this;
		Retire();
		Entry = std::move(Other.Entry);
		return *this;
	}

	auto FModuleOwnedCallbackRegistration::IsValid() const -> bool
	{
		return Entry != nullptr;
	}

	auto FModuleOwnedCallbackRegistration::Retire() -> FModularFeatureRetirementSnapshot
	{
		if (!Entry) return {};
		return FModularFeatureRegistry::Get().RetireEntry(Entry);
	}

	auto FModuleOwnedCallbackRegistration::Reset(std::chrono::milliseconds Timeout)
		-> FModularFeatureRetirementResult
	{
		if (!Entry)
			return {EModularFeatureRetirementStatus::InvalidRegistration, {},
				"The callback registration token is empty or moved-from."};
		Retire();
		auto Result = FModularFeatureRegistry::Get().WaitEntry(Entry, Timeout);
		if (Result.Succeeded()) Entry.reset();
		return Result;
	}

	FModularFeatureRegistration::FModularFeatureRegistration(std::shared_ptr<Detail::FModularFeatureEntryState> InEntry)
		: Entry(std::move(InEntry))
	{
	}

	FModularFeatureRegistration::FModularFeatureRegistration(FModularFeatureRegistration&& Other) noexcept
		: Entry(std::move(Other.Entry))
	{
	}

	auto FModularFeatureRegistration::operator=(FModularFeatureRegistration&& Other) noexcept -> FModularFeatureRegistration&
	{
		if (this == &Other) return *this;
		Retire();
		Entry = std::move(Other.Entry);
		return *this;
	}

	FModularFeatureRegistration::~FModularFeatureRegistration()
	{
		Retire();
	}

	auto FModularFeatureRegistration::IsValid() const -> bool
	{
		return Entry != nullptr;
	}

	auto FModularFeatureRegistration::Retire() -> FModularFeatureRetirementSnapshot
	{
		if (!Entry) return {};
		return FModularFeatureRegistry::Get().RetireEntry(Entry);
	}

	auto FModularFeatureRegistration::Reset(std::chrono::milliseconds Timeout) -> FModularFeatureRetirementResult
	{
		if (!Entry)
		{
			return {EModularFeatureRetirementStatus::InvalidRegistration, {}, "The registration token is empty or moved-from."};
		}
		Retire();
		auto Result = FModularFeatureRegistry::Get().WaitEntry(Entry, Timeout);
		if (Result.Succeeded()) Entry.reset();
		return Result;
	}

	auto FModularFeatureRegistry::Get() -> FModularFeatureRegistry&
	{
		// The state must outlive module-manager teardown because fail-closed module
		// instances can release their registration handles during static destruction.
		(void)GetRegistryState();
		static FModularFeatureRegistry Registry;
		return Registry;
	}

	auto FModularFeatureRegistry::CreateOwner(FName OwnerName, uint64 Generation)
		-> std::shared_ptr<Detail::FModuleOwnerState>
	{
		auto Owner = std::make_shared<Detail::FModuleOwnerState>();
		Owner->Name = OwnerName;
		Owner->Generation = Generation;
		return Owner;
	}

	auto FModularFeatureRegistry::Register(
		const std::shared_ptr<Detail::FModuleOwnerState>& Owner,
		FModularFeatureIdentity Identity,
		IModularFeature& Implementation
	) -> FModularFeatureRegistration
	{
		if (!Owner || Identity.Name.IsNone() || Identity.Version == 0) return {};
		auto Entry = std::make_shared<Detail::FModularFeatureEntryState>();
		Entry->Owner = Owner;
		Entry->FeatureIdentity = std::move(Identity);
		Entry->Implementation = &Implementation;
		auto& State = GetRegistryState();
		{
			std::lock_guard Lock(State.Mutex);
			if (Owner->bFeatureAdmissionRetired.load(std::memory_order_acquire)) return {};
			Entry->Identity = State.NextEntryIdentity++;
			State.Entries.push_back(Entry);
		}
		return FModularFeatureRegistration(std::move(Entry));
	}

	auto FModularFeatureRegistry::RegisterOwnedCallback(
		const std::shared_ptr<Detail::FModuleOwnerState>& Owner,
		FName DomainName) -> FModuleOwnedCallbackRegistration
	{
		if (!Owner || DomainName.IsNone()) return {};
		auto Entry = std::make_shared<Detail::FModularFeatureEntryState>();
		Entry->Owner = Owner;
		Entry->FeatureIdentity = {std::move(DomainName), 0};
		auto& State = GetRegistryState();
		{
			std::lock_guard Lock(State.Mutex);
			if (Owner->bFeatureAdmissionRetired.load(std::memory_order_acquire)) return {};
			Entry->Identity = State.NextEntryIdentity++;
			State.Entries.push_back(Entry);
		}
		return FModuleOwnedCallbackRegistration(std::move(Entry));
	}

	auto FModularFeatureRegistry::BeginInvoke(const FModularFeatureIdentity& Identity)
		-> std::vector<Detail::FModularFeatureInvocation>
	{
		std::vector<Detail::FModularFeatureInvocation> Result;
		auto& State = GetRegistryState();
		std::lock_guard Lock(State.Mutex);
		for (const auto& Entry : State.Entries)
		{
			if (Entry->State != Detail::EEntryState::Published || Entry->FeatureIdentity != Identity) continue;
			++Entry->InFlightCount;
			Result.push_back(Detail::FModularFeatureInvocation(Entry));
		}
		return Result;
	}

	auto FModularFeatureRegistry::BeginInvokeEntry(
		const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry)
		-> Detail::FModularFeatureInvocation
	{
		if (!Entry) return {};
		auto& State = GetRegistryState();
		std::lock_guard Lock(State.Mutex);
		if (Entry->State != Detail::EEntryState::Published) return {};
		++Entry->InFlightCount;
		return Detail::FModularFeatureInvocation(Entry);
	}

	auto FModularFeatureRegistry::RetainEntryResource(
		const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry)
		-> FModuleOwnedResourceLease
	{
		if (!Entry) return {};
		auto& State = GetRegistryState();
		std::lock_guard Lock(State.Mutex);
		if (Entry->State != Detail::EEntryState::Published) return {};
		++Entry->RetainedResourceCount;
		return FModuleOwnedResourceLease(Entry);
	}

	auto FModularFeatureRegistry::RetireEntry(const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry)
		-> FModularFeatureRetirementSnapshot
	{
		auto& State = GetRegistryState();
		std::lock_guard Lock(State.Mutex);
		if (Entry && Entry->State == Detail::EEntryState::Published) Entry->State = Detail::EEntryState::Retiring;
		RetireCompletedEntriesLocked(State);
		return MakeEntrySnapshotLocked(State, Entry);
	}

	auto FModularFeatureRegistry::WaitEntry(
		const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry,
		std::chrono::milliseconds Timeout
	) -> FModularFeatureRetirementResult
	{
		if (!Entry) return {EModularFeatureRetirementStatus::InvalidRegistration, {}, "The registration token is empty."};
		auto& State = GetRegistryState();
		std::unique_lock Lock(State.Mutex);
		if (IsOwnerActiveOnThisThread(Entry->Owner.get()))
		{
			return {EModularFeatureRetirementStatus::SelfWait, MakeEntrySnapshotLocked(State, Entry),
				"Retirement cannot wait from inside the matching feature invocation."};
		}
		const bool bDrained = State.Quiescence.wait_for(Lock, Timeout, [&]() {
			return Entry->InFlightCount == 0 && Entry->RetainedResourceCount == 0;
		});
		RetireCompletedEntriesLocked(State);
		return {
			bDrained ? EModularFeatureRetirementStatus::Succeeded : EModularFeatureRetirementStatus::TimedOut,
			MakeEntrySnapshotLocked(State, Entry),
			bDrained ? "Feature registration retired." : "Timed out waiting for feature invocation retirement."
		};
	}

	auto FModularFeatureRegistry::RetireOwner(
		const std::shared_ptr<Detail::FModuleOwnerState>& Owner,
		std::chrono::milliseconds Timeout
	) -> FModularFeatureRetirementResult
	{
		if (!Owner) return {EModularFeatureRetirementStatus::InvalidRegistration, {}, "The module owner is invalid."};
		auto& State = GetRegistryState();
		std::unique_lock Lock(State.Mutex);
		Owner->bFeatureAdmissionRetired.store(true, std::memory_order_release);
		for (const auto& Entry : State.Entries)
		{
			if (Entry->Owner == Owner && Entry->State == Detail::EEntryState::Published)
			{
				Entry->State = Detail::EEntryState::Retiring;
			}
		}
		RetireCompletedEntriesLocked(State);
		if (IsOwnerActiveOnThisThread(Owner.get()))
		{
			return {EModularFeatureRetirementStatus::SelfWait, MakeSnapshotLocked(State, Owner),
				"Module retirement was requested from one of its own feature invocations."};
		}
		const bool bDrained = State.Quiescence.wait_for(Lock, Timeout, [&]() {
			return MakeSnapshotLocked(State, Owner).InFlightInvocationCount == 0;
		});
		RetireCompletedEntriesLocked(State);
		return {
			bDrained ? EModularFeatureRetirementStatus::Succeeded : EModularFeatureRetirementStatus::TimedOut,
			MakeSnapshotLocked(State, Owner),
			bDrained ? "Module feature owner retired." : "Timed out waiting for owned feature invocations."
		};
	}

	auto FModularFeatureRegistry::SnapshotOwner(const std::shared_ptr<Detail::FModuleOwnerState>& Owner)
		-> FModularFeatureRetirementSnapshot
	{
		auto& State = GetRegistryState();
		std::lock_guard Lock(State.Mutex);
		return MakeSnapshotLocked(State, Owner);
	}
}
