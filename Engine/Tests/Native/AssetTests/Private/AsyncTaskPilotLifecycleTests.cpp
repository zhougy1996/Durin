#include "NativeAssetBuildTestSupport.h"
#include "Threading/TaskComposition.h"
#include <gtest/gtest.h>
#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/Texture2DCompilationTestSupport.h"
#include "DerivedDataCache/DerivedDataCache.h"
#include "Texture/TextureCube.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshCompilationTestSupport.h"
#include "../../EngineTests/Private/DerivedDataBuildTestSupport.h"

namespace
{
	using namespace Durin;
	class FHeldDerivedDataCache final : public DerivedData::ICache
	{
	public:
		mutable std::atomic<uint32> Reads = 0;
		mutable std::atomic<uint32> Writes = 0;
		mutable FThreadEvent Release;
		mutable FThreadEvent ReleaseWrites;
		bool bHoldWrites = false;
		bool bRetainRecords = false;
		mutable std::mutex RecordMutex;
		mutable std::unordered_map<std::string, DerivedData::FCacheRecord> Records;
		auto Get(const DerivedData::FCacheGetRequest& Request) const -> DerivedData::FCacheGetResult override
		{
			++Reads;
			// Model an uninterruptible physical read; cancellation must retain its owner.
			Release.WaitFor(10.0);
			std::lock_guard Lock(RecordMutex);
			if (const auto It = Records.find(Request.Key.ToString()); It != Records.end()) return std::optional{It->second};
			return std::optional<DerivedData::FCacheRecord>{};
		}
		auto Put(const DerivedData::FCachePutRequest& Request) const -> DerivedData::FCachePutResult override
		{
			++Writes;
			if (bHoldWrites) ReleaseWrites.WaitFor(10.0);
			if (bRetainRecords) { std::lock_guard Lock(RecordMutex); Records.insert_or_assign(Request.Record.GetKey().ToString(), Request.Record); }
			return {};
		}
	};

