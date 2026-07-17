#pragma once

#include "CoreAPI.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	class FDelegateHandle
	{
	public:
		FDelegateHandle() = default;

		CORE_API static auto GenerateNewHandle() -> FDelegateHandle;

		auto IsValid() const -> bool { return Id != 0; }
		auto Reset() -> void { Id = 0; }
		auto GetId() const -> uint64 { return Id; }

		auto operator==(const FDelegateHandle& Other) const -> bool = default;

	private:
		explicit FDelegateHandle(uint64 InId)
			: Id(InId)
		{
		}

		uint64 Id = 0;
	};

	enum class EDelegateThreadSafety : uint8
	{
		NotThreadSafe,
		ThreadSafe
	};

	namespace Private
	{
		class FDelegateNullMutex
		{
		public:
			auto lock() -> void {}
			auto unlock() -> void {}
		};

		template<EDelegateThreadSafety ThreadSafety>
		using TDelegateMutex = std::conditional_t<ThreadSafety == EDelegateThreadSafety::ThreadSafe, std::mutex, FDelegateNullMutex>;

		[[noreturn]] CORE_API auto ReportUnboundDelegateExecution() -> void;

		template<typename Signature>
		struct TIsValidMulticastSignature : std::false_type
		{
		};

		template<typename... Args>
		struct TIsValidMulticastSignature<void(Args...)>
			: std::bool_constant<((std::is_lvalue_reference_v<Args> || (!std::is_reference_v<Args> && std::is_copy_constructible_v<Args>)) && ...)>
		{
		};

		template<typename Signature>
		inline constexpr bool TIsValidMulticastSignatureV = TIsValidMulticastSignature<Signature>::value;

		template<typename Signature>
		struct TDelegateBinding;

		template<typename ReturnType, typename... Args>
		struct TDelegateBinding<ReturnType(Args...)>
		{
			using FCallback = std::function<ReturnType(Args...)>;

			FCallback Callback;
			// A weak binding stores a raw invocation target, while this owner pins it for the entire call.
			std::function<std::shared_ptr<const void>()> PinLifetime;
			const void* Object = nullptr;
			bool bIsWeak = false;

			auto IsBound() const -> bool
			{
				if (!Callback)
				{
					return false;
				}
				return !bIsWeak || static_cast<bool>(PinLifetime());
			}

			auto Pin() const -> std::shared_ptr<const void>
			{
				return bIsWeak ? PinLifetime() : std::shared_ptr<const void>{};
			}

			template<typename Callable>
		static auto Create(Callable&& InCallable, const void* InObject = nullptr) -> TDelegateBinding
		{
				TDelegateBinding Binding;
				Binding.Callback = FCallback(std::forward<Callable>(InCallable));
				Binding.Object = InObject;
				return Binding;
		}

			template<typename T, typename MethodType>
		static auto CreateWeak(std::weak_ptr<T> InInstance, MethodType InMethod) -> TDelegateBinding
		{
				const std::shared_ptr<T> Instance = InInstance.lock();
				T* Object = Instance.get();

				TDelegateBinding Binding;
				Binding.Callback = [Object, InMethod](Args... InArgs) -> ReturnType {
					if constexpr (std::is_void_v<ReturnType>)
					{
						(Object->*InMethod)(std::forward<Args>(InArgs)...);
					}
					else
					{
						return (Object->*InMethod)(std::forward<Args>(InArgs)...);
					}
				};
				Binding.PinLifetime = [WeakInstance = std::move(InInstance)]() -> std::shared_ptr<const void> {
					return WeakInstance.lock();
				};
				Binding.Object = Object;
				Binding.bIsWeak = true;
				return Binding;
		}
		};
	}

	template<typename Signature, EDelegateThreadSafety ThreadSafety = EDelegateThreadSafety::NotThreadSafe>
	class TDelegate;

	template<typename ReturnType, EDelegateThreadSafety ThreadSafety, typename... Args>
	class TDelegate<ReturnType(Args...), ThreadSafety>
	{
	public:
		using FCallback = std::function<ReturnType(Args...)>;

		TDelegate() = default;

		TDelegate(const TDelegate& Other)
		{
			std::lock_guard Lock(Other.Mutex);
			Binding = Other.Binding;
		}

		TDelegate(TDelegate&& Other) noexcept
		{
			std::lock_guard Lock(Other.Mutex);
			Binding = std::move(Other.Binding);
			Other.Binding = {};
		}

		auto operator=(const TDelegate& Other) -> TDelegate&
		{
			if (this == &Other)
			{
				return *this;
			}

			FBinding NewBinding;
			{
				std::lock_guard Lock(Other.Mutex);
				NewBinding = Other.Binding;
			}
			{
				std::lock_guard Lock(Mutex);
				Binding = std::move(NewBinding);
			}
			return *this;
		}

		auto operator=(TDelegate&& Other) noexcept -> TDelegate&
		{
			if (this == &Other)
			{
				return *this;
			}

			FBinding NewBinding;
			{
				std::lock_guard Lock(Other.Mutex);
				NewBinding = std::move(Other.Binding);
				Other.Binding = {};
			}
			{
				std::lock_guard Lock(Mutex);
				Binding = std::move(NewBinding);
			}
			return *this;
		}

		auto BindStatic(ReturnType (*InFunction)(Args...)) -> void
		{
			SetBinding(FBinding::Create(InFunction));
		}

		template<typename T>
		auto BindRaw(T* InInstance, ReturnType (T::*InMethod)(Args...)) -> void
		{
			SetBinding(FBinding::Create(
				[InInstance, InMethod](Args... InArgs) -> ReturnType {
					if constexpr (std::is_void_v<ReturnType>)
					{
						(InInstance->*InMethod)(std::forward<Args>(InArgs)...);
					}
					else
					{
						return (InInstance->*InMethod)(std::forward<Args>(InArgs)...);
					}
				},
				InInstance
			));
		}

		template<typename T>
		auto BindRaw(const T* InInstance, ReturnType (T::*InMethod)(Args...) const) -> void
		{
			SetBinding(FBinding::Create(
				[InInstance, InMethod](Args... InArgs) -> ReturnType {
					if constexpr (std::is_void_v<ReturnType>)
					{
						(InInstance->*InMethod)(std::forward<Args>(InArgs)...);
					}
					else
					{
						return (InInstance->*InMethod)(std::forward<Args>(InArgs)...);
					}
				},
				InInstance
			));
		}

		template<typename T>
		auto BindSP(std::weak_ptr<T> InInstance, ReturnType (T::*InMethod)(Args...)) -> void
		{
			SetBinding(FBinding::CreateWeak(std::move(InInstance), InMethod));
		}

		template<typename T>
		auto BindSP(const std::shared_ptr<T>& InInstance, ReturnType (T::*InMethod)(Args...)) -> void
		{
			BindSP(std::weak_ptr<T>(InInstance), InMethod);
		}

		template<typename T>
		auto BindSP(std::weak_ptr<T> InInstance, ReturnType (T::*InMethod)(Args...) const) -> void
		{
			SetBinding(FBinding::CreateWeak(std::move(InInstance), InMethod));
		}

		template<typename T>
		auto BindSP(const std::shared_ptr<T>& InInstance, ReturnType (T::*InMethod)(Args...) const) -> void
		{
			BindSP(std::weak_ptr<T>(InInstance), InMethod);
		}

		template<typename Callable>
		auto BindLambda(Callable&& InCallable) -> void
		{
			SetBinding(FBinding::Create(std::forward<Callable>(InCallable)));
		}

		auto Unbind() -> void
		{
			std::lock_guard Lock(Mutex);
			Binding = {};
		}

		auto IsBound() const -> bool
		{
			const FBinding Snapshot = GetBindingSnapshot();
			return Snapshot.IsBound();
		}

		auto Execute(Args... InArgs) const -> ReturnType
		{
			const FBinding Snapshot = GetBindingSnapshot();
			const std::shared_ptr<const void> Lifetime = Snapshot.Pin();
			if (!Snapshot.Callback || (Snapshot.bIsWeak && !Lifetime))
			{
				Private::ReportUnboundDelegateExecution();
			}

			if constexpr (std::is_void_v<ReturnType>)
			{
				Snapshot.Callback(std::forward<Args>(InArgs)...);
			}
			else
			{
				return Snapshot.Callback(std::forward<Args>(InArgs)...);
			}
		}

		auto ExecuteIfBound(Args... InArgs) const -> bool requires std::is_void_v<ReturnType>
		{
			const FBinding Snapshot = GetBindingSnapshot();
			const std::shared_ptr<const void> Lifetime = Snapshot.Pin();
			if (!Snapshot.Callback || (Snapshot.bIsWeak && !Lifetime))
			{
				return false;
			}

			Snapshot.Callback(std::forward<Args>(InArgs)...);
			return true;
		}

	private:
		using FBinding = Private::TDelegateBinding<ReturnType(Args...)>;
		using FMutex = Private::TDelegateMutex<ThreadSafety>;

		auto SetBinding(FBinding InBinding) -> void
		{
			std::lock_guard Lock(Mutex);
			Binding = std::move(InBinding);
		}

		auto GetBindingSnapshot() const -> FBinding
		{
			std::lock_guard Lock(Mutex);
			return Binding;
		}

		FBinding Binding;
		mutable FMutex Mutex;
	};

	template<typename Signature, EDelegateThreadSafety ThreadSafety = EDelegateThreadSafety::NotThreadSafe>
	class TMulticastDelegate
	{
		static_assert(Private::TIsValidMulticastSignatureV<Signature>, "Multicast delegates require a void signature and copyable by-value parameters");
	};

	template<EDelegateThreadSafety ThreadSafety, typename... Args>
	class TMulticastDelegate<void(Args...), ThreadSafety>
	{
		static_assert(Private::TIsValidMulticastSignatureV<void(Args...)>, "Multicast delegates do not support rvalue-reference or move-only by-value parameters");

	public:
		using FCallback = std::function<void(Args...)>;

		TMulticastDelegate() = default;
		TMulticastDelegate(const TMulticastDelegate&) = delete;
		auto operator=(const TMulticastDelegate&) -> TMulticastDelegate& = delete;

		TMulticastDelegate(TMulticastDelegate&& Other) noexcept
		{
			std::lock_guard Lock(Other.Mutex);
			Listeners = std::move(Other.Listeners);
			Other.Listeners.clear();
		}

		auto operator=(TMulticastDelegate&& Other) noexcept -> TMulticastDelegate&
		{
			if (this == &Other)
			{
				return *this;
			}

			std::vector<FListener> NewListeners;
			{
				std::lock_guard Lock(Other.Mutex);
				NewListeners = std::move(Other.Listeners);
				Other.Listeners.clear();
			}
			{
				std::lock_guard Lock(Mutex);
				Listeners = std::move(NewListeners);
			}
			return *this;
		}

		auto AddStatic(void (*InFunction)(Args...)) -> FDelegateHandle
		{
			return AddBinding(FBinding::Create(InFunction));
		}

		template<typename T>
		auto AddRaw(T* InInstance, void (T::*InMethod)(Args...)) -> FDelegateHandle
		{
			return AddBinding(FBinding::Create(
				[InInstance, InMethod](Args... InArgs) {
					(InInstance->*InMethod)(std::forward<Args>(InArgs)...);
				},
				InInstance
			));
		}

		template<typename T>
		auto AddRaw(const T* InInstance, void (T::*InMethod)(Args...) const) -> FDelegateHandle
		{
			return AddBinding(FBinding::Create(
				[InInstance, InMethod](Args... InArgs) {
					(InInstance->*InMethod)(std::forward<Args>(InArgs)...);
				},
				InInstance
			));
		}

		template<typename T>
		auto AddSP(std::weak_ptr<T> InInstance, void (T::*InMethod)(Args...)) -> FDelegateHandle
		{
			return AddBinding(FBinding::CreateWeak(std::move(InInstance), InMethod));
		}

		template<typename T>
		auto AddSP(const std::shared_ptr<T>& InInstance, void (T::*InMethod)(Args...)) -> FDelegateHandle
		{
			return AddSP(std::weak_ptr<T>(InInstance), InMethod);
		}

		template<typename T>
		auto AddSP(std::weak_ptr<T> InInstance, void (T::*InMethod)(Args...) const) -> FDelegateHandle
		{
			return AddBinding(FBinding::CreateWeak(std::move(InInstance), InMethod));
		}

		template<typename T>
		auto AddSP(const std::shared_ptr<T>& InInstance, void (T::*InMethod)(Args...) const) -> FDelegateHandle
		{
			return AddSP(std::weak_ptr<T>(InInstance), InMethod);
		}

		template<typename Callable>
		auto AddLambda(Callable&& InCallable) -> FDelegateHandle
		{
			return AddBinding(FBinding::Create(std::forward<Callable>(InCallable)));
		}

		auto Remove(const FDelegateHandle& Handle) -> bool
		{
			if (!Handle.IsValid())
			{
				return false;
			}

			std::lock_guard Lock(Mutex);
			const auto It = std::find_if(Listeners.begin(), Listeners.end(), [&Handle](const FListener& Listener) {
				return Listener.Handle == Handle;
			});
			if (It == Listeners.end())
			{
				return false;
			}
			Listeners.erase(It);
			return true;
		}

		template<typename T>
		auto RemoveAll(const T* InInstance) -> size_t
		{
			const void* Object = InInstance;
			std::lock_guard Lock(Mutex);
			const size_t PreviousNum = Listeners.size();
			std::erase_if(Listeners, [Object](const FListener& Listener) {
				return Listener.Binding.Object == Object;
			});
			return PreviousNum - Listeners.size();
		}

		auto Clear() -> void
		{
			std::lock_guard Lock(Mutex);
			Listeners.clear();
		}

		auto IsBound() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return std::ranges::any_of(Listeners, [](const FListener& Listener) {
				return Listener.Binding.IsBound();
			});
		}

		auto Num() const -> size_t
		{
			std::lock_guard Lock(Mutex);
			return Listeners.size();
		}

		auto Broadcast(Args... InArgs) -> void
		{
			std::vector<FListener> Snapshot;
			{
				std::lock_guard Lock(Mutex);
				Snapshot = Listeners;
			}

			// Mutations made by callbacks affect future broadcasts, including nested broadcasts, but not this snapshot.
			bool bHasExpiredListener = false;
			for (const FListener& Listener : Snapshot)
			{
				const std::shared_ptr<const void> Lifetime = Listener.Binding.Pin();
				if (!Listener.Binding.Callback || (Listener.Binding.bIsWeak && !Lifetime))
				{
					bHasExpiredListener |= Listener.Binding.bIsWeak;
					continue;
				}
				// Named arguments remain lvalues so every listener receives its own copy for by-value parameters.
				Listener.Binding.Callback(InArgs...);
			}

			if (bHasExpiredListener)
			{
				RemoveExpired();
			}
		}

	private:
		using FBinding = Private::TDelegateBinding<void(Args...)>;
		using FMutex = Private::TDelegateMutex<ThreadSafety>;

		struct FListener
		{
			FDelegateHandle Handle;
			FBinding Binding;
		};

		auto AddBinding(FBinding InBinding) -> FDelegateHandle
		{
			FListener Listener{ FDelegateHandle::GenerateNewHandle(), std::move(InBinding) };
			const FDelegateHandle Handle = Listener.Handle;
			std::lock_guard Lock(Mutex);
			Listeners.push_back(std::move(Listener));
			return Handle;
		}

		auto RemoveExpired() -> void
		{
			std::lock_guard Lock(Mutex);
			std::erase_if(Listeners, [](const FListener& Listener) {
				return Listener.Binding.bIsWeak && !Listener.Binding.IsBound();
			});
		}

		std::vector<FListener> Listeners;
		mutable FMutex Mutex;
	};

	template<typename Signature>
	using TThreadSafeDelegate = TDelegate<Signature, EDelegateThreadSafety::ThreadSafe>;

	template<typename Signature>
	using TThreadSafeMulticastDelegate = TMulticastDelegate<Signature, EDelegateThreadSafety::ThreadSafe>;
}

