#include "RHIGlobals.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIThread.h"

namespace Durin
{
	static std::unique_ptr<FRHIThread> RHIThreadOwner;
	static const char* GOwnedBackendModuleName = nullptr;
	static FRHIReleaseResourcesDelegate RHIReleaseResourcesDelegate;

	namespace
	{
		auto UseThreadedRHIExecution() -> bool
		{
			return ResolveRHIExecutionMode(std::getenv("DURIN_RHI_EXECUTION"))
				== ERHIExecutionMode::Threaded;
		}

		auto CreateDynamicRHI(ERHIBackend Backend) -> FDynamicRHI*
		{
#if !defined(__APPLE__)
			if (Backend == ERHIBackend::Metal)
			{
				DURIN_ERROR("MetalRHI requires macOS on Apple Silicon.");
				return nullptr;
			}
#endif
			const char* ModuleName = Backend == ERHIBackend::Metal
				? "MetalRHI" : "VulkanRHI";
			IDynamicRHIModule* DynamicRHIModule =
				FModuleManager::LoadModule<IDynamicRHIModule>(ModuleName);
			if (!DynamicRHIModule)
			{
				DURIN_ERROR("Failed to load selected RHI module '{}'.", ModuleName);
				return nullptr;
			}
			GOwnedBackendModuleName = ModuleName;
			return DynamicRHIModule->CreateRHI();
		}

		auto RollbackBackendInitialization() -> void
		{
			try
			{
				GDynamicRHI->Shutdown();
			}
			catch (const std::exception& Exception)
			{
				DURIN_ERROR("Backend initialization rollback failed: {}", Exception.what());
			}
			catch (...)
			{
				DURIN_ERROR("Backend initialization rollback failed with an unknown exception.");
			}
		}

		auto InitializeBackendWithRollback(
			const FRHIInitializationContext& Context) -> FRHIThreadWorkResult
		{
			try
			{
				GDynamicRHI->Init(Context);
				return FRHIThreadWorkResult::Success();
			}
			catch (const std::exception& Exception)
			{
				RollbackBackendInitialization();
				return FRHIThreadWorkResult::FromExternalException(Exception.what());
			}
			catch (...)
			{
				RollbackBackendInitialization();
				return FRHIThreadWorkResult::Failure(ERHIThreadFailure::UnknownException);
			}
		}

		auto ReleaseFailedInitialization(bool bUnloadBackendModule) -> void
		{
			if (RHIThreadOwner)
			{
				RHIThreadOwner->Stop();
				RHIThreadOwner.reset();
			}
			delete GDynamicRHI;
			GDynamicRHI = nullptr;
			if (bUnloadBackendModule && GOwnedBackendModuleName)
			{
				if (!FModuleManager::Get().UnloadModule(GOwnedBackendModuleName))
					DURIN_ERROR("Failed to unload {} after initialization failure.",
						GOwnedBackendModuleName);
			}
			GOwnedBackendModuleName = nullptr;
		}

