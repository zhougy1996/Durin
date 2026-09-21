#include "Asset/MutationExtensions.h"

#include "DObject/Class.h"

namespace Durin
{
	namespace
	{
		struct FRegisteredOwnedPayloadRelocator
		{
			FAssetOwnedPayloadRelocatorHandle Handle = 0;
			FAssetOwnedPayloadRelocator Relocator;
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

		auto GetMoveObservers()
			-> std::map<FAssetMoveObserverHandle, IAssetMoveObserver*>&
		{
			static std::map<FAssetMoveObserverHandle, IAssetMoveObserver*>
				Observers;
			return Observers;
		}

		auto NextMoveObserverHandle() -> FAssetMoveObserverHandle&
		{
			static FAssetMoveObserverHandle Handle = 1;
			return Handle;
		}


	}

	auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator)
		-> FAssetOwnedPayloadRelocatorHandle
	{
		if (!Class || !Relocator) return 0;
		auto& Relocators = GetOwnedPayloadRelocators();
		if (Relocators.contains(Class)) return 0;

		auto& NextHandle = NextOwnedPayloadRelocatorHandle();
		const FAssetOwnedPayloadRelocatorHandle Handle = NextHandle++;
		Relocators.emplace(Class, FRegisteredOwnedPayloadRelocator{
			.Handle = Handle,
			.Relocator = std::move(Relocator),
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
		IAssetMoveObserver* Observer)
		-> FAssetMoveObserverHandle
	{
		if (!Observer) return 0;
		auto& NextHandle = NextMoveObserverHandle();
		const FAssetMoveObserverHandle Handle = NextHandle++;

		GetMoveObservers().emplace(Handle, Observer);
		return Handle;
	}

	auto UnregisterAssetMoveObserver(FAssetMoveObserverHandle Handle) -> void
	{
		if (Handle != 0) GetMoveObservers().erase(Handle);
	}


	auto FindAssetOwnedPayloadRelocator(DClass* AssetClass)
		-> FAssetOwnedPayloadRelocator
	{
		auto& Relocators = GetOwnedPayloadRelocators();
		for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
		{
			auto Found = Relocators.find(Class);
			if (Found == Relocators.end()) continue;

			return Found->second.Relocator;
		}
		return {};
	}

	auto NotifyAssetMoveObservers(
		std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const auto& [Handle, Observer] : GetMoveObservers())
		{
			(void)Handle;
			Observer->OnAssetsRelocated(Mappings);
		}
	}


}