#define DURIN_DECLARE_DELEGATE(ThreadSafety, DelegateName, ReturnType, ...) \
	using DelegateName = Durin::TDelegate<ReturnType(__VA_ARGS__), ThreadSafety>;

#define DURIN_DECLARE_MULTICAST_DELEGATE(ThreadSafety, DelegateName, ...) \
	using DelegateName = Durin::TMulticastDelegate<void(__VA_ARGS__), ThreadSafety>;

#define DECLARE_DELEGATE(DelegateName) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, void)
#define DECLARE_DELEGATE_OneParam(DelegateName, Param1Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, void, Param1Type)
#define DECLARE_DELEGATE_TwoParams(DelegateName, Param1Type, Param2Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, void, Param1Type, Param2Type)
#define DECLARE_DELEGATE_ThreeParams(DelegateName, Param1Type, Param2Type, Param3Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, void, Param1Type, Param2Type, Param3Type)

#define DECLARE_DELEGATE_RetVal(ReturnType, DelegateName) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, ReturnType)
#define DECLARE_DELEGATE_RetVal_OneParam(ReturnType, DelegateName, Param1Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, ReturnType, Param1Type)
#define DECLARE_DELEGATE_RetVal_TwoParams(ReturnType, DelegateName, Param1Type, Param2Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, ReturnType, Param1Type, Param2Type)
#define DECLARE_DELEGATE_RetVal_ThreeParams(ReturnType, DelegateName, Param1Type, Param2Type, Param3Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, ReturnType, Param1Type, Param2Type, Param3Type)

