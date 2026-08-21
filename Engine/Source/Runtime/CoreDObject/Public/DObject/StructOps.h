#pragma once

#include "CoreDObjectAPI.h"
#include "Misc/EnumClassFlags.h"

#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace Durin
{
	class FArchive;
	struct FArchiveVersionContext;
	class FReferenceCollector;

	inline constexpr uint32 DStructOpsVersion = 1;

	enum class EDStructOpsFlags : uint16
	{
		None = 0,
		DefaultConstruct = 1 << 0,
		TriviallyDestructible = 1 << 1,
		Destroy = 1 << 2,
		CopyConstruct = 1 << 3,
		CopyAssign = 1 << 4,
		ZeroConstruct = 1 << 5,
		Identical = 1 << 6,
		Serialize = 1 << 7,
		PostDeserialize = 1 << 8,
		CollectReferences = 1 << 9,
		AuthoredFieldsComplete = 1 << 10
	};
	ENUM_CLASS_FLAGS(EDStructOpsFlags)

	enum class EDStructDeserializeSource : uint8
	{
		RuntimeArchive,
		AuthoredAsset
	};

	struct FDStructPostDeserializeContext
	{
		EDStructDeserializeSource Source = EDStructDeserializeSource::RuntimeArchive;
		uint32 SourceVersion = 0;
		const FArchiveVersionContext* VersionContext = nullptr;
		std::string* Error = nullptr;

		auto Fail(std::string_view Message) const -> bool
		{
			if (Error) *Error = Message;
			return false;
		}
	};

	struct FDStructOps
	{
		using FDefaultConstruct = void (*)(void* Destination);
		using FDestroy = void (*)(void* Value);
		using FCopyConstruct = void (*)(void* Destination, const void* Source);
		using FCopyAssign = void (*)(void* Destination, const void* Source);
		using FZeroConstruct = void (*)(void* Destination);
		using FIdentical = bool (*)(const void* Left, const void* Right);
		using FSerialize = void (*)(FArchive& Archive, void* Value);
		using FPostDeserialize = bool (*)(void* Value, FDStructPostDeserializeContext& Context);
		using FCollectReferences = void (*)(void* Value, FReferenceCollector& Collector);

		uint32 Version = DStructOpsVersion;
		EDStructOpsFlags Flags = EDStructOpsFlags::None;
		FDefaultConstruct DefaultConstruct = nullptr;
		FDestroy Destroy = nullptr;
		FCopyConstruct CopyConstruct = nullptr;
		FCopyAssign CopyAssign = nullptr;
		FZeroConstruct ZeroConstruct = nullptr;
		FIdentical Identical = nullptr;
		FSerialize Serialize = nullptr;
		FPostDeserialize PostDeserialize = nullptr;
		FCollectReferences CollectReferences = nullptr;
	};

	template<typename T>
	struct TDStructOpsTraitsBase
	{
		static constexpr bool bWithDefaultConstruct = std::is_default_constructible_v<T>;
		static constexpr bool bWithDestroy = std::is_destructible_v<T>;
		static constexpr bool bIsTriviallyDestructible = std::is_trivially_destructible_v<T>;
		static constexpr bool bWithCopyConstruct = std::is_copy_constructible_v<T>;
		static constexpr bool bWithCopyAssign = std::is_copy_assignable_v<T>;
		static constexpr bool bWithZeroConstruct = false;
		static constexpr bool bWithIdentical = false;
		static constexpr bool bWithSerializer = false;
		static constexpr bool bWithPostDeserialize = false;
		static constexpr bool bWithReferenceCollector = false;
		static constexpr bool bHasCompleteAuthoredFields = true;

		static auto DefaultConstruct(void* Destination) -> void
			requires std::is_default_constructible_v<T>
		{
			std::construct_at(static_cast<T*>(Destination));
		}

		static auto Destroy(T& Value) -> void
			requires std::is_destructible_v<T>
		{
			std::destroy_at(&Value);
		}

		static auto CopyConstruct(void* Destination, const T& Source) -> void
			requires std::is_copy_constructible_v<T>
		{
			std::construct_at(static_cast<T*>(Destination), Source);
		}

		static auto CopyAssign(T& Destination, const T& Source) -> void
			requires std::is_copy_assignable_v<T>
		{
			Destination = Source;
		}
	};

	template<typename T>
	struct TDStructOpsTraits : TDStructOpsTraitsBase<T>
	{
	};

	namespace Private
	{
		template<typename Traits>
		concept CValidDStructDefaultConstructTrait = requires
		{
			static_cast<void (*)(void*)>(&Traits::DefaultConstruct);
		};

		template<typename T, typename Traits>
		concept CValidDStructDestroyTrait = requires
		{
			static_cast<void (*)(T&)>(&Traits::Destroy);
		};

		template<typename T, typename Traits>
		concept CValidDStructCopyConstructTrait = requires
		{
			static_cast<void (*)(void*, const T&)>(&Traits::CopyConstruct);
		};

		template<typename T, typename Traits>
		concept CValidDStructCopyAssignTrait = requires
		{
			static_cast<void (*)(T&, const T&)>(&Traits::CopyAssign);
		};

		template<typename Traits>
		concept CValidDStructZeroConstructTrait = requires
		{
			static_cast<void (*)(void*)>(&Traits::ZeroConstruct);
		};

		template<typename T, typename Traits>
		concept CValidDStructIdenticalTrait = requires
		{
			static_cast<bool (*)(const T&, const T&)>(&Traits::Identical);
		};

		template<typename T, typename Traits>
		concept CValidDStructSerializeTrait = requires
		{
			static_cast<void (*)(FArchive&, T&)>(&Traits::Serialize);
		};

		template<typename T, typename Traits>
		concept CValidDStructPostDeserializeTrait = requires
		{
			static_cast<bool (*)(T&, FDStructPostDeserializeContext&)>(&Traits::PostDeserialize);
		};

		template<typename T, typename Traits>
		concept CValidDStructReferenceCollectorTrait = requires
		{
			static_cast<void (*)(T&, FReferenceCollector&)>(&Traits::CollectReferences);
		};

		template<typename T>
		auto DefaultConstructDStruct(void* Destination) -> void
		{
			TDStructOpsTraits<T>::DefaultConstruct(Destination);
		}

		template<typename T>
		auto DestroyDStruct(void* Value) -> void
		{
			TDStructOpsTraits<T>::Destroy(*static_cast<T*>(Value));
		}

		template<typename T>
		auto CopyConstructDStruct(void* Destination, const void* Source) -> void
		{
			TDStructOpsTraits<T>::CopyConstruct(Destination, *static_cast<const T*>(Source));
		}

		template<typename T>
		auto CopyAssignDStruct(void* Destination, const void* Source) -> void
		{
			TDStructOpsTraits<T>::CopyAssign(*static_cast<T*>(Destination), *static_cast<const T*>(Source));
		}

		template<typename T>
		auto ZeroConstructDStruct(void* Destination) -> void
		{
			TDStructOpsTraits<T>::ZeroConstruct(Destination);
		}

		template<typename T>
		auto IdenticalDStruct(const void* Left, const void* Right) -> bool
		{
			return TDStructOpsTraits<T>::Identical(*static_cast<const T*>(Left), *static_cast<const T*>(Right));
		}

		template<typename T>
		auto SerializeDStruct(FArchive& Archive, void* Value) -> void
		{
			TDStructOpsTraits<T>::Serialize(Archive, *static_cast<T*>(Value));
		}

		template<typename T>
		auto PostDeserializeDStruct(void* Value, FDStructPostDeserializeContext& Context) -> bool
		{
			return TDStructOpsTraits<T>::PostDeserialize(*static_cast<T*>(Value), Context);
		}

		template<typename T>
		auto CollectDStructReferences(void* Value, FReferenceCollector& Collector) -> void
		{
			TDStructOpsTraits<T>::CollectReferences(*static_cast<T*>(Value), Collector);
		}

		template<typename T>
		auto BuildDStructOps() -> FDStructOps
		{
			using Traits = TDStructOpsTraits<T>;
			FDStructOps Ops;

			static_assert(
				!Traits::bIsTriviallyDestructible || std::is_trivially_destructible_v<T>,
				"TDStructOpsTraits<T>::bIsTriviallyDestructible requires a trivially destructible T."
			);

			if constexpr (Traits::bWithDefaultConstruct)
			{
				constexpr bool bValid = CValidDStructDefaultConstructTrait<Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::DefaultConstruct must have signature void(void*).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::DefaultConstruct;
					Ops.DefaultConstruct = &DefaultConstructDStruct<T>;
				}
			}

			if constexpr (Traits::bIsTriviallyDestructible)
			{
				Ops.Flags |= EDStructOpsFlags::TriviallyDestructible;
			}
			else if constexpr (Traits::bWithDestroy)
			{
				constexpr bool bValid = CValidDStructDestroyTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::Destroy must have signature void(T&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::Destroy;
					Ops.Destroy = &DestroyDStruct<T>;
				}
			}

			if constexpr (Traits::bWithCopyConstruct)
			{
				constexpr bool bValid = CValidDStructCopyConstructTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::CopyConstruct must have signature void(void*, const T&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::CopyConstruct;
					Ops.CopyConstruct = &CopyConstructDStruct<T>;
				}
			}

			if constexpr (Traits::bWithCopyAssign)
			{
				constexpr bool bValid = CValidDStructCopyAssignTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::CopyAssign must have signature void(T&, const T&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::CopyAssign;
					Ops.CopyAssign = &CopyAssignDStruct<T>;
				}
			}

			if constexpr (Traits::bWithZeroConstruct)
			{
				constexpr bool bValid = CValidDStructZeroConstructTrait<Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::ZeroConstruct must have signature void(void*).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::ZeroConstruct;
					Ops.ZeroConstruct = &ZeroConstructDStruct<T>;
				}
			}

			if constexpr (Traits::bWithIdentical)
			{
				constexpr bool bValid = CValidDStructIdenticalTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::Identical must have signature bool(const T&, const T&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::Identical;
					Ops.Identical = &IdenticalDStruct<T>;
				}
			}

			if constexpr (Traits::bWithSerializer)
			{
				constexpr bool bValid = CValidDStructSerializeTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::Serialize must have signature void(FArchive&, T&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::Serialize;
					Ops.Serialize = &SerializeDStruct<T>;
				}
			}

			if constexpr (Traits::bWithPostDeserialize)
			{
				constexpr bool bValid = CValidDStructPostDeserializeTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::PostDeserialize must have signature bool(T&, FDStructPostDeserializeContext&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::PostDeserialize;
					Ops.PostDeserialize = &PostDeserializeDStruct<T>;
				}
			}

			if constexpr (Traits::bWithReferenceCollector)
			{
				constexpr bool bValid = CValidDStructReferenceCollectorTrait<T, Traits>;
				static_assert(bValid, "TDStructOpsTraits<T>::CollectReferences must have signature void(T&, FReferenceCollector&).");
				if constexpr (bValid)
				{
					Ops.Flags |= EDStructOpsFlags::CollectReferences;
					Ops.CollectReferences = &CollectDStructReferences<T>;
				}
			}

			if constexpr (Traits::bHasCompleteAuthoredFields)
			{
				Ops.Flags |= EDStructOpsFlags::AuthoredFieldsComplete;
			}

			return Ops;
		}
	}

	template<typename T>
	auto GetDStructOps() -> const FDStructOps&
	{
		static const FDStructOps Ops = Private::BuildDStructOps<T>();
		return Ops;
	}

	inline auto GetEmptyDStructOps() -> const FDStructOps&
	{
		static const FDStructOps Ops;
		return Ops;
	}

	inline auto IsValidDStructOps(const FDStructOps& Ops) -> bool
	{
		if (Ops.Version != DStructOpsVersion) return false;
		const auto Matches = [&Ops](EDStructOpsFlags Flag, bool bHasCallback)
		{
			return EnumHasAnyFlags(Ops.Flags, Flag) == bHasCallback;
		};
		return Matches(EDStructOpsFlags::DefaultConstruct, Ops.DefaultConstruct != nullptr)
			&& Matches(EDStructOpsFlags::Destroy, Ops.Destroy != nullptr)
			&& Matches(EDStructOpsFlags::CopyConstruct, Ops.CopyConstruct != nullptr)
			&& Matches(EDStructOpsFlags::CopyAssign, Ops.CopyAssign != nullptr)
			&& Matches(EDStructOpsFlags::ZeroConstruct, Ops.ZeroConstruct != nullptr)
			&& Matches(EDStructOpsFlags::Identical, Ops.Identical != nullptr)
			&& Matches(EDStructOpsFlags::Serialize, Ops.Serialize != nullptr)
			&& Matches(EDStructOpsFlags::PostDeserialize, Ops.PostDeserialize != nullptr)
			&& Matches(EDStructOpsFlags::CollectReferences, Ops.CollectReferences != nullptr);
	}
}
