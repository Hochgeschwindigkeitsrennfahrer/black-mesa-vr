#pragma once
#include <d3d9.h>

// A reused state block can retain its creation-time stream offsets.
// Preserve the live bindings around custom DrawPrimitiveUP/hand passes.
class VrD3D9StreamSnapshot
{
  public:
    VrD3D9StreamSnapshot() = default;
    VrD3D9StreamSnapshot(const VrD3D9StreamSnapshot &) = delete;
    VrD3D9StreamSnapshot &operator=(const VrD3D9StreamSnapshot &) = delete;

    ~VrD3D9StreamSnapshot()
    {
        Release();
    }

    bool Capture(IDirect3DDevice9 *device, UINT count = 16)
    {
        Release();
        m_Device = device;
        m_Count = count <= 16 ? count : 16;
        if (!m_Device)
            return false;
        for (UINT i = 0; i < m_Count; ++i)
        {
            Stream &stream = m_Streams[i];
            if (FAILED(m_Device->GetStreamSource(i, &stream.buffer, &stream.offset, &stream.stride)))
            {
                Release();
                return false;
            }
        }
        return true;
    }

    void Restore()
    {
        if (m_Device)
        {
            for (UINT i = 0; i < m_Count; ++i)
            {
                const Stream &stream = m_Streams[i];
                m_Device->SetStreamSource(i, stream.buffer, stream.offset, stream.stride);
            }
        }
        Release();
    }

  private:
    struct Stream
    {
        IDirect3DVertexBuffer9 *buffer = nullptr;
        UINT offset = 0;
        UINT stride = 0;
    };
    Stream m_Streams[16]{};
    IDirect3DDevice9 *m_Device = nullptr;
    UINT m_Count = 0;

    void Release()
    {
        for (Stream &stream : m_Streams)
        {
            if (stream.buffer)
                stream.buffer->Release();
            stream = {};
        }
        m_Device = nullptr;
        m_Count = 0;
    }
};
