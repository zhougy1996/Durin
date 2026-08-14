#include "DynamicUnloadFixtureContract.h"

#include "Threading/Task.h"

namespace Durin
{
	namespace
	{
		auto InvokeHost(auto&& Callable) -> bool
		{
			return FModularFeatureRegistry::Get()
				.InvokeSingle<Tests::IDynamicUnloadHostFeature>(
					std::forward<decltype(Callable)>(Callable))
				.Status == EFeatureInvokeStatus::Invoked;
		}

		auto RecordHostEvent(
			Tests::EDynamicUnloadFixtureEvent Phase,
			uint64 InstanceSerial) -> void
		{
			(void)InvokeHost([&](Tests::IDynamicUnloadHostFeature& Host) {
				Host.Record({Phase, InstanceSerial});
			});
		}

		struct FAsyncCaptureProbe
		{
			explicit FAsyncCaptureProbe(uint64 InInstanceSerial)
				: InstanceSerial(InInstanceSerial)
			{
			}

			~FAsyncCaptureProbe()
			{
				RecordHostEvent(
					Tests::EDynamicUnloadFixtureEvent::AsyncCaptureDestroyed,
					InstanceSerial);
			}

			uint64 InstanceSerial = 0;
		};
	}

	class FDynamicUnloadFixtureModule final
		: public IModuleInterface
		, public Tests::IDynamicUnloadFixtureFeature
	{
	public:
		~FDynamicUnloadFixtureModule() override
		{
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::ModuleDestroyed,
				InstanceSerial);
		}