		auto InitializeRHI(
			FDynamicRHI* Backend,
			bool bThreaded,
			bool bForceThreadLaunchFailure,
			bool bOwnsBackendModule,
			FRHIInitializationContext Context) -> bool
		{
			if (!Backend)
			{
				DURIN_ERROR("Failed to create dynamic RHI");
				if (bOwnsBackendModule && GOwnedBackendModuleName)
				{
					if (!FModuleManager::Get().UnloadModule(GOwnedBackendModuleName))
						DURIN_ERROR("Failed to unload {} after backend creation failure.",
							GOwnedBackendModuleName);
					GOwnedBackendModuleName = nullptr;
				}
				return false;
			}
			if (GDynamicRHI || RHIThreadOwner)
			{
				DURIN_ERROR("Cannot initialize RHI more than once.");
				delete Backend;
				return false;
			}

			GDynamicRHI = Backend;
			if (bThreaded)
			{
				RHIThreadOwner = std::make_unique<FRHIThread>();
				if (bForceThreadLaunchFailure || !RHIThreadOwner->Start())
				{
					DURIN_ERROR("Failed to start RHI thread");
					ReleaseFailedInitialization(bOwnsBackendModule);
					return false;
				}
				FRHIThreadWork InitWork;
				InitWork.Execute = [Context]() {
					return InitializeBackendWithRollback(Context);
				};
				const FRHIThreadSynchronousResult InitResult =
					RHIThreadOwner->EnqueueSynchronous(InitWork);
				if (!InitResult.IsCompleted())
				{
					DURIN_ERROR(
						"Failed to initialize dynamic RHI on RHI thread: {}",
						ToString(InitResult.Error));
					ReleaseFailedInitialization(bOwnsBackendModule);
					return false;
				}
				GCommandListExecutor.SetThreadedMode(*RHIThreadOwner);
				DURIN_DEBUG("RHI execution mode: threaded");
			}
			else
			{
				const FRHIThreadWorkResult InitResult =
					InitializeBackendWithRollback(Context);
				if (!InitResult.IsSuccess())
				{
					DURIN_ERROR(
						"Failed to initialize dynamic RHI inline: {}",
						ToString(InitResult.Error));
					ReleaseFailedInitialization(bOwnsBackendModule);
					return false;
				}
				DURIN_DEBUG("RHI execution mode: inline");
			}

			// The command list exists before the backend; bind its default pipeline
			// only after the context is valid.
			FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::Graphics);
			DURIN_DEBUG("RHI initialized successfully");
			return true;
		}
	}

	auto ResolveRHIExecutionMode(const char* ConfiguredMode)
		-> ERHIExecutionMode
	{
		if (!ConfiguredMode)
		{
			return ERHIExecutionMode::Threaded;
		}

		const std::string_view Mode(ConfiguredMode);
		if (Mode == "inline")
		{
			return ERHIExecutionMode::Inline;
		}
		if (Mode != "threaded")
		{
			DURIN_ERROR(
				"Invalid DURIN_RHI_EXECUTION value '{}'; expected 'inline' or 'threaded'. Using threaded mode.",
				Mode);
		}
		return ERHIExecutionMode::Threaded;
	}

	auto ResolveRHIBackend(const char* ConfiguredBackend)
		-> std::optional<ERHIBackend>
	{
		if (!ConfiguredBackend)
		{
#if defined(__APPLE__)
			return ERHIBackend::Metal;
#else
			return ERHIBackend::Vulkan;
#endif
		}
		const std::string_view Name(ConfiguredBackend);
		if (Name == "vulkan") return ERHIBackend::Vulkan;
		if (Name == "metal") return ERHIBackend::Metal;
		DURIN_ERROR(
			"Invalid DURIN_RHI_BACKEND value '{}'; expected 'vulkan' or 'metal'.",
			Name);
		return std::nullopt;
	}

	auto GetActiveRHIBackend() -> std::optional<ERHIBackend>
	{
		if (!GDynamicRHI || !GDynamicRHI->RHIGetCapabilities() || !GOwnedBackendModuleName)
			return std::nullopt;
		return std::string_view(GOwnedBackendModuleName) == "MetalRHI"
			? ERHIBackend::Metal : ERHIBackend::Vulkan;
	}

	auto RHIInit(FRHIInitializationContext Context) -> bool
	{
		if (GDynamicRHI || RHIThreadOwner)
		{
			DURIN_ERROR("Cannot initialize RHI more than once.");
			return false;
		}
		const char* ConfiguredBackend = std::getenv("DURIN_RHI_BACKEND");
		const auto SelectedBackend = ResolveRHIBackend(ConfiguredBackend);
		if (!SelectedBackend) return false;
		const auto TryInitialize = [&](ERHIBackend Selection) {
			FDynamicRHI* Backend = CreateDynamicRHI(Selection);
			if (!Backend && !GOwnedBackendModuleName) return false;
			return InitializeRHI(Backend, UseThreadedRHIExecution(), false,
				GOwnedBackendModuleName != nullptr, Context);
		};
		if (TryInitialize(*SelectedBackend)) return true;
#if defined(__APPLE__) && DURIN_WITH_EDITOR
		// Authored editor shaders can follow the actual backend after rollback.
		// Cooked runtimes retain their explicit platform/library contract.
		if (!ConfiguredBackend && *SelectedBackend == ERHIBackend::Metal)
		{
			DURIN_WARN("Default MetalRHI initialization failed; retrying with VulkanRHI.");
			return TryInitialize(ERHIBackend::Vulkan);
		}
#endif
		return false;
	}

	auto GetRHIReleaseResourcesDelegate()
		-> FRHIReleaseResourcesDelegate&
	{
		return RHIReleaseResourcesDelegate;
	}

	auto RHIInitWithBackendForTests(
		FDynamicRHI* Backend,
		bool bThreaded,
		bool bForceThreadLaunchFailure,
		FRHIInitializationContext Context) -> bool
	{
		return InitializeRHI(
			Backend, bThreaded, bForceThreadLaunchFailure, false,
			std::move(Context));
	}

	auto RHIExit() -> void
	{
		check(GDynamicRHI);
		GDynamicRHI->RHIStopPipelineCreation();
		RHIReleaseResourcesDelegate.Broadcast();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		GDynamicRHI->RHIRetirePipelineCreationResults();
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		if (RHIThreadOwner)
		{
			FRHIThreadWork ShutdownWork;
			ShutdownWork.Execute = []() {
				RHIFlushDeferredResources();
				checkf(FRHIResource::GetNumPendingDeletes() == 0,
					"RHI shutdown marker found pending RHI resource deletions.");
				GDynamicRHI->Shutdown();
				return FRHIThreadWorkResult::Success();
			};
			const FRHIThreadSubmission ShutdownSubmission =
				RHIThreadOwner->EnqueueTerminal(ShutdownWork);
			if (!ShutdownSubmission.IsAccepted())
			{
				DURIN_FATAL(
					"Failed to atomically install the dynamic RHI shutdown marker ({}).",
					static_cast<uint32>(ShutdownSubmission.Result));
				std::terminate();
			}
			const ERHIThreadWaitResult ShutdownWaitResult =
				RHIThreadOwner->WaitForSerial(ShutdownSubmission.Serial);
			if (ShutdownWaitResult != ERHIThreadWaitResult::Completed)
			{
				DURIN_FATAL(
					"Dynamic RHI shutdown marker failed on RHI thread ({}).",
					static_cast<uint32>(ShutdownWaitResult));
				std::terminate();
			}
			GCommandListExecutor.SetInlineMode();
			RHIThreadOwner->Stop();
			const FRHIThreadStats ShutdownStats = RHIThreadOwner->GetStats();
			check(ShutdownStats.AdmissionState
				== ERHIThreadAdmissionState::Stopped);
			check(ShutdownStats.OutstandingEntryCount == 0);
			check(ShutdownStats.OutstandingBatchCount == 0);
			check(ShutdownStats.OutstandingPayloadBytes == 0);
			RHIThreadOwner.reset();
		}
		else
		{
			GDynamicRHI->Shutdown();
		}
		delete GDynamicRHI;
		GDynamicRHI = nullptr;
		if (const char* ModuleName =
			std::exchange(GOwnedBackendModuleName, nullptr))
		{
			checkf(FModuleManager::Get().UnloadModule(ModuleName),
				"The selected RHI module must unload after its backend and RHI thread are destroyed.");
		}
	}
}
