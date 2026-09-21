#pragma once

#include "Misc/AssertionMacros.h"

#include <concepts>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace Durin::Private
{
	template<typename Signature>
	class TMoveOnlyFunction;

	template<typename R, typename... Args>
	class TMoveOnlyFunction<R(Args...)>
	{
	public:
		TMoveOnlyFunction() = default;
		TMoveOnlyFunction(std::nullptr_t) {}

		template<typename F>
		requires (!std::same_as<std::decay_t<F>, TMoveOnlyFunction>
			&& std::is_move_constructible_v<std::decay_t<F>>
			&& std::is_destructible_v<std::decay_t<F>>
			&& std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
		TMoveOnlyFunction(F&& Function)
		{
			using FTarget = std::decay_t<F>;
			if constexpr (CanStoreInline<FTarget>)
			{
				Object = new (Storage) FTarget(std::forward<F>(Function));
				bInline = true;
			}
			else
			{
				Object = new FTarget(std::forward<F>(Function));
			}
			Operations = &GetOperations<FTarget>();
		}

		TMoveOnlyFunction(const TMoveOnlyFunction&) = delete;
		auto operator=(const TMoveOnlyFunction&) -> TMoveOnlyFunction& = delete;

		TMoveOnlyFunction(TMoveOnlyFunction&& Other) noexcept
		{
			MoveFrom(std::move(Other));
		}

		auto operator=(TMoveOnlyFunction&& Other) noexcept -> TMoveOnlyFunction&
		{
			if (this == &Other) return *this;
			Reset();
			MoveFrom(std::move(Other));
			return *this;
		}

		auto operator=(std::nullptr_t) noexcept -> TMoveOnlyFunction&
		{
			Reset();
			return *this;
		}

		~TMoveOnlyFunction()
		{
			Reset();
		}

		explicit operator bool() const noexcept
		{
			return Operations != nullptr;
		}

		auto operator()(Args... Arguments) -> R
		{
			check(Operations);
			if constexpr (std::is_void_v<R>)
			{
				Operations->Invoke(Object, std::forward<Args>(Arguments)...);
			}
			else
			{
				return Operations->Invoke(Object, std::forward<Args>(Arguments)...);
			}
		}

		auto Reset() noexcept -> void
		{
			if (!Operations) return;
			Operations->Destroy(Object, bInline);
			Object = nullptr;
			Operations = nullptr;
			bInline = false;
		}

	private:
		static constexpr size_t InlineSize = sizeof(void*) * 3;

		struct FOperations
		{
			auto (*Invoke)(void*, Args&&...) -> R;
			void (*Destroy)(void*, bool) noexcept;
			void (*MoveInline)(void*, void*) noexcept;
		};

		template<typename F>
		static constexpr bool CanStoreInline = sizeof(F) <= InlineSize
			&& alignof(F) <= alignof(std::max_align_t)
			&& std::is_nothrow_move_constructible_v<F>;

		template<typename F>
		static auto GetOperations() -> const FOperations&
		{
			static const FOperations OperationsForType{
				.Invoke = [](void* Target, Args&&... Arguments) -> R {
					if constexpr (std::is_void_v<R>)
					{
						std::invoke(*static_cast<F*>(Target), std::forward<Args>(Arguments)...);
					}
					else
					{
						return std::invoke(*static_cast<F*>(Target), std::forward<Args>(Arguments)...);
					}
				},
				.Destroy = [](void* Target, bool bTargetInline) noexcept {
					if (bTargetInline) static_cast<F*>(Target)->~F();
					else delete static_cast<F*>(Target);
				},
				.MoveInline = [](void* Source, void* Destination) noexcept {
					new (Destination) F(std::move(*static_cast<F*>(Source)));
					static_cast<F*>(Source)->~F();
				},
			};
			return OperationsForType;
		}

		auto MoveFrom(TMoveOnlyFunction&& Other) noexcept -> void
		{
			if (!Other.Operations) return;
			Operations = Other.Operations;
			bInline = Other.bInline;
			if (bInline)
			{
				Operations->MoveInline(Other.Object, Storage);
				Object = Storage;
			}
			else
			{
				Object = Other.Object;
			}
			Other.Object = nullptr;
			Other.Operations = nullptr;
			Other.bInline = false;
		}

		alignas(std::max_align_t) std::byte Storage[InlineSize]{};
		void* Object = nullptr;
		const FOperations* Operations = nullptr;
		bool bInline = false;
	};
}