			auto StartupModule(FModuleContext& Context) -> void override
		{
			const auto Serial = FModularFeatureRegistry::Get()
				.InvokeSingle<Tests::IDynamicUnloadHostFeature>(
					[](Tests::IDynamicUnloadHostFeature& Host) {
						return Host.AllocateInstanceSerial();
					});
			if (!Serial.WasInvoked() || !Serial.Value) throw std::runtime_error(
				"Dynamic unload fixture requires its process-resident host feature.");
			InstanceSerial = *Serial.Value;
			ModuleName = Context.GetModuleName();
			FeatureRegistration =
				Context.RegisterFeature<Tests::IDynamicUnloadFixtureFeature>(*this);
			OwnedCallbacks = Context.CreateOwnedCallbackRegistration(
				"DynamicUnloadFixture.SpecializedCallbacks");
			AsyncOperations = Context.CreateAsyncOperationGroup(
				"DynamicUnloadFixture.Drained",
				{.ShutdownMode = EAsyncOperationCloseMode::Drain});
			FailureOperations = Context.CreateAsyncOperationGroup(
				"DynamicUnloadFixture.Failures",
				{.ShutdownMode = EAsyncOperationCloseMode::Cancel});
			if (!FeatureRegistration.IsValid() || !OwnedCallbacks.IsValid()
				|| !AsyncOperations.IsValid() || !FailureOperations.IsValid())
				throw std::runtime_error(
					"Dynamic unload fixture failed to create its owned boundaries.");
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::Startup,
				InstanceSerial);
		}

			auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::Shutdown,
				InstanceSerial);
			if (bThrowOnShutdown) throw std::runtime_error(
				"Injected dynamic unload fixture shutdown failure.");
			Publisher = {};
			Worker = {};
			BlockingWorker = {};
			if (!bRetainResult) RetainedResult = {};
		}

		auto GetInstanceSerial() const -> uint64 override
		{
			return InstanceSerial;
		}

		auto RunSynchronousBarrier() -> void override
		{
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::SynchronousEntered,
				InstanceSerial);
			(void)InvokeHost([this](Tests::IDynamicUnloadHostFeature& Host) {
				Host.WaitForSynchronousRelease(InstanceSerial);
			});
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::SynchronousExited,
				InstanceSerial);
		}

		auto StartDrainedAsyncChain() -> bool override
		{
			if (Worker.IsValid() || Publisher.IsValid()) return false;
			auto Capture = std::make_shared<FAsyncCaptureProbe>(InstanceSerial);
			FTaskLaunchOptions WorkerOptions;
			WorkerOptions.Scope = AsyncOperations.GetTaskScope();
			WorkerOptions.CancellationToken = AsyncOperations.GetCancellationToken();
			Worker = LaunchTask<uint64>(
				"DynamicUnloadFixture.Worker",
				[Capture, Serial = InstanceSerial] {
					RecordHostEvent(
						Tests::EDynamicUnloadFixtureEvent::AsyncWorkerCompleted,
						Serial);
					return Serial;
				},
				WorkerOptions);
			if (!Worker.IsValid()) return false;

			FTaskContinuationOptions PublisherOptions;
			PublisherOptions.Scope = AsyncOperations.GetTaskScope();
			PublisherOptions.CancellationToken = AsyncOperations.GetCancellationToken();
			PublisherOptions.Target = ETaskTarget::GameThreadDeferred;
			PublisherOptions.EstimatedPayloadBytes = sizeof(uint64);
			Publisher = ThenOutcome(
				Worker,
				"DynamicUnloadFixture.Publish",
				[Capture, Serial = InstanceSerial](FTaskOutcome<uint64> Outcome) {
					if (Outcome.State == ETaskState::Succeeded
						&& Outcome.Result && *Outcome.Result == Serial)
					{
						RecordHostEvent(
							Tests::EDynamicUnloadFixtureEvent::AsyncPublished,
							Serial);
					}
				},
				PublisherOptions);
			return Publisher.IsValid();
		}

		auto RetainOwnerResourceForFailure() -> bool override
		{
			RetainedOwnerResource = OwnedCallbacks.GetGate().RetainResource();
			return static_cast<bool>(RetainedOwnerResource);
		}

		auto StartRetainedResultForFailure() -> bool override
		{
			if (RetainedResult.IsValid()) return false;
			FTaskLaunchOptions Options;
			Options.Scope = FailureOperations.GetTaskScope();
			Options.CancellationToken = FailureOperations.GetCancellationToken();
			RetainedResult = LaunchTask<uint64>(
				"DynamicUnloadFixture.RetainedResult",
				[Serial = InstanceSerial] {
					RecordHostEvent(
						Tests::EDynamicUnloadFixtureEvent::RetainedResultReady,
						Serial);
					return Serial;
				},
				Options);
			bRetainResult = RetainedResult.IsValid();
			return bRetainResult;
		}

		auto StartBlockingWorkerForFailure() -> bool override
		{
			if (BlockingWorker.IsValid()) return false;
			FTaskLaunchOptions Options;
			Options.Scope = FailureOperations.GetTaskScope();
			Options.CancellationToken = FailureOperations.GetCancellationToken();
			BlockingWorker = LaunchTask(
				"DynamicUnloadFixture.BlockingWorker",
				[Serial = InstanceSerial] {
					RecordHostEvent(
						Tests::EDynamicUnloadFixtureEvent::BlockingWorkerEntered,
						Serial);
					(void)InvokeHost(
						[Serial](Tests::IDynamicUnloadHostFeature& Host) {
							Host.WaitForAsyncRelease(Serial);
						});
				},
				Options);
			return BlockingWorker.IsValid();
		}

		auto SetThrowOnShutdownForFailure() -> void override
		{
			bThrowOnShutdown = true;
		}

		auto RequestRecursiveUnloadForFailure()
			-> EModuleOperationStatus override
		{
			return FModuleManager::Get().UnloadModule(ModuleName).Status;
		}

	private:
		uint64 InstanceSerial = 0;
		FName ModuleName;
		FModularFeatureRegistration FeatureRegistration;
		FModuleOwnedCallbackRegistration OwnedCallbacks;
		FModuleOwnedResourceLease RetainedOwnerResource;
		FAsyncOperationGroup AsyncOperations;
		FAsyncOperationGroup FailureOperations;
		TTaskHandle<uint64> Worker;
		FTaskHandle Publisher;
		TTaskHandle<uint64> RetainedResult;
		FTaskHandle BlockingWorker;
		bool bRetainResult = false;
		bool bThrowOnShutdown = false;
	};

	IMPLEMENT_MODULE(FDynamicUnloadFixtureModule, DynamicUnloadFixture)
}
