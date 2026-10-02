#include "PipelineStateCache.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include <future>
#include <thread>

namespace Durin
{
	struct FRHIPipelineStateCache::FState
	{
		using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
		struct FKeyHash
		{
			auto operator()(const FKey& Key) const -> size_t
			{
				return std::visit([](const auto& Value) -> size_t {
					if constexpr (std::same_as<std::decay_t<decltype(Value)>, FGraphicsPipelineStateKey>)
						return FGraphicsPipelineStateKeyHasher{}(Value);
					else return FComputePipelineStateKeyHasher{}(Value);
				}, Key) ^ Key.index();
			}
		};
		struct FEntry
		{
			std::weak_ptr<FPipelineState> Pipeline;
			std::shared_ptr<void> Metadata;
		};
		FState(const FRHICapabilities& InCapabilities, FRHIPipelineCreationService& InService)
			: Capabilities(InCapabilities), Service(InService) {}

		template<typename T>
		auto Request(const T& Initializer, std::string_view Name) -> FRHIPipelineCreationRequest
		{
			auto Submit = [&] {
				if constexpr (std::same_as<T, FGraphicsPipelineStateInitializer>) return Service.RequestGraphics(Initializer, Name);
				else return Service.RequestCompute(Initializer, Name);
			};
			if (!Private::FTaskRuntimeAccess::GetCurrentTaskId()) return Submit();
			// Get holds Mutex, so at most one bounded admission handoff is live.
			// The caller retains its inputs until admission returns, never until PSO completion.
			if (!AdmissionThread.joinable()) AdmissionThread = std::thread([this] {
				std::unique_lock Lock(AdmissionMutex);
				for (;;)
				{
					AdmissionChanged.wait(Lock, [&] { return StopAdmission || Admission; });
					if (StopAdmission) return;
					auto Work = std::exchange(Admission, {});
					Lock.unlock();
					Work();
					Lock.lock();
				}
			});
			auto Work = std::make_shared<std::packaged_task<FRHIPipelineCreationRequest()>>(Submit);
			auto Result = Work->get_future();
			{
				std::lock_guard Lock(AdmissionMutex);
				Admission = [Work] { (*Work)(); };
			}
			AdmissionChanged.notify_one();
			return Result.get();
		}

