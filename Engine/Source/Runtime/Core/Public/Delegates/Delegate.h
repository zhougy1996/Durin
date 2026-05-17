#pragma once

#include "CoreAPI.h"

namespace Durin
{
	class FDelegateHandle
	{
	public:
		CORE_API FDelegateHandle();

		bool operator==(const FDelegateHandle& other) const { return Id == other.Id; }
		bool operator!=(const FDelegateHandle& other) const { return Id != other.Id; }

		uint32 GetId() const { return Id; }

	private:
		uint32 Id;
	};

	// Multicast delegate class
	// Return value not supported yet
	template<typename... Args>
	class TDelegate
	{
	public:
		using FCallback = std::function<void(Args...)>;

		~TDelegate() = default;

		FDelegateHandle Add(const FCallback& Func)
		{
			std::lock_guard Lock(Mutex);

			FListenerEntry NewEntry(Func);

			FDelegateHandle Handle = NewEntry.Handle;
			Listeners.push_back(std::move(NewEntry));
			return Handle;
		}

		template<typename T>
		FDelegateHandle AddRaw(T* Instance, void (T::*Func)(Args...))
		{
			std::lock_guard Lock(Mutex);

			FListenerEntry NewEntry([Instance, Func](Args... FuncArgs) { (Instance->*Func)(std::forward<FuncArgs>(FuncArgs)...); });
			const FDelegateHandle Handle = NewEntry.Handle;
			Listeners.push_back(std::move(NewEntry));
			return Handle;
		}

		template<typename T>
		FDelegateHandle AddWeak(std::weak_ptr<T> Instance, void (T::*Func)(Args...))
		{
			std::lock_guard Lock(Mutex);

			FListenerEntry NewEntry(Func);
			NewEntry.WeakPtr = Instance;
			NewEntry.bIsWeak = true;

			const FDelegateHandle Handle = NewEntry.Handle;
			Listeners.push_back(std::move(NewEntry));
			return Handle;
		}

		// Add lambda function, use perfect forwarding
		// Function Add(const Callback& callback) can also add lambda, but this one avoid unnecessary copy
		template<typename Lambda>
		FDelegateHandle AddLambda(Lambda&& Callback)
		{
			std::lock_guard Lock(Mutex);

			FListenerEntry NewEntry(std::forward<Lambda>(Callback));

			FDelegateHandle Handle = NewEntry.Handle;
			Listeners.push_back(std::move(NewEntry));
			return Handle;
		}

		void Remove(const FDelegateHandle& Handle)
		{
			std::lock_guard Lock(Mutex);

			Listeners.erase(
				std::remove_if(Listeners.begin(), Listeners.end(), [&](const auto& listenerEntry) { return listenerEntry.handle == Handle; }),
				Listeners.end()
			);
		}

		void RemoveExpired()
		{
			std::lock_guard Lock(Mutex);

			Listeners.erase(
				std::remove_if(Listeners.begin(), Listeners.end(), [](const auto& listenerEntry) { return listenerEntry.IsExpired(); }),
				Listeners.end()
			);
		}

		void Broadcast(Args... FuncArgs)
		{
			std::vector<FListenerEntry> Snapshot;
			bool bNeedsCleanup = false;

			// Snapshot with lock, also check for expired weak pointers and mark for cleanup
			{
				std::lock_guard Lock(Mutex);

				Snapshot.reserve(Listeners.size());
				for (const auto& Entry : Listeners)
				{
					if (!Entry.IsExpired())
					{
						Snapshot.push_back(Entry);
					}
					else
					{
						bNeedsCleanup = true;
					}
				}
			}

			// Execute callbacks without lock
			for (auto& listenerEntry : Snapshot)
			{
				if (listenerEntry.Callback)
				{
					listenerEntry.Callback(std::forward<Args>(FuncArgs)...);
				}
			}

			if (bNeedsCleanup)
			{
				RemoveExpired();
			}
		}

		void ClearAll()
		{
			std::lock_guard Lock(Mutex);
			Listeners.clear();
		}

	private:
		struct FListenerEntry
		{
			FListenerEntry(FCallback InCallback)
				: Callback(std::move(Callback))
			{
			}

			FDelegateHandle Handle{};

			FCallback Callback;

			std::weak_ptr<void> WeakObjectPtr;

			bool bIsWeak = false;

			bool IsExpired() const
			{
				return bIsWeak && WeakObjectPtr.expired();
			}
		};

		std::atomic<uint32> NextId = 0;

		std::vector<FListenerEntry> Listeners;

		std::mutex Mutex;
	};
} // namespace Doge

#define DECLARE_DELEGATE(DelegateName, ...) \
	using DelegateName = Doge::TDelegate<__VA_ARGS__>;

#define DECLARE_DELEGATE_OneParam(DelegateName, Param1Type) \
	DECLARE_DELEGATE(DelegateName, Param1Type)

#define DECLARE_DELEGATE_TwoParams(DelegateName, Param1Type, Param2Type) \
	DECLARE_DELEGATE(DelegateName, Param1Type, Param2Type)

#define DECLARE_DELEGATE_ThreeParams(DelegateName, Param1Type, Param2Type, Param3Type) \
	DECLARE_DELEGATE(DelegateName, Param1Type, Param2Type, Param3Type)