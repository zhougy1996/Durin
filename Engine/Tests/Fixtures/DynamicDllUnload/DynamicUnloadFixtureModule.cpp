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
			FAsyncCaptureProbe(uint64 InInstanceSerial, std::function<void(Tests::FDynamicUnloadFixtureEvent)> InRecord)
				: InstanceSerial(InInstanceSerial), Record(std::move(InRecord))
			{
			}

			~FAsyncCaptureProbe()
			{
				Record({Tests::EDynamicUnloadFixtureEvent::AsyncCaptureDestroyed, InstanceSerial});
			}

			uint64 InstanceSerial = 0;
			std::function<void(Tests::FDynamicUnloadFixtureEvent)> Record;
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
			ModuleName = FModuleStartup::GetModuleName();
			if (ModuleName == FName("DynamicUnloadFixtureResidentStartupFailure"))
			{
				bDynamicReloading = false;
				throw std::runtime_error("Injected process-resident startup failure.");
			}
			const auto Serial = FModularFeatureRegistry::Get()
				.InvokeSingle<Tests::IDynamicUnloadHostFeature>(
					[](Tests::IDynamicUnloadHostFeature& Host) {
						return Host.AllocateInstanceSerial();
					});
			if (!Serial.WasInvoked() || !Serial.Value) throw std::runtime_error(
				"Dynamic unload fixture requires its process-resident host feature.");
			InstanceSerial = *Serial.Value;
			require(InvokeHost([this](auto& Host) { WorkerCallbacks = Host.GetWorkerCallbacks(); }));
			FeatureRegistration =
				FModularFeatureRegistry::Get().RegisterFeature<Tests::IDynamicUnloadFixtureFeature>(*this);

			if (!FeatureRegistration.IsValid())
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

		auto SupportsDynamicReloading() const -> bool override { return bDynamicReloading; }

		auto ShutdownModule() -> void override
		{
			FeatureRegistration.Reset();
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::Shutdown,
				InstanceSerial);
			if (bThrowOnShutdown) throw std::runtime_error(
				"Injected dynamic unload fixture shutdown failure.");
			FConsoleCommandRegistry::Get().UnregisterCommand(ConsoleCommand);
			ConsoleCommand = 0;
			if (bRetainResult || (BlockingWorker.IsValid() && !BlockingWorker.IsCompleted()))
				throw std::runtime_error("Fixture asynchronous cleanup failed");
			if (Worker.IsValid()) require(Worker.Wait().WaitStatus == ETaskWaitStatus::Completed);
			if (Publisher.IsValid()) require(WaitTask(Publisher).WaitStatus == ETaskWaitStatus::Completed);
			Publisher = {};
			Worker = {};
			BlockingWorker = {};
			if (!bRetainResult) RetainedResult = {};
		}

		auto GetInstanceSerial() const -> uint64 override
		{
			return InstanceSerial;
		}

		auto RunSynchronousCall() -> void override
		{
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::SynchronousEntered,
				InstanceSerial);
			RecordHostEvent(
				Tests::EDynamicUnloadFixtureEvent::SynchronousExited,
				InstanceSerial);
		}

		auto StartDrainedAsyncChain() -> bool override
		{
			if (Worker.IsValid() || Publisher.IsValid()) return false;
			auto Capture = std::make_shared<FAsyncCaptureProbe>(InstanceSerial, WorkerCallbacks.Record);
			Tasks::FTaskExecutionOptions WorkerOptions;
			Worker = Tasks::LaunchTask(
				"DynamicUnloadFixture.Worker",
				[Capture, Callbacks = WorkerCallbacks, Serial = InstanceSerial] {
					Callbacks.Record({Tests::EDynamicUnloadFixtureEvent::AsyncWorkerCompleted, Serial});
					return Serial;
				},
				WorkerOptions);
			if (!Worker.IsValid()) return false;

			Tasks::FTaskExecutionOptions PublisherOptions;
			PublisherOptions.DebugName = "DynamicUnloadFixture.Publish";
			Publisher = Durin::Tasks::FTask(Tasks::ThenCompleted(Worker, Tasks::ETaskExecutor::Worker, PublisherOptions,
				[Capture, Callbacks = WorkerCallbacks, Serial = InstanceSerial](const Tasks::TTask<uint64>& Outcome) {
					if (Outcome.GetState() == ETaskState::Succeeded
						&& Outcome.GetResult() == Serial)
					{
						Callbacks.Record({Tests::EDynamicUnloadFixtureEvent::AsyncPublished, Serial});
					}
				}));
			return Publisher.IsValid();
		}

		auto StartRetainedResultForFailure() -> bool override
		{
			if (RetainedResult.IsValid()) return false;
			Tasks::FTaskExecutionOptions Options;
			RetainedResult = Tasks::LaunchTask(
				"DynamicUnloadFixture.RetainedResult",
				[Callbacks = WorkerCallbacks, Serial = InstanceSerial] {
					Callbacks.Record({Tests::EDynamicUnloadFixtureEvent::RetainedResultReady, Serial});
					return Serial;
				},
				Options);
			bRetainResult = RetainedResult.IsValid();
			return bRetainResult;
		}

		auto StartBlockingWorkerForFailure() -> bool override
		{
			if (BlockingWorker.IsValid()) return false;
			Tasks::FTaskExecutionOptions Options;
			BlockingWorker = Durin::Tasks::FTask(Tasks::LaunchTask(
				"DynamicUnloadFixture.BlockingWorker",
				[Callbacks = WorkerCallbacks, Serial = InstanceSerial] {
					Callbacks.Record({Tests::EDynamicUnloadFixtureEvent::BlockingWorkerEntered, Serial});
					Callbacks.WaitForAsyncRelease(Serial);
				},
				Options));
			return BlockingWorker.IsValid();
		}

		auto SetThrowOnShutdownForFailure() -> void override
		{
			bThrowOnShutdown = true;
		}

		auto RequestRecursiveUnloadForFailure() -> bool override
		{
			return FModuleManager::Get().UnloadModule(ModuleName);
		}

	private:
		bool bDynamicReloading = true;
		Tests::FDynamicUnloadWorkerCallbacks WorkerCallbacks;
		uint64 InstanceSerial = 0;
		FConsoleCommandHandle ConsoleCommand = 0;
		FName ModuleName;
		FModularFeatureRegistration FeatureRegistration;

		Tasks::TTask<uint64> Worker;
		Tasks::FTask Publisher;
		Tasks::TTask<uint64> RetainedResult;
		Tasks::FTask BlockingWorker;
		bool bRetainResult = false;
		bool bThrowOnShutdown = false;
	};

	IMPLEMENT_MODULE(FDynamicUnloadFixtureModule, DynamicUnloadFixture)
}