		template<typename T>
		auto Get(const T& Initializer, std::string_view Name)
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TPipeline = std::conditional_t<Graphics, FGraphicsPipelineState, FComputePipelineState>;
			using TResult = std::expected<std::shared_ptr<TPipeline>, ERHIPipelineRequestRejection>;
			std::lock_guard Lock(Mutex);
			if (Closed || Service.IsClosed()) return TResult(std::unexpected(ERHIPipelineRequestRejection::Closed));
			if (Private::FTaskRuntimeAccess::IsExecutingIndependentCPU())
				return TResult(std::unexpected(ERHIPipelineRequestRejection::Unsupported));
			if (!IsPipelineCreationPayloadBounded(Initializer, Name))
				return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
			try
			{
				auto Valid = [&] {
					if constexpr (Graphics) return BuildGraphicsPipelineStateKey(Initializer, &Capabilities);
					else return BuildComputePipelineStateKey(Initializer, &Capabilities);
				}();
				if (!Valid) return TResult(std::unexpected(ERHIPipelineRequestRejection::InvalidDescription));
				FKey Key(std::move(*Valid));
				if (auto Existing = Entries.find(Key); Existing != Entries.end())
				{
					if (auto Pipeline = Existing->second.Pipeline.lock())
					{
						const auto Status = Pipeline->GetState();
						if (Status == ERHIPipelineRequestState::Pending || Status == ERHIPipelineRequestState::Ready)
							return TResult(std::static_pointer_cast<TPipeline>(std::move(Pipeline)));
					}
					Entries.erase(Existing);
				}
				if (Entries.size() >= 4096)
					std::erase_if(Entries, [](const auto& Entry) { return Entry.second.Pipeline.expired(); });
				if (Entries.size() >= 4096) return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
				uint64 Bytes = sizeof(FKey) + sizeof(FEntry) + sizeof(TPipeline) + 256;
				std::visit([&](const auto& Value) {
					Bytes += Value.PipelineLayout.BindingLayouts.capacity() * sizeof(FBindingLayout);
					Bytes += Value.PipelineLayout.PushConstantRanges.capacity() * sizeof(FPushConstantRange);
					for (const auto& Layout : Value.PipelineLayout.BindingLayouts)
						Bytes += Layout.BindingLayouts.capacity() * sizeof(FBindingLayoutItem);
					if constexpr (std::same_as<std::decay_t<decltype(Value)>, FGraphicsPipelineStateKey>)
					{
						Bytes += Value.VertexElements.capacity() * sizeof(FRHIVertexElementIdentity);
						Bytes += Value.ColorBlendStates.capacity() * sizeof(FRHIColorBlendState);
					}
				}, Key);
				if (Bytes > 1024 * 1024) return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded));
				auto Metadata = [&] {
					try { return Service.ReserveCacheMetadata(Bytes); }
					catch (const FRHIRecoverableCreationError&)
					{
						// Weak keys must not consume the device budget indefinitely
						// when owners have released their identities below entry capacity.
						std::erase_if(Entries, [](const auto& Entry) { return Entry.second.Pipeline.expired(); });
						return Service.ReserveCacheMetadata(Bytes);
					}
				}();
				auto Observation = Request(Initializer, Name);
				if (!Observation.IsAccepted()) return TResult(std::unexpected(Observation.GetRejection()));
				auto Pipeline = std::shared_ptr<TPipeline>(new TPipeline(std::move(Observation), Metadata));
				Entries.emplace(std::move(Key), FEntry{Pipeline, std::move(Metadata)});
				return TResult(std::move(Pipeline));
			}
			catch (const FRHIRecoverableCreationError&) { return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded)); }
			catch (const std::bad_alloc&) { return TResult(std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded)); }
		}

		FRHICapabilities Capabilities;
		FRHIPipelineCreationService& Service;
		std::mutex Mutex;
		std::unordered_map<FKey, FEntry, FKeyHash> Entries;
		bool Closed = false;
		std::thread AdmissionThread;
		std::mutex AdmissionMutex;
		std::condition_variable AdmissionChanged;
		std::function<void()> Admission;
		bool StopAdmission = false;
	};

	FRHIPipelineStateCache::FRHIPipelineStateCache(const FRHICapabilities& Capabilities, FRHIPipelineCreationService& Service)
		: State(std::make_unique<FState>(Capabilities, Service)) {}
	FRHIPipelineStateCache::~FRHIPipelineStateCache() { Close(); }
	auto FRHIPipelineStateCache::GetGraphics(const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name)
		-> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection> { return State->Get(Initializer, Name); }
	auto FRHIPipelineStateCache::GetCompute(const FComputePipelineStateInitializer& Initializer, std::string_view Name)
		-> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection> { return State->Get(Initializer, Name); }
	auto FRHIPipelineStateCache::Close() -> void
	{
		std::lock_guard Lock(State->Mutex);
		State->Closed = true;
		{
			std::lock_guard AdmissionLock(State->AdmissionMutex);
			State->StopAdmission = true;
		}
		State->AdmissionChanged.notify_one();
		if (State->AdmissionThread.joinable()) State->AdmissionThread.join();
		State->Entries.clear();
	}

	namespace PipelineStateCache
	{
		auto GetAndOrCreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer, FName Name)
			-> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection>
		{
			if (!GDynamicRHI) return std::unexpected(ERHIPipelineRequestRejection::Unsupported);
			auto* Cache = GDynamicRHI->RHIGetPipelineStateCache();
			if (!Cache) return std::unexpected(GDynamicRHI->RHIIsPipelineCreationClosed()
				? ERHIPipelineRequestRejection::Closed : ERHIPipelineRequestRejection::Unsupported);
			return Cache->GetGraphics(Initializer, Name.ToString());
		}
		auto GetAndOrCreateComputePipelineState(const FComputePipelineStateInitializer& Initializer, FName Name)
			-> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>
		{
			if (!GDynamicRHI) return std::unexpected(ERHIPipelineRequestRejection::Unsupported);
			auto* Cache = GDynamicRHI->RHIGetPipelineStateCache();
			if (!Cache) return std::unexpected(GDynamicRHI->RHIIsPipelineCreationClosed()
				? ERHIPipelineRequestRejection::Closed : ERHIPipelineRequestRejection::Unsupported);
			return Cache->GetCompute(Initializer, Name.ToString());
		}
		auto PrecacheGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer, FName Name)
			-> std::expected<FGraphicsPipelineStateRef, ERHIPipelineRequestRejection>
		{ return GetAndOrCreateGraphicsPipelineState(Initializer, Name); }
		auto PrecacheComputePipelineState(const FComputePipelineStateInitializer& Initializer, FName Name)
			-> std::expected<FComputePipelineStateRef, ERHIPipelineRequestRejection>
		{ return GetAndOrCreateComputePipelineState(Initializer, Name); }
	}

	auto SetGraphicsPipelineState(FRHICommandListBase& Commands,
		const FGraphicsPipelineStateInitializer& Initializer, FName Name) -> void
	{
		require(GDynamicRHI);
		if (!IsTaskSchedulerRunning())
		{
			auto Native = GDynamicRHI->RHICreateGraphicsPipelineState(Name, Initializer);
			requiref(Native, "Synchronous graphics pipeline creation failed.");
			Commands.SetGraphicsPipelineState(*Native);
			return;
		}
		auto Pipeline = PipelineStateCache::GetAndOrCreateGraphicsPipelineState(Initializer, Name);
		requiref(Pipeline.has_value(), "Graphics pipeline cache admission failed.");
		Commands.SetGraphicsPipelineState(*Pipeline);
	}
	auto SetComputePipelineState(FRHICommandListBase& Commands,
		const FComputePipelineStateInitializer& Initializer, FName Name) -> void
	{
		require(GDynamicRHI);
		if (!IsTaskSchedulerRunning())
		{
			auto Native = GDynamicRHI->RHICreateComputePipelineState(Name, Initializer);
			requiref(Native, "Synchronous compute pipeline creation failed.");
			Commands.SetComputePipelineState(*Native);
			return;
		}
		auto Pipeline = PipelineStateCache::GetAndOrCreateComputePipelineState(Initializer, Name);
		requiref(Pipeline.has_value(), "Compute pipeline cache admission failed.");
		Commands.SetComputePipelineState(*Pipeline);
	}
}
