#pragma once

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

	virtual auto Empty() -> void = 0;

	virtual auto GetStride() -> uint32 const = 0;

	virtual auto Num() -> uint32 const = 0;

	virtual auto GetDataPointer() -> uint8* = 0;

	virtual auto GetResourceSize() -> size_t const = 0;

	virtual auto GetAllowCPUAccess() -> bool const = 0;
};

template<typename VertexDataType>
class TStaticMeshVertexData : public IStaticMeshVertexData
{
	using FVertexDataArray = TArray<VertexDataType>;

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

	virtual auto Empty() -> void override
	{
		Data_.empty();
	}

	virtual auto GetStride() -> uint32 const override
	{
		return sizeof(VertexDataType);
	}

	virtual auto Num() -> uint32 const override
	{
		return Data_.size();
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