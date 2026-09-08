#include "Threading/TaskComposition.h"
#include "DynamicUnloadFixtureContract.h"

#include "Threading/Task.h"
#include "Console/ConsoleCommand.h"

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

		struct FConsoleCaptureProbe
		{
			explicit FConsoleCaptureProbe(uint64 InSerial) : InstanceSerial(InSerial) {}
			~FConsoleCaptureProbe()
			{
				RecordHostEvent(Tests::EDynamicUnloadFixtureEvent::ConsoleCaptureDestroyed,
					InstanceSerial);
			}
			uint64 InstanceSerial;
		};

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

		auto StartupModule() -> void override
		{
			const auto Serial = FModularFeatureRegistry::Get()
				.InvokeSingle<Tests::IDynamicUnloadHostFeature>(
					[](Tests::IDynamicUnloadHostFeature& Host) {
						return Host.AllocateInstanceSerial();
					});
			if (!Serial.WasInvoked() || !Serial.Value) throw std::runtime_error(
				"Dynamic unload fixture requires its process-resident host feature.");
			InstanceSerial = *Serial.Value;
			ModuleName = FModuleStartup::GetModuleName();
			FeatureRegistration =
				FModuleStartup::RegisterFeature<Tests::IDynamicUnloadFixtureFeature>(*this);

			AsyncOperations = FModuleStartup::CreateAsyncOperationGroup(
				"DynamicUnloadFixture.Drained",
				{.ShutdownMode = EAsyncOperationCloseMode::Drain});
			FailureOperations = FModuleStartup::CreateAsyncOperationGroup(
				"DynamicUnloadFixture.Failures",
				{.ShutdownMode = EAsyncOperationCloseMode::Cancel});
			if (!FeatureRegistration.IsValid()
				|| !AsyncOperations.IsValid() || !FailureOperations.IsValid())
				throw std::runtime_error(
					"Dynamic unload fixture failed to create its owned boundaries.");
			ConsoleCommand = FConsoleCommandRegistry::Get().RegisterCommand({
				std::format("fixture.{}", InstanceSerial), "Fixture lifetime probe", "",
				[Probe = std::make_shared<FConsoleCaptureProbe>(InstanceSerial)](auto) {
					return FConsoleCommandResult::Success(std::to_string(Probe->InstanceSerial));
				}});
			require(ConsoleCommand != 0);
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::Startup,
				InstanceSerial);
		}

		auto ShutdownModule() -> void override
		{
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::Shutdown,
				InstanceSerial);
			if (bThrowOnShutdown) throw std::runtime_error(
				"Injected dynamic unload fixture shutdown failure.");
			FConsoleCommandRegistry::Get().UnregisterCommand(ConsoleCommand);
			ConsoleCommand = 0;
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
			Tasks::FTaskExecutionOptions WorkerOptions;
			WorkerOptions.Scope = AsyncOperations.GetTaskScope();
			WorkerOptions.Cancellation = AsyncOperations.GetCancellationToken();
			Worker = Tasks::Share(Tasks::LaunchTask(
				"DynamicUnloadFixture.Worker",
				[Capture, Serial = InstanceSerial] {
					RecordHostEvent(
						Tests::EDynamicUnloadFixtureEvent::AsyncWorkerCompleted,
						Serial);
					return Serial;
				},
				WorkerOptions));
			if (!Worker.IsValid()) return false;

			Tasks::FTaskExecutionOptions PublisherOptions;
			PublisherOptions.Scope = AsyncOperations.GetTaskScope();
			PublisherOptions.Cancellation = AsyncOperations.GetCancellationToken();
			PublisherOptions.DebugName = "DynamicUnloadFixture.Publish";
			Publisher = Tasks::ThenCompleted(Worker, Tasks::ETaskExecutor::GameThreadDeferred, PublisherOptions,
				[Capture, Serial = InstanceSerial](const Tasks::TSharedTask<uint64>& Outcome) {
					if (Outcome.GetState() == ETaskState::Succeeded
						&& Outcome.GetResult() == Serial)
					{
						RecordHostEvent(
							Tests::EDynamicUnloadFixtureEvent::AsyncPublished,
							Serial);
					}
				}).GetCompletion().GetTaskHandle();
			return Publisher.IsValid();
		}

		auto StartRetainedResultForFailure() -> bool override
		{
			if (RetainedResult.IsValid()) return false;
			Tasks::FTaskExecutionOptions Options;
			Options.Scope = FailureOperations.GetTaskScope();
			Options.Cancellation = FailureOperations.GetCancellationToken();
			RetainedResult = Tasks::Share(Tasks::LaunchTask(
				"DynamicUnloadFixture.RetainedResult",
				[Serial = InstanceSerial] {
					RecordHostEvent(
						Tests::EDynamicUnloadFixtureEvent::RetainedResultReady,
						Serial);
					return Serial;
				},
				Options));
			bRetainResult = RetainedResult.IsValid();
			return bRetainResult;
		}

		auto StartBlockingWorkerForFailure() -> bool override
		{
			if (BlockingWorker.IsValid()) return false;
			Tasks::FTaskExecutionOptions Options;
			Options.Scope = FailureOperations.GetTaskScope();
			Options.Cancellation = FailureOperations.GetCancellationToken();
			BlockingWorker = Tasks::LaunchTask(
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
				Options).GetCompletion().GetTaskHandle();
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
		FConsoleCommandHandle ConsoleCommand = 0;
		FName ModuleName;
		FModularFeatureRegistration FeatureRegistration;

		FAsyncOperationGroup AsyncOperations;
		FAsyncOperationGroup FailureOperations;
		Tasks::TSharedTask<uint64> Worker;
		FTaskHandle Publisher;
		Tasks::TSharedTask<uint64> RetainedResult;
		FTaskHandle BlockingWorker;
		bool bRetainResult = false;
		bool bThrowOnShutdown = false;
	};

	IMPLEMENT_MODULE(FDynamicUnloadFixtureModule, DynamicUnloadFixture)
}
