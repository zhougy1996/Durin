#pragma once

// Template implementation for RDG.h; include RDG/RDG.h when authoring graphs.
namespace Durin
{
	template<typename T, typename... Args>
	requires std::constructible_from<T, Args...> && std::destructible<T>
	auto FRDGBuilder::CreateValue(std::string_view Name, std::string_view StableTypeName,
		Args&&... ConstructorArgs) -> TRDGValueHandle<T>
	{
		RequireBuilding();
		static_assert(std::is_object_v<T> && !std::is_const_v<T>
			&& !std::is_volatile_v<T>,
			"Render graph values require an unqualified object type");
		uint32 Index = 0;
		void* Storage = AllocateValueStorage(Name, StableTypeName,
			&RDGPrivate::GValueTypeIdentity<T>, sizeof(T), alignof(T),
			[](void* Value) { std::destroy_at(static_cast<T*>(Value)); }, Index);
		if (Storage == nullptr) return {};
		FStorageConstructionScope Construction(*this);
		std::construct_at(static_cast<T*>(Storage),
			std::forward<Args>(ConstructorArgs)...);
		MarkValueStorageConstructed(Index);
		return {StateOwner(), Index};
	}

	template<typename ParameterStruct>
	requires CRDGParameters<ParameterStruct>
	auto FRDGBuilder::AllocParameters() -> TRDGParametersRef<ParameterStruct>
	{
		RequireBuilding();
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		static_assert(std::default_initializable<ParameterStruct>,
			"Render graph parameter structs must be default constructible");
		static_assert(std::destructible<ParameterStruct>,
			"Render graph parameter structs must be destructible");
		std::weak_ptr<void> Lifetime;
		size_t AllocationIndex = TRDGParametersRef<ParameterStruct>::InvalidAllocationIndex;
		const auto& LayoutResult =
			GetRDGParameterLayoutBuildResult<ParameterStruct>();
		void* Storage = AllocateParameterStorage(sizeof(ParameterStruct),
			alignof(ParameterStruct),
			ParameterStruct::GetRDGParametersMetadata(),
			LayoutResult,
			[](void* Value) { std::destroy_at(
				static_cast<ParameterStruct*>(Value)); }, Lifetime, AllocationIndex);
		if (Storage == nullptr) return {};
		FStorageConstructionScope Construction(*this);
		auto* Parameters = std::construct_at(
			static_cast<ParameterStruct*>(Storage));
		MarkParameterStorageConstructed(AllocationIndex);
		return {Parameters, std::move(Lifetime), LayoutResult->get(), AllocationIndex};
	}

	template<typename ParameterStruct, typename ExecuteFunction>
	requires CRDGParameters<ParameterStruct>
		&& std::invocable<ExecuteFunction&, FRHICommandListImmediate&,
			const ParameterStruct&, const FRDGParameterResolver&>
	auto FRDGBuilder::AddPass(std::string_view Name, ERDGPassType Type,
		TRDGParametersRef<ParameterStruct>&& Parameters,
		ExecuteFunction&& ExecuteCallback) -> FRDGPassHandle
	{
		RequireBuilding();
		ParameterStruct* TypedData = Parameters.Data;
		FRDGParameterizedPassExecute ErasedExecute =
			[TypedData, Callback = std::forward<ExecuteFunction>(ExecuteCallback)](
				FRHICommandListImmediate& CommandList,
				const FRDGParameterResolver& Resolver) mutable {
				std::invoke(Callback, CommandList,
					static_cast<const ParameterStruct&>(*TypedData), Resolver);
			};
		RequireBuilding();
		auto Lifetime = Parameters.Lifetime.lock();
		void* Data = std::exchange(Parameters.Data, nullptr);
		const FRDGParameterLayout* Layout =
			std::exchange(Parameters.Layout, nullptr);
		if (Layout == nullptr)
			Layout = GetRDGParameterLayout<ParameterStruct>();
		Parameters.Lifetime.reset();
		const size_t AllocationIndex = std::exchange(Parameters.AllocationIndex,
			TRDGParametersRef<ParameterStruct>::InvalidAllocationIndex);
		return AddParameterizedPass(Name, Type,
			Layout, Data, AllocationIndex,
			std::move(Lifetime), std::move(ErasedExecute));
	}

	template<typename ParameterStruct, typename ExecuteFunction>
	requires CRDGParameters<ParameterStruct>
		&& std::invocable<ExecuteFunction&, FRHICommandList&, const ParameterStruct&, const FRDGParameterResolver&>
	auto FRDGBuilder::AddRecordingPass(std::string_view Name, ERDGPassType Type,
		TRDGParametersRef<ParameterStruct>&& Parameters, ExecuteFunction&& ExecuteCallback,
		ERDGRecordingPolicy Policy) -> FRDGPassHandle
	{
		RequireBuilding();
		ParameterStruct* TypedData = Parameters.Data;
		FRDGRecordingPassExecute ErasedExecute =
			[TypedData, Callback = std::forward<ExecuteFunction>(ExecuteCallback)](
				FRHICommandList& CommandList, const FRDGParameterResolver& Resolver) mutable {
				std::invoke(Callback, CommandList, static_cast<const ParameterStruct&>(*TypedData), Resolver);
			};
		auto Lifetime = Parameters.Lifetime.lock();
		void* Data = std::exchange(Parameters.Data, nullptr);
		const auto* Layout = std::exchange(Parameters.Layout, nullptr);
		if (!Layout) Layout = GetRDGParameterLayout<ParameterStruct>();
		Parameters.Lifetime.reset();
		const size_t AllocationIndex = std::exchange(Parameters.AllocationIndex,
			TRDGParametersRef<ParameterStruct>::InvalidAllocationIndex);
		return AddParameterizedPass(Name, Type, Layout, Data, AllocationIndex,
			std::move(Lifetime), {}, std::move(ErasedExecute), Policy);
	}

	template<typename ParameterStruct>
	requires CRDGParameters<ParameterStruct>
	auto FRDGBuilder::AddTestPass(std::string_view Name, ERDGPassType Type,
		TRDGParametersRef<ParameterStruct>&& Parameters,
		FRDGPassExecute Execute) -> FRDGPassHandle
	{
		return AddPass(Name, Type, std::move(Parameters),
			[Callback = std::move(Execute)](FRHICommandListImmediate& CommandList,
				const ParameterStruct&, const FRDGParameterResolver& Resolver) {
				if (Callback) Callback(CommandList, Resolver.Resources);
			});
	}

	template<typename T>
	auto FRDGBuilder::UseValue(FRDGPassHandle Pass,
		TRDGValueHandle<T> Value, ERDGUse Use) -> void
	{
		UseValueErased(Pass, Value.Owner, Value.Index,
			&RDGPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, Use);
	}
} // namespace Durin
