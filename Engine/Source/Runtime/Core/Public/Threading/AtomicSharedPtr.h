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

	private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
		std::atomic<std::shared_ptr<T>> Ptr;
#else
		mutable std::mutex Mutex;
		std::shared_ptr<T> Ptr;
#endif
	};
}
