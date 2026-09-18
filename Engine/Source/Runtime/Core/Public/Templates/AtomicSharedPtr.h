#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace Durin
{
	// Publishes owning pointer snapshots across threads; does not synchronize the pointee.
	// The fallback guarantees per-instance synchronization, not a global seq_cst order.
	template<typename T>
	class TAtomicSharedPtr
	{
	public:
		TAtomicSharedPtr() = default;
		explicit TAtomicSharedPtr(std::shared_ptr<T> Value) : Ptr(std::move(Value)) {}

		TAtomicSharedPtr(const TAtomicSharedPtr&) = delete;
		auto operator=(const TAtomicSharedPtr&) -> TAtomicSharedPtr& = delete;

		auto Load() const -> std::shared_ptr<T>
		{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
			return Ptr.load();
#else
			std::lock_guard Lock(Mutex);
			return Ptr;
#endif
		}

		auto Store(std::shared_ptr<T> Value) -> void
		{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
			Ptr.store(std::move(Value));
#else
			{
				std::lock_guard Lock(Mutex);
				Ptr.swap(Value);
			}
			// Release the previous owner outside the lock: destruction may reenter.
#endif
		}

		auto Exchange(std::shared_ptr<T> Value) -> std::shared_ptr<T>
		{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
			return Ptr.exchange(std::move(Value));
#else
			{
				std::lock_guard Lock(Mutex);
				Ptr.swap(Value);
			}
			return Value;
#endif
		}

		// Strong CAS: failure refreshes Expected; equivalence includes shared ownership.
		auto CompareExchange(std::shared_ptr<T>& Expected, std::shared_ptr<T> Desired) -> bool
		{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
			return Ptr.compare_exchange_strong(Expected, std::move(Desired));
#else
			std::shared_ptr<T> Actual;
			bool Exchanged;
			{
				std::lock_guard Lock(Mutex);
				Exchanged = Ptr == Expected && !Ptr.owner_before(Expected) && !Expected.owner_before(Ptr);
				if (Exchanged) Ptr.swap(Desired);
				else Actual = Ptr;
			}
			// Releasing either replaced owner may reenter this instance.
			if (!Exchanged) Expected = std::move(Actual);
			return Exchanged;
#endif
		}

	private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
		std::atomic<std::shared_ptr<T>> Ptr;
#else
		mutable std::mutex Mutex;
		std::shared_ptr<T> Ptr;
#endif
	};
}
