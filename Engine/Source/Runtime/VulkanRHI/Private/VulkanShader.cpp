#include "VulkanShader.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		constexpr uint32 GSpirvMagic = 0x07230203u;
		constexpr uint16 GSpirvOpEntryPoint = 15u;
		constexpr uint16 GSpirvOpVariable = 59u;
		constexpr uint16 GSpirvOpDecorate = 71u;
		constexpr uint32 GSpirvExecutionModelVertex = 0u;
		constexpr uint32 GSpirvStorageClassInput = 1u;
		constexpr uint32 GSpirvDecorationBuiltIn = 11u;
		constexpr uint32 GSpirvDecorationLocation = 30u;

		auto SpirvStringWordCount(std::span<const uint32> Words) -> size_t
		{
			for (size_t WordIndex = 0; WordIndex < Words.size(); ++WordIndex)
			{
				const uint32 Word = Words[WordIndex];
				for (size_t ByteIndex = 0; ByteIndex < sizeof(uint32); ++ByteIndex)
				{
					if (((Word >> (ByteIndex * 8u)) & 0xffu) == 0u)
						return WordIndex + 1u;
				}
			}
			return 0u;
		}

		auto ReflectVertexInputLocations(
			std::span<const uint32> Words,
			std::unordered_set<uint32>& OutLocations) -> bool
		{
			OutLocations.clear();
			if (Words.size() < 5u || Words[0] != GSpirvMagic) return false;

			std::unordered_set<uint32> EntryPointInterfaces;
			std::unordered_set<uint32> InputVariables;
			std::unordered_set<uint32> BuiltIns;
			std::unordered_map<uint32, uint32> Locations;
			bool bFoundVertexEntryPoint = false;
			for (size_t Offset = 5u; Offset < Words.size();)
			{
				const uint32 Instruction = Words[Offset];
				const uint16 WordCount = static_cast<uint16>(Instruction >> 16u);
				const uint16 Opcode = static_cast<uint16>(Instruction & 0xffffu);
				if (WordCount == 0u || Offset + WordCount > Words.size())
					return false;

				const std::span<const uint32> Operands = Words.subspan(
					Offset + 1u, WordCount - 1u);
				if (Opcode == GSpirvOpEntryPoint && Operands.size() >= 3u
					&& Operands[0] == GSpirvExecutionModelVertex)
				{
					const size_t NameWords = SpirvStringWordCount(Operands.subspan(2u));
					if (NameWords == 0u || 2u + NameWords > Operands.size())
						return false;
					bFoundVertexEntryPoint = true;
					for (const uint32 Id : Operands.subspan(2u + NameWords))
						EntryPointInterfaces.insert(Id);
				}
				else if (Opcode == GSpirvOpVariable && Operands.size() >= 3u
					&& Operands[2] == GSpirvStorageClassInput)
				{
					InputVariables.insert(Operands[1]);
				}
				else if (Opcode == GSpirvOpDecorate && Operands.size() >= 2u)
				{
					if (Operands[1] == GSpirvDecorationLocation
						&& Operands.size() >= 3u)
						Locations.insert_or_assign(Operands[0], Operands[2]);
					else if (Operands[1] == GSpirvDecorationBuiltIn)
						BuiltIns.insert(Operands[0]);
				}
				Offset += WordCount;
			}

			if (!bFoundVertexEntryPoint) return false;
			for (const uint32 Id : EntryPointInterfaces)
			{
				if (!InputVariables.contains(Id)) continue;
				if (const auto Location = Locations.find(Id);
					Location != Locations.end())
				{
					OutLocations.insert(Location->second);
				}
				else if (!BuiltIns.contains(Id))
				{
					OutLocations.clear();
					return false;
				}
			}
			return true;
		}
	}

	FVulkanShader::FVulkanShader(FVulkanDevice& InDevice, const FRHIShaderCreateDesc& InCreateDesc)
		: FRHIShader(InCreateDesc)
		, Device(InDevice)
		, EntryPoint(InCreateDesc.EntryPoint)
	{
		CheckVulkanRHIThread();
		vk::ShaderModuleCreateInfo createInfo;
		// The shader code is expected to be in SPIR-V bytecode format, which is a binary format where each instruction is 4 bytes (32 bits) long. Therefore, the size of the code should be a multiple of 4 bytes.
		check(InCreateDesc.Code.size() % sizeof(uint32) == 0);
		std::span Code = {
			reinterpret_cast<const uint32*>(InCreateDesc.Code.data()),
			InCreateDesc.Code.size() / sizeof(uint32)
		};
		if (InCreateDesc.Frequency == EShaderFrequency::Vertex)
		{
			bHasReflectedVertexInputs = ReflectVertexInputLocations(
				Code, VertexInputLocations);
		}
		createInfo.setCode(Code);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		ThrowIfVulkanNativeCreateFailureIsArmed(
			EVulkanCreateFailurePoint::ShaderModule);
#endif
		ShaderModule = Device.GetHandle().createShaderModule(createInfo);
		Device.GetRHI().GetDebugUtils().NameObject(ShaderModule,
			InCreateDesc.DebugName ? InCreateDesc.DebugName
				: Device.GetRHI().GetDebugUtils().MakeInternalName("ShaderModule"));
	}

	FVulkanShader::~FVulkanShader()
	{
		CheckVulkanRHIThread();
		if (ShaderModule)
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::ShaderModule, ShaderModule);
		}
	}

	auto FVulkanDynamicRHI::RHICreateShader(const FRHIShaderCreateDesc& InCreateDesc) -> FShaderRHIRef
	{
		FShaderRHIRef Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, InCreateDesc, &Result]() {
					Result = new FVulkanShader(*Device, InCreateDesc);
				});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI shader '{}': {}",
				InCreateDesc.DebugName ? InCreateDesc.DebugName : "<unnamed>",
				CreationResult.Diagnostic);
			return nullptr;
		}
		return Result;
	}
}
