#pragma once

namespace Doge
{
	enum class EResizeBufferFlags
	{
		None = 0,					 // No flags
		AllowSlackOnGrow = 1 << 0,	 // will allocate slack when growing the array.
		AllowSlackOnReduce = 1 << 1, // will leave the slack when reducing the array.
	};
	ENUM_CLASS_FLAGS(EResizeBufferFlags);

	class IStaticMeshVertexData
	{
	public:
		virtual ~IStaticMeshVertexData() = default;

		virtual auto ResizeBuffer(uint32 NumVertices, EResizeBufferFlags BufferFlags = EResizeBufferFlags::None) -> void = 0;

		virtual auto Clear(uint32 NumVertices = 0) -> void = 0;

		virtual bool IsValidIndex(uint32 Index) = 0;

		virtual auto GetStride() -> uint32 const = 0;

		virtual auto Num() -> uint32 const = 0;

		virtual auto GetDataPointer() -> uint8* = 0;

		virtual auto GetResourceSize() -> size_t const = 0;

		virtual auto GetAllowCPUAccess() -> bool const = 0;
	};

	template<typename VertexDataType>
	class TStaticMeshVertexData : public IStaticMeshVertexData
	{
		using FVertexDataArray = std::vector<VertexDataType>;

	private:
		FVertexDataArray Data_;

		bool bNeedCPUAccess_ = false;

	public:
		TStaticMeshVertexData(bool bNeedCPUAccess = false)
			: bNeedCPUAccess_(bNeedCPUAccess)
		{
		}

		virtual auto ResizeBuffer(uint32 NumVertices, EResizeBufferFlags BufferFlags = EResizeBufferFlags::None) -> void override
		{
			// TODO : use flags
			Data_.resize(NumVertices);
		}

		virtual auto IsValidIndex(uint32 Index) -> bool override
		{
			return Index < Data_.size();
		}

		virtual auto Clear(uint32 NumVertices = 0) -> void override
		{
			Data_.clear();

			if (Data_.capacity() < NumVertices * 2)
			{
				Data_.reserve(NumVertices);
			}
			else
			{
				std::vector<VertexDataType> temp;
				temp.reserve(NumVertices);
				Data_.swap(temp);
			}
		}

		virtual auto GetStride() -> uint32 const override
		{
			return sizeof(VertexDataType);
		}

		virtual auto Num() -> uint32 const override
		{
			return static_cast<uint32>(Data_.size());
		}

		virtual auto GetDataPointer() -> uint8* override
		{
			return reinterpret_cast<uint8*>(Data_.data());
		}

		virtual auto GetResourceSize() -> size_t const override
		{
			return Data_.capacity() * sizeof(VertexDataType);
		}

		virtual auto GetAllowCPUAccess() -> bool const override
		{
			return bNeedCPUAccess_;
		}
	};
}