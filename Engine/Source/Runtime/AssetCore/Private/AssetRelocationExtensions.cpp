#include "AssetRelocationExtensionsInternal.h"

#include "DObject/Class.h"

namespace Durin::Asset
{
	namespace
	{
		struct FRegisteredOwnedPayloadRelocator
		{
			FAssetOwnedPayloadRelocatorHandle Handle = 0;
			FModuleOwnedResourceLease OwnerResource;
			FAssetOwnedPayloadRelocator Relocator;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto GetOwnedPayloadRelocators()
			-> std::unordered_map<DClass*, FRegisteredOwnedPayloadRelocator>&
		{
			static std::unordered_map<DClass*, FRegisteredOwnedPayloadRelocator>
				Relocators;
			return Relocators;
		}

		auto NextOwnedPayloadRelocatorHandle()
			-> FAssetOwnedPayloadRelocatorHandle&
		{
			static FAssetOwnedPayloadRelocatorHandle Handle = 1;
			return Handle;
		}

		struct FRegisteredAssetMoveObserver
		{
			FModuleOwnedResourceLease OwnerResource;
			IAssetMoveObserver* Observer = nullptr;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto GetMoveObservers()
			-> std::map<FAssetMoveObserverHandle, FRegisteredAssetMoveObserver>&
		{
			static std::map<FAssetMoveObserverHandle, FRegisteredAssetMoveObserver>
				Observers;
			return Observers;
		}

		auto NextMoveObserverHandle() -> FAssetMoveObserverHandle&
		{
			static FAssetMoveObserverHandle Handle = 1;
			return Handle;
		}

		struct FRelocationFailureInjection
		{
			std::map<EAssetRelocationFailurePoint, uint32> RemainingOccurrences;
		};

		auto GetRelocationFailureInjection() -> FRelocationFailureInjection&
		{
			static FRelocationFailureInjection Injection;
			return Injection;
		}
	}

	auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetOwnedPayloadRelocatorHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (!Class || !Relocator || (OwnerGate.IsValid() && !Call)) return 0;
		auto& Relocators = GetOwnedPayloadRelocators();
		if (Relocators.contains(Class)) return 0;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		auto& NextHandle = NextOwnedPayloadRelocatorHandle();
		const FAssetOwnedPayloadRelocatorHandle Handle = NextHandle++;
		Relocators.emplace(Class, FRegisteredOwnedPayloadRelocator{
			.Handle = Handle,
			.OwnerResource = std::move(Resource),
			.Relocator = std::move(Relocator),
			.OwnerGate = std::move(OwnerGate),
		});
		return Handle;
	}

	auto UnregisterAssetOwnedPayloadRelocator(
		FAssetOwnedPayloadRelocatorHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Relocators = GetOwnedPayloadRelocators();
		std::erase_if(Relocators, [Handle](const auto& Pair) {
			return Pair.second.Handle == Handle;
		});
	}

	auto RegisterAssetMoveObserver(
		IAssetMoveObserver* Observer,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetMoveObserverHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return 0;
		if (!Observer) return 0;
		auto& NextHandle = NextMoveObserverHandle();
		const FAssetMoveObserverHandle Handle = NextHandle++;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		GetMoveObservers().emplace(Handle,
			FRegisteredAssetMoveObserver{
				std::move(Resource), Observer, std::move(OwnerGate)});
		return Handle;
	}

	auto UnregisterAssetMoveObserver(FAssetMoveObserverHandle Handle) -> void
	{
		if (Handle != 0) GetMoveObservers().erase(Handle);
	}

	auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence) -> void
	{
		auto& Injection = GetRelocationFailureInjection();
		if (Point == EAssetRelocationFailurePoint::None)
		{
			Injection.RemainingOccurrences.clear();
			return;
		}
		Injection.RemainingOccurrences.insert_or_assign(
			Point, std::max(Occurrence, 1u));
	}

	namespace Private
	{
		auto AcquireAssetOwnedPayloadRelocator(
			DClass* AssetClass,
			FAssetOwnedPayloadRelocatorInvocation& OutInvocation) -> FAssetResult
		{
			OutInvocation = {};
			auto& Relocators = GetOwnedPayloadRelocators();
			for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
			{
				auto Found = Relocators.find(Class);
				if (Found == Relocators.end()) continue;
				auto Call = Found->second.OwnerGate.TryEnter();
				if (Found->second.OwnerGate.IsValid() && !Call)
					return {
						EAssetError::StaleData,
						"The owned-payload relocator is unavailable."};
				OutInvocation.Relocator = Found->second.Relocator;
				OutInvocation.OwnerCall = std::move(Call);
				return {};
			}
			return {};
		}

		auto NotifyAssetMoveObservers(
			std::span<const FAssetRelocationMapping> Mappings) -> void
		{
			for (const auto& [Handle, Entry] : GetMoveObservers())
			{
				(void)Handle;
				auto Call = Entry.OwnerGate.TryEnter();
				if (Entry.Observer && (!Entry.OwnerGate.IsValid() || Call))
					Entry.Observer->OnAssetsRelocated(Mappings);
			}
		}

		auto ConsumeAssetRelocationFailure(
			EAssetRelocationFailurePoint Point) -> bool
		{
			FRelocationFailureInjection& Injection =
				GetRelocationFailureInjection();
			auto Injected = Injection.RemainingOccurrences.find(Point);
			if (Injected == Injection.RemainingOccurrences.end()
				|| Injected->second == 0)
				return false;
			if (--Injected->second != 0) return false;
			Injection.RemainingOccurrences.erase(Injected);
			return true;
		}
	}
}