#define DECLARE_MULTICAST_DELEGATE(DelegateName) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName)
#define DECLARE_MULTICAST_DELEGATE_OneParam(DelegateName, Param1Type) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, Param1Type)
#define DECLARE_MULTICAST_DELEGATE_TwoParams(DelegateName, Param1Type, Param2Type) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, Param1Type, Param2Type)
#define DECLARE_MULTICAST_DELEGATE_ThreeParams(DelegateName, Param1Type, Param2Type, Param3Type) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::NotThreadSafe, DelegateName, Param1Type, Param2Type, Param3Type)

#define DECLARE_TS_DELEGATE(DelegateName) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, void)
#define DECLARE_TS_DELEGATE_OneParam(DelegateName, Param1Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, void, Param1Type)
#define DECLARE_TS_DELEGATE_TwoParams(DelegateName, Param1Type, Param2Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, void, Param1Type, Param2Type)
#define DECLARE_TS_DELEGATE_ThreeParams(DelegateName, Param1Type, Param2Type, Param3Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, void, Param1Type, Param2Type, Param3Type)

#define DECLARE_TS_DELEGATE_RetVal(ReturnType, DelegateName) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, ReturnType)
#define DECLARE_TS_DELEGATE_RetVal_OneParam(ReturnType, DelegateName, Param1Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, ReturnType, Param1Type)
#define DECLARE_TS_DELEGATE_RetVal_TwoParams(ReturnType, DelegateName, Param1Type, Param2Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, ReturnType, Param1Type, Param2Type)
#define DECLARE_TS_DELEGATE_RetVal_ThreeParams(ReturnType, DelegateName, Param1Type, Param2Type, Param3Type) \
	DURIN_DECLARE_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, ReturnType, Param1Type, Param2Type, Param3Type)

#define DECLARE_TS_MULTICAST_DELEGATE(DelegateName) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName)
#define DECLARE_TS_MULTICAST_DELEGATE_OneParam(DelegateName, Param1Type) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, Param1Type)
#define DECLARE_TS_MULTICAST_DELEGATE_TwoParams(DelegateName, Param1Type, Param2Type) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, Param1Type, Param2Type)
#define DECLARE_TS_MULTICAST_DELEGATE_ThreeParams(DelegateName, Param1Type, Param2Type, Param3Type) \
	DURIN_DECLARE_MULTICAST_DELEGATE(Durin::EDelegateThreadSafety::ThreadSafe, DelegateName, Param1Type, Param2Type, Param3Type)