	// This guard must be declared after every local captured by an owner callback.
	struct FScopedOwnerDrain
	{
		std::vector<std::shared_ptr<FHeldDerivedDataCache>> Caches;
		std::vector<FThreadEvent*> Gates;
		bool bShutdown = false;
		std::function<void()> Restore;
		~FScopedOwnerDrain()
		{
			for (const auto& Cache : Caches) { Cache->Release.Trigger(); Cache->ReleaseWrites.Trigger(); }
			for (auto* Gate : Gates) Gate->Trigger();
			if (bShutdown) ShutdownAssetCompilingManager();
			else FAssetCompilingManager::Get().FinishAllCompilation();
			if (Restore) Restore();
		}
	};
	auto RestoreTextureBridgeTests() -> void
	{
		AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
		AssetPrivate::SetTextureAsyncCacheForTests({});
	}
	auto RestoreMeshBridgeTests() -> void
	{
		AssetPrivate::SetStaticMeshCompilationPhaseHookForTests({});
		AssetPrivate::SetStaticMeshBuildCompletionHookForTests({});
		AssetPrivate::SetStaticMeshAsyncCacheForTests({});
	}
	template<typename F>
	auto WaitUntil(F&& Ready, std::chrono::seconds Timeout = std::chrono::seconds(5), bool bPumpOwners = false) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while (!Ready())
		{
			if (std::chrono::steady_clock::now() >= Deadline) return false;
			if (bPumpOwners) FAssetCompilingManager::Get().ProcessAsyncTasks();
			else std::this_thread::yield();
		}
		return true;
	}

	auto MakeBridgeMeshSource() -> FStaticMeshSource
	{
		FMeshDescription Geometry;
		Geometry.PolygonGroups.push_back({"Material", 0, "Material"});
		auto& Section = Geometry.Sections.emplace_back();
		Section.Name = "Bridge";
		Section.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
		Section.Indices = {0, 1, 2};
		FStaticMeshSource Source;
		require(Source.Initialize(std::move(Geometry)).has_value());
		Source.ReleaseGeometry();
		return Source;
	}
	auto SubmitBridgeMesh(DStaticMesh& Mesh, FStaticMeshSource Source,
		FStaticMeshCompilationCompletion Completion = {}) -> std::expected<void, std::vector<std::string>>
	{
		return Mesh.BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = std::move(Source),
			.PreparedMaterialSlots = std::vector<FMeshMaterialSlotDefinition>{{.Name = FName("Material"), .SourceName = "Material"}}},
			std::move(Completion));
	}
	auto ExerciseStaticMeshOwnerBridge() -> void
	{
		auto Cache = std::make_shared<FHeldDerivedDataCache>();
		Cache->bHoldWrites = true;
		Cache->bRetainRecords = true;
		AssetPrivate::SetStaticMeshAsyncCacheForTests(Cache);
		FThreadEvent MailboxEntered, ReleaseMailbox, CallbackEntered, ReleaseCallback;
		AssetPrivate::SetStaticMeshBuildCompletionHookForTests([&] {
			CallbackEntered.Trigger(); ReleaseCallback.WaitFor(10.0);
		});
		AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([&](uint64, EStaticMeshCompilationPhase Phase) {
			if (Phase == EStaticMeshCompilationPhase::Mailbox) {
				EXPECT_FALSE(IsInGameThread()); MailboxEntered.Trigger(); ReleaseMailbox.WaitFor(10.0);
			}
		});
		std::array<DStaticMesh*, 3> Meshes{};
		std::array<uint32, 3> Completed{};
		FScopedOwnerDrain Drain{.Caches = {Cache}, .Gates = {&ReleaseMailbox, &ReleaseCallback}, .Restore = RestoreMeshBridgeTests};
		auto Source = MakeBridgeMeshSource();
		auto Probe = Testing::AttachBuildSourceReadProbe(Source, Source.GetGeometryBulk(), "Geometry");
		ASSERT_TRUE(Probe);
		for (uint32 Index = 0; Index < Meshes.size(); ++Index)
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/MeshBridge" + std::to_string(Index), Path));
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Meshes[Index]));
			ASSERT_TRUE(SubmitBridgeMesh(*Meshes[Index], Source, [&, Index](const auto& Result) {
				EXPECT_TRUE(IsInGameThread()); ++Completed[Index];
				EXPECT_EQ(Index == 0 ? EStaticMeshCompilationStatus::Cancelled : EStaticMeshCompilationStatus::Succeeded, Result.Status);
			}));
		}
		ASSERT_TRUE(WaitUntil([&] { return Cache->Reads.load() >= 2; }, std::chrono::seconds(5)));
		ASSERT_EQ(2u, Cache->Reads.load());
		const auto Held = GetStaticMeshCompilationManagerDiagnostics();
		EXPECT_EQ(2u, Held.AdmittedBuildCount);
		EXPECT_EQ(3u, Held.OutstandingRecords);
		EXPECT_GT(Held.ReservedBytes, 0u);
		auto Marker = Tasks::LaunchTask("MeshOwnerReleasedWorkers", [] {});
		ASSERT_TRUE(WaitUntil([&] { return Marker.GetCompletion().IsReady(); }, std::chrono::seconds(2)));
		ASSERT_TRUE(Marker.GetCompletion().IsReady());
		EXPECT_EQ(ETaskState::Succeeded, Marker.GetState());
		CancelStaticMeshCompilation(*Meshes[0]);
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_EQ(Held.ReservedBytes, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
		EXPECT_EQ(3u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
		Cache->Release.Trigger();
		ASSERT_TRUE(WaitUntil([&] { return Cache->Writes.load() != 0; }, std::chrono::seconds(5)));
		ASSERT_EQ(1u, Cache->Writes.load());
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
		EXPECT_EQ(0u, Completed[1]);
		Cache->ReleaseWrites.Trigger();
		ASSERT_TRUE(CallbackEntered.WaitFor(5.0));
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_FALSE(MailboxEntered.WaitFor(0.01));
		EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
		EXPECT_EQ(0u, Completed[1]);
		EXPECT_GT(GetStaticMeshCompilationManagerDiagnostics().ReservedBytes, 0u);
		ReleaseCallback.Trigger();
		ASSERT_TRUE(WaitUntil([&] { return MailboxEntered.WaitFor(0.001); }, std::chrono::seconds(5), true));
		ASSERT_TRUE(MailboxEntered.WaitFor(0.01));
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
		EXPECT_EQ(0u, Completed[1]);
		ReleaseMailbox.Trigger();
		FinishStaticMeshCompilation(*Meshes[1], EStaticMeshCompilationProducts::Render);
		EXPECT_NE(nullptr, Meshes[1]->GetRenderData());
		EXPECT_EQ(nullptr, Meshes[2]->GetRenderData());
		EXPECT_EQ(0u, Completed[2]);
		FAssetCompilingManager::Get().FinishAllCompilation();
		EXPECT_EQ((std::array<uint32, 3>{1, 1, 1}), Completed);
		EXPECT_EQ(nullptr, Meshes[0]->GetRenderData());
		EXPECT_NE(nullptr, Meshes[2]->GetRenderData());
		const auto SourceReads = Probe->GetReadStats().RequestCount;
		const auto Writes = Cache->Writes.load();
		ASSERT_TRUE(Meshes[1]->Build(EStaticMeshBuildMode::Asynchronous));
		FinishStaticMeshCompilation(*Meshes[1], EStaticMeshCompilationProducts::Render);
		EXPECT_EQ(SourceReads, Probe->GetReadStats().RequestCount);
		EXPECT_EQ(Writes, Cache->Writes.load());
		Cache->Release.Reset();
		const auto SupersedeReads = Cache->Reads.load();
		ASSERT_TRUE(Meshes[1]->Build(EStaticMeshBuildMode::Asynchronous));
		const auto FirstId = GetStaticMeshCompilationDiagnostic(*Meshes[1]).RequestId;
		ASSERT_TRUE(WaitUntil([&] { return Cache->Reads.load() != SupersedeReads; }, std::chrono::seconds(5)));
		ASSERT_GT(Cache->Reads.load(), SupersedeReads);
		ASSERT_TRUE(Meshes[1]->Build(EStaticMeshBuildMode::Asynchronous));
		Cache->Release.Trigger();
		FinishStaticMeshCompilation(*Meshes[1], EStaticMeshCompilationProducts::Render);
		EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, GetStaticMeshCompilationDiagnostic(FirstId).Status);
		EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(*Meshes[1]).Status);
		FAssetCompilingManager::Get().FinishAllCompilation();
		EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	}

	auto ExerciseCubeOwnerBridge() -> void
	{
		auto Cache = std::make_shared<FHeldDerivedDataCache>();
		Cache->bHoldWrites = true;
		Cache->bRetainRecords = true;
		std::shared_ptr<Testing::FBuildSourceReadProbe> HDRProbe;
		FThreadEvent CallbackEntered, ReleaseCallback;
		AssetPrivate::SetTextureAsyncCacheForTests(Cache);
		AssetPrivate::SetTexture2DCompilationPhaseHookForTests([&](uint64, ETexture2DCompilationPhase Phase) {
			if (Phase == ETexture2DCompilationPhase::AwaitingApply) {
				CallbackEntered.Trigger();
				ReleaseCallback.WaitFor(10.0);
			}
		});
		std::array<DTextureCube*, 3> Cubes{};
		FScopedOwnerDrain Drain{.Caches = {Cache}, .Gates = {&ReleaseCallback}, .Restore = RestoreTextureBridgeTests};
		for (uint32 Index = 0; Index < Cubes.size(); ++Index)
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/CubeBridge" + std::to_string(Index), Path));
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Cubes[Index]));
			if (Index == 0)
			{
				FTextureCubeFaceImages Faces;
				Faces.SourceChannelCount = 4;
				for (auto& Face : Faces.Faces)
				{
					auto Image = Image::FImage::TryCreate({.Width = 4, .Height = 4,
						.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{255}));
					ASSERT_TRUE(Image);
					Face = std::move(*Image);
				}
				auto Source = PrepareTextureCubeSource(Faces);
				ASSERT_TRUE(Source);
				Cubes[Index]->SetSource(std::move(*Source));
				Cubes[Index]->SetBuildSettings(ETextureCubeSourceLayout::SixFaces, 0, 0.0f, 4, 4, true);
			}
			else
			{
				const bool HDR = Index == 1;
				auto Image = Image::FImage::TryCreate({.Width = 8, .Height = 4,
					.Format = HDR ? Image::ERawImageFormat::RGBA32F : Image::ERawImageFormat::RGBA8,
					.GammaSpace = HDR ? Image::EImageGammaSpace::Linear : Image::EImageGammaSpace::Unknown},
					FByteBuffer(8 * 4 * (HDR ? 16 : 4), HDR ? std::byte{0} : std::byte{255}));
				ASSERT_TRUE(Image);
				auto Source = PrepareTextureCubePanoramaSource(Image->GetView(), 4, 0);
				ASSERT_TRUE(Source);
				if (HDR)
				{
					Source->ReleaseSourceMemory();
					HDRProbe = Testing::AttachBuildSourceReadProbe(*Source, Source->GetBulkData(), "Payload");
					ASSERT_TRUE(HDRProbe);
				}
				Cubes[Index]->SetSource(std::move(*Source));
				Cubes[Index]->SetBuildSettings(ETextureCubeSourceLayout::EquirectangularPanorama,
					2, 0.0f, 8, 4, !HDR, HDR ? ETextureCubeOutput::HDR : ETextureCubeOutput::LDR);
			}
			Cubes[Index]->BeginCachePlatformData();
		}
		ASSERT_TRUE(WaitUntil([&] { return Cache->Reads.load() >= 2; }, std::chrono::seconds(5)));
		ASSERT_EQ(2u, Cache->Reads.load());
		// Two suspended Cube owners retain their slots; the third stays queued.
		EXPECT_EQ(3u, FAssetCompilingManager::Get().GetNumRemainingAssets());
		auto Marker = Tasks::LaunchTask("CubeOwnerReleasedWorkers", [] {});
		ASSERT_TRUE(WaitUntil([&] { return Marker.GetCompletion().IsReady(); }, std::chrono::seconds(2)));
		ASSERT_TRUE(Marker.GetCompletion().IsReady());
		EXPECT_EQ(ETaskState::Succeeded, Marker.GetState());
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Cubes[0]);
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_FALSE(Cubes[0]->IsAsyncCacheComplete());
		Cache->Release.Trigger();
		ASSERT_TRUE(WaitUntil([&] { return Cache->Writes.load() != 0; }, std::chrono::seconds(5)));
		ASSERT_EQ(1u, Cache->Writes.load());
		EXPECT_FALSE(Cubes[1]->HasPlatformData());
		Cache->ReleaseWrites.Trigger();
		ASSERT_TRUE(CallbackEntered.WaitFor(5.0));
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_FALSE(Cubes[1]->HasPlatformData());
		EXPECT_FALSE(Cubes[1]->IsAsyncCacheComplete());
		ReleaseCallback.Trigger();
		ASSERT_TRUE(Cubes[1]->FinishCachePlatformData());
		EXPECT_EQ(2u, Cubes[1]->GetBuiltFaceDimension());
		EXPECT_FALSE(Cubes[2]->HasPlatformData());
		EXPECT_FALSE(Cubes[2]->IsAsyncCacheComplete());
		FAssetCompilingManager::Get().FinishAllCompilation();
		EXPECT_FALSE(Cubes[0]->HasPlatformData());
		EXPECT_TRUE(Cubes[0]->IsAsyncCacheComplete());
		EXPECT_TRUE(Cubes[2]->HasPlatformData());
		EXPECT_EQ(2u, Cubes[2]->GetBuiltFaceDimension());
		EXPECT_EQ(3u, Cache->Reads.load());
		EXPECT_EQ(2u, Cache->Writes.load());
		EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
		ASSERT_TRUE(HDRProbe);
		EXPECT_EQ(1u, HDRProbe->GetReadStats().RequestCount);
		auto WarmSource = Cubes[1]->GetSource().CopyTornOff();
		WarmSource.ReleaseSourceMemory();
		Cubes[1]->SetSource(std::move(WarmSource));
		Cubes[1]->BeginCachePlatformData();
		ASSERT_TRUE(Cubes[1]->FinishCachePlatformData());
		EXPECT_EQ(4u, Cache->Reads.load());
		EXPECT_EQ(2u, Cache->Writes.load());
		EXPECT_EQ(1u, HDRProbe->GetReadStats().RequestCount);
		Cache->Release.Reset();
		Cubes[1]->SetSource(Cubes[1]->GetSource().CopyTornOff());
		Cubes[1]->BeginCachePlatformData();
		ASSERT_TRUE(WaitUntil([&] { return Cache->Reads.load() >= 5; }, std::chrono::seconds(5)));
		ASSERT_EQ(5u, Cache->Reads.load());
		Cubes[1]->SetBuildSettings(ETextureCubeSourceLayout::EquirectangularPanorama, 4, 0.0f, 8, 4, false, ETextureCubeOutput::HDR);
		Cubes[1]->BeginCachePlatformData();
		ASSERT_TRUE(WaitUntil([&] { return Cache->Reads.load() >= 6; }, std::chrono::seconds(5)));
		ASSERT_EQ(6u, Cache->Reads.load());
		Cache->Release.Trigger();
		ASSERT_TRUE(Cubes[1]->FinishCachePlatformData());
		EXPECT_EQ(4u, Cubes[1]->GetBuiltFaceDimension());
		FAssetCompilingManager::Get().FinishAllCompilation();
		EXPECT_EQ(4u, Cubes[1]->GetBuiltFaceDimension());
		EXPECT_EQ(3u, Cache->Writes.load());
	}
	// The aggregate has terminal shutdown, so this target owns one full lifecycle.
	struct FPilotLifecycle
	{
		~FPilotLifecycle() { ShutdownAssetCompilingManager(); ShutdownTaskSystem(ETaskShutdownMode::Cancel); }
	};

	TEST(FAsyncTaskPilotLifecycleTests, LargeCommitAndSaturatedShutdownPreserveCompletion)
	{
		Testing::InitializeDObjectSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		ASSERT_TRUE(Mounts.IsValid());
		FPilotLifecycle Lifetime;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 2, .MaxNonterminalTasks = 8, .NumBlockingIOThreads = 3}));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor({.MaxQueuedEntries = 1, .MaxQueuedPayloadBytes = 64, .MaxPayloadBytesPerEntry = 64}));
		ASSERT_TRUE(InitializeAssetCompilingManager());
		FModuleManager::Get().LoadModuleChecked("TextureCompressor");
		auto Root = Tasks::LaunchTask("TextureSaturationRoot", [] {}).GetCompletion().GetTaskHandle();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		auto Deferred = Tasks::Then(Tasks::FTaskCompletion(Root), Tasks::ETaskExecutor::GameThreadDeferred, {}, [] {}).GetCompletion().GetTaskHandle();
		ASSERT_TRUE(Deferred.IsValid());
		Testing::RegisterMountPointForTests("/AsyncPilot/", (Testing::GetTestWorkDirectory() / "LargeContent").generic_string() + "/");
		ExerciseStaticMeshOwnerBridge();
		ExerciseCubeOwnerBridge();
		{
			auto Cache = std::make_shared<FHeldDerivedDataCache>();
			Cache->bHoldWrites = true;
			FThreadEvent ReleaseCallback;
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests([&](uint64, ETexture2DCompilationPhase Phase) {
				if (Phase == ETexture2DCompilationPhase::AwaitingApply) {
					ReleaseCallback.WaitFor(10.0);
				}
			});
			AssetPrivate::SetTextureAsyncCacheForTests(Cache);
			std::array<DTexture2D*, 2> Owners{};
			std::array<uint32, 2> Deliveries{};
			FScopedOwnerDrain Drain{.Caches = {Cache}, .Gates = {&ReleaseCallback}, .Restore = RestoreTextureBridgeTests};
			for (uint32 Index = 0; Index < Owners.size(); ++Index)
			{
				FPackagePath OwnerPath;
				ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/HeldCache" + std::to_string(Index), OwnerPath));
				ASSERT_TRUE(CreatePackageLeafAssetForTesting(OwnerPath, Owners[Index]));
				auto Image = Image::FImage::TryCreate({.Width = 4, .Height = 4,
					.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{static_cast<uint8>(Index + 11)}));
				ASSERT_TRUE(Image);
				FTextureSource Source;
				ASSERT_TRUE(Source.Init2D(Image->GetView(), 4));
				FTexture2DCompilationRequest Request;
				Request.Build = MakeTexture2DBuildRequest(Source).value();
				Request.ResultApplication.SourceReplacement = Source;
				Request.Priority = ETexture2DCompilationPriority::Interactive;
				ASSERT_TRUE(SubmitTexture2DCompilation(*Owners[Index], std::move(Request), [&, Index](FTextureOperationResult Result) {
					EXPECT_EQ(Index == 0 ? DerivedData::EStatus::Canceled : DerivedData::EStatus::Ok, Result.Status);
					++Deliveries[Index];
				}));
			}
			ASSERT_TRUE(WaitUntil([&] { return Cache->Reads.load() >= 2; }, std::chrono::seconds(5)));
			ASSERT_EQ(2u, Cache->Reads.load());
			// Both owner slots are suspended in I/O; the two Core workers remain usable.
			auto Marker = Tasks::LaunchTask("TextureOwnerReleasedWorkers", [] { return 42; });
			ASSERT_TRUE(WaitUntil([&] { return Marker.GetCompletion().IsReady(); }, std::chrono::seconds(2)));
			ASSERT_TRUE(Marker.GetCompletion().IsReady());
			EXPECT_EQ(ETaskState::Succeeded, Marker.GetState());
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owners[0]);
			FAssetCompilingManager::Get().ProcessAsyncTasks();
			EXPECT_EQ(2u, FAssetCompilingManager::Get().GetNumRemainingAssets());
			EXPECT_EQ(0u, Deliveries[0]);
			EXPECT_FALSE(Owners[0]->FinishCachePlatformData(0.0));
			Cache->Release.Trigger();
			ASSERT_TRUE(WaitUntil([&] { return Cache->Writes.load() >= 1; }, std::chrono::seconds(5)));
			ASSERT_EQ(1u, Cache->Writes.load());
			EXPECT_FALSE(Owners[1]->FinishCachePlatformData(0.0));
			EXPECT_EQ(0u, Deliveries[1]);
			Cache->ReleaseWrites.Trigger();
			ASSERT_TRUE(WaitUntil([&] {
				return AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Owners[1]).Phase
					== ETexture2DCompilationPhase::AwaitingApply;
			}, std::chrono::seconds(5)));
			// Work can await Apply before its native callback/captures retire.
			EXPECT_EQ(ETexture2DCompilationPhase::AwaitingApply,
				AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Owners[1]).Phase);
			EXPECT_FALSE(Owners[1]->FinishCachePlatformData(0.0));
			EXPECT_EQ(0u, Deliveries[1]);
			ReleaseCallback.Trigger();
			ASSERT_TRUE(Owners[1]->FinishCachePlatformData(10.0));
			EXPECT_EQ(1u, Deliveries[1]);
			EXPECT_EQ(0u, Deliveries[0]);
			EXPECT_TRUE(!Owners[0]->IsAsyncCacheComplete());
			EXPECT_FALSE(Owners[0]->HasPlatformData());
			FAssetCompilingManager::Get().FinishAllCompilation();
			EXPECT_EQ(1u, Deliveries[0]);
			EXPECT_EQ(1u, Cache->Writes.load());
			EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
			// A failing internal assembly/publication callback still produces a terminal result.
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests([](uint64, ETexture2DCompilationPhase Phase) {
				if (Phase == ETexture2DCompilationPhase::AwaitingApply) throw std::runtime_error("Texture callback failure");
			});
			FTexture2DCompilationRequest FailingCallback;
			FailingCallback.Build = MakeTexture2DBuildRequest(Owners[1]->GetSource()).value();
			FailingCallback.Build.bPersistDerivedData = false;
			ASSERT_TRUE(SubmitTexture2DCompilation(*Owners[1], std::move(FailingCallback), [&](FTextureOperationResult Result) {
				EXPECT_EQ(DerivedData::EStatus::Error, Result.Status);
				EXPECT_EQ(Result.Diagnostic, "Texture build task failed.");
				++Deliveries[1];
			}));
			EXPECT_FALSE(Owners[1]->FinishCachePlatformData(10.0));
			EXPECT_EQ(2u, Deliveries[1]);
			EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
		}
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/LargeTexture", Path));
		DTexture2D* Texture = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
		auto ImageResult1 = Image::FImage::TryCreate({.Width = 256, .Height = 256,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(256 * 256 * 4, std::byte{42}));
		ASSERT_TRUE(ImageResult1);
		auto SourceImage = std::move(*ImageResult1);
		FTextureSource Source;
		ASSERT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
		FTexture2DCompilationRequest Request;
		Request.Build = Durin::MakeTexture2DBuildRequest(Source).value();
		Request.ResultApplication.SourceReplacement = Source;
		Request.Build.bPersistDerivedData = false;
		uint32 LargeCompleted = 0;
		ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(Request), [&](FTextureOperationResult Result) {
			EXPECT_TRUE(Result.Succeeded()); ++LargeCompleted;
		}));
		ASSERT_TRUE(Texture->FinishCachePlatformData(10.0));
		EXPECT_EQ(1u, LargeCompleted);
		EXPECT_GT(AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).Metrics.ResultBytes, 64u);
		EXPECT_FALSE(Deferred.IsComplete());
		EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
		CancelTask(Deferred);
		WaitTask(Deferred);

		// Keep an accepted asynchronous build suspended when the owner and Core shut down.
		auto ShutdownCache = std::make_shared<FHeldDerivedDataCache>();
		AssetPrivate::SetTextureAsyncCacheForTests(ShutdownCache);
		uint32 HeldCompleted = 0;
		auto ShutdownMeshCache = std::make_shared<FHeldDerivedDataCache>();
		AssetPrivate::SetStaticMeshAsyncCacheForTests(ShutdownMeshCache);
		std::array<DStaticMesh*, 3> ShutdownMeshes{};
		std::array<uint32, 3> MeshShutdownCompleted{};
		FThreadEvent StartedA, StartedB, Release;
		std::array<FTaskHandle, 8> Blockers;
		uint32 SaturatedCompleted = 0;
		std::array<uint32, 4> Completed{};
		std::array<DObject*, 4> PendingTextures{};
		auto HoldWorker = [&](FThreadEvent& Started, const FTaskCancellationToken& Token) {
			Started.Trigger();
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Token.IsCancellationRequested() && std::chrono::steady_clock::now() < Deadline)
				if (Release.WaitFor(0.001)) break;
		};
		FScopedOwnerDrain ShutdownDrain{.Caches = {ShutdownCache, ShutdownMeshCache}, .Gates = {&Release},
			.bShutdown = true, .Restore = [&] {
				RestoreTextureBridgeTests(); RestoreMeshBridgeTests();
				for (const auto& Task : Blockers) if (Task.IsValid()) WaitTask(Task);
			}};
		FPackagePath HeldPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/HeldShutdownTexture", HeldPath));
		DTexture2D* HeldTexture = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(HeldPath, HeldTexture));
		FTexture2DCompilationRequest HeldRequest;
		HeldRequest.Build = MakeTexture2DBuildRequest(Source).value();
		HeldRequest.ResultApplication.SourceReplacement = Source;
		ASSERT_TRUE(SubmitTexture2DCompilation(*HeldTexture, std::move(HeldRequest), [&](FTextureOperationResult Result) {
			EXPECT_EQ(DerivedData::EStatus::Canceled, Result.Status);
			++HeldCompleted;
		}));
		ASSERT_TRUE(WaitUntil([&] { return ShutdownCache->Reads.load() != 0; }, std::chrono::seconds(5)));
		ASSERT_EQ(1u, ShutdownCache->Reads.load());
		FPackagePath HeldCubePath;
		ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/HeldShutdownCube", HeldCubePath));
		DTextureCube* HeldCube = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(HeldCubePath, HeldCube));
		FTextureCubeFaceImages HeldFaces;
		HeldFaces.SourceChannelCount = 4;
		for (auto& Face : HeldFaces.Faces)
			Face = Image::FImage::TryCreate({.Width = 4, .Height = 4,
				.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{255})).value();
		auto HeldCubeSource = PrepareTextureCubeSource(HeldFaces);
		ASSERT_TRUE(HeldCubeSource);
		HeldCube->SetSource(std::move(*HeldCubeSource));
		HeldCube->SetBuildSettings(ETextureCubeSourceLayout::SixFaces, 0, 0.0f, 4, 4, true);
		HeldCube->BeginCachePlatformData();
		ASSERT_TRUE(WaitUntil([&] { return ShutdownCache->Reads.load() >= 2; }, std::chrono::seconds(5)));
		ASSERT_EQ(2u, ShutdownCache->Reads.load());

		for (uint32 Index = 0; Index < ShutdownMeshes.size(); ++Index)
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/ShutdownMesh" + std::to_string(Index), Path));
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, ShutdownMeshes[Index]));
			ASSERT_TRUE(SubmitBridgeMesh(*ShutdownMeshes[Index], MakeBridgeMeshSource(), [&, Index](const auto& Result) {
				EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, Result.Status); ++MeshShutdownCompleted[Index];
			}));
		}
		ASSERT_TRUE(WaitUntil([&] { return ShutdownMeshCache->Reads.load() != 0; }, std::chrono::seconds(5)));
		ASSERT_EQ(1u, ShutdownMeshCache->Reads.load());
		EXPECT_EQ(3u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);

		Blockers[0] = Tasks::LaunchTask("HoldTextureWorkerA", [&](const FTaskCancellationToken& Token) { HoldWorker(StartedA, Token); }).GetCompletion().GetTaskHandle();
		Blockers[1] = Tasks::LaunchTask("HoldTextureWorkerB", [&](const FTaskCancellationToken& Token) { HoldWorker(StartedB, Token); }).GetCompletion().GetTaskHandle();
		ASSERT_TRUE(StartedA.WaitFor(1.0));
		ASSERT_TRUE(StartedB.WaitFor(1.0));
		Testing::RegisterMountPointForTests("/AsyncPilot/", (Testing::GetTestWorkDirectory() / "ShutdownContent").generic_string() + "/");
		// Fill scheduler capacity independently of the manager's pending-request limit.
		// Requests accepted below must still receive failure/cancellation delivery.
		for (uint32 Index = 2; Index < Blockers.size(); ++Index)
		{
			Blockers[Index] = Tasks::LaunchTask("FillTextureScheduler", [] {}).GetCompletion().GetTaskHandle();
			ASSERT_TRUE(Blockers[Index].IsValid());
		}
		auto ImageResult2 = Image::FImage::TryCreate({.Width = 1, .Height = 1,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(4));
		ASSERT_TRUE(ImageResult2);
		auto SaturatedSourceImage = std::move(*ImageResult2);
		FTextureSource SaturatedSource;
		ASSERT_TRUE(SaturatedSource.Init2D(SaturatedSourceImage.GetView(), 4));
		FTexture2DCompilationRequest SaturatedRequest;
		SaturatedRequest.Build = Durin::MakeTexture2DBuildRequest(SaturatedSource).value();
		SaturatedRequest.ResultApplication.SourceReplacement = SaturatedSource;
		ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(SaturatedRequest),
			[&](FTextureOperationResult Result) {
				EXPECT_EQ(DerivedData::EStatus::Canceled, Result.Status);
				++SaturatedCompleted;
			}));
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_EQ(0u, SaturatedCompleted);
		for (uint32 Index = 2; Index < Blockers.size(); ++Index)
		{
			CancelTask(Blockers[Index]);
			EXPECT_EQ(ETaskState::Canceled, WaitTask(Blockers[Index]).TaskState);
		}
		for (uint32 Index = 0; Index < 4; ++Index)
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/ShutdownTexture" + std::to_string(Index), Path));
			DTexture2D* Texture = nullptr;
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
			PendingTextures[Index] = Texture;
			auto ImageResult3 = Image::FImage::TryCreate({.Width = 64, .Height = 64,
				.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64 * 64 * 4, std::byte{42}));
			ASSERT_TRUE(ImageResult3);
			auto SourceImage = std::move(*ImageResult3);
			FTextureSource Source;
			ASSERT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
			FTexture2DCompilationRequest Request;
			Request.Build = Durin::MakeTexture2DBuildRequest(Source).value();
			Request.ResultApplication.SourceReplacement = Source;
			Request.Build.bPersistDerivedData = false;
			ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(Request), [&, Index](FTextureOperationResult Result) { EXPECT_EQ(DerivedData::EStatus::Canceled, Result.Status); ++Completed[Index]; }));
		}
		// Scheduler thresholds do not reject queued owner work.
		for (uint32 Index = 2; Index < 6; ++Index)
		{
			Blockers[Index] = Tasks::LaunchTask("FillTextureShutdown", [] {}).GetCompletion().GetTaskHandle();
			ASSERT_TRUE(Blockers[Index].IsValid());
		}
		EXPECT_TRUE(Tasks::LaunchTask("SaturatedTextureShutdown", [] {}).GetCompletion().GetTaskHandle().IsValid());
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(PendingTextures);
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*HeldTexture);
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*HeldCube);
		EXPECT_EQ(0u, HeldCompleted);
		ShutdownCache->Release.Trigger();
		ShutdownMeshCache->Release.Trigger();
		// Exercise native cancellation without relying on a producer body or deferred callback.
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		ShutdownAssetCompilingManager();
		EXPECT_EQ(1u, HeldCompleted);
		EXPECT_FALSE(HeldTexture->HasPlatformData());
		EXPECT_FALSE(HeldCube->HasPlatformData());
		EXPECT_TRUE(HeldCube->IsAsyncCacheComplete());
		for (uint32 Index = 0; Index < ShutdownMeshes.size(); ++Index) {
			EXPECT_EQ(1u, MeshShutdownCompleted[Index]);
			EXPECT_EQ(nullptr, ShutdownMeshes[Index]->GetRenderData());
		}
		EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
		for (uint32 Count : Completed) EXPECT_EQ(1u, Count);
		EXPECT_EQ(1u, SaturatedCompleted);
		Release.Trigger();
		for (const auto& Blocker : Blockers) if (Blocker.IsValid()) WaitTask(Blocker);
		ASSERT_TRUE(WaitUntil([&] { return GetTaskSchedulerDiagnostics().NonterminalTaskCount == 0; }, std::chrono::seconds(1)));
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().NonterminalTaskCount);
	}
}
