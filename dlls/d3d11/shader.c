/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include "config.h"
#include "wine/port.h"

#include "d3d11_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

struct aon9_header
{
    DWORD chunk_size;
    DWORD shader_version;
    DWORD unknown;
    DWORD byte_code_offset;
};

struct shader_handler_context
{
    D3D_FEATURE_LEVEL feature_level;
    struct wined3d_shader_desc *desc;
};

static HRESULT shdr_handler(const char *data, DWORD data_size, DWORD tag, void *context)
{
    const struct shader_handler_context *ctx = context;
    struct wined3d_shader_desc *desc = ctx->desc;
    HRESULT hr;

    switch (tag)
    {
        case TAG_ISGN:
            if (ctx->feature_level <= D3D_FEATURE_LEVEL_9_3)
            {
                TRACE("Skipping shader input signature on feature level %#x.\n", ctx->feature_level);
                break;
            }
            if (FAILED(hr = shader_parse_signature(data, data_size, &desc->input_signature)))
                return hr;
            break;

        case TAG_OSGN:
            if (ctx->feature_level <= D3D_FEATURE_LEVEL_9_3)
            {
                TRACE("Skipping shader output signature on feature level %#x.\n", ctx->feature_level);
                break;
            }
            if (FAILED(hr = shader_parse_signature(data, data_size, &desc->output_signature)))
                return hr;
            break;

        case TAG_SHDR:
        case TAG_SHEX:
            if (ctx->feature_level <= D3D_FEATURE_LEVEL_9_3)
            {
                TRACE("Skipping SM4+ shader code on feature level %#x.\n", ctx->feature_level);
                break;
            }
            if (desc->byte_code)
                FIXME("Multiple shader code chunks.\n");
            desc->byte_code = (const DWORD *)data;
            break;

        case TAG_AON9:
            if (ctx->feature_level <= D3D_FEATURE_LEVEL_9_3)
            {
                const struct aon9_header *header = (const struct aon9_header *)data;
                unsigned int unknown_dword_count;
                const char *byte_code;

                if (data_size < sizeof(*header))
                {
                    WARN("Invalid Aon9 data size %#x.\n", data_size);
                    return E_FAIL;
                }
                byte_code = data + header->byte_code_offset;
                unknown_dword_count = (header->byte_code_offset - sizeof(*header)) / sizeof(DWORD);

                if (data_size - 2 * sizeof(DWORD) < header->byte_code_offset)
                {
                    WARN("Invalid byte code offset %#x (size %#x).\n", header->byte_code_offset, data_size);
                    return E_FAIL;
                }
                FIXME("Skipping %u unknown DWORDs.\n", unknown_dword_count);

                if (desc->byte_code)
                    FIXME("Multiple shader code chunks.\n");
                desc->byte_code = (const DWORD *)byte_code;
                TRACE("Feature level 9 shader version 0%08x, 0%08x.\n", header->shader_version, *desc->byte_code);
            }
            else
            {
                TRACE("Skipping feature level 9 shader code on feature level %#x.\n", ctx->feature_level);
            }
            break;

        default:
            FIXME("Unhandled chunk %s.\n", debugstr_an((const char *)&tag, 4));
            break;
    }

    return S_OK;
}

static HRESULT shader_extract_from_dxbc(const void *dxbc, SIZE_T dxbc_length, struct wined3d_shader_desc *desc,
        D3D_FEATURE_LEVEL feature_level)
{
    struct shader_handler_context ctx = {feature_level, desc};
    HRESULT hr;

    desc->byte_code = NULL;
    memset(&desc->input_signature, 0, sizeof(desc->input_signature));
    memset(&desc->output_signature, 0, sizeof(desc->output_signature));

    hr = parse_dxbc(dxbc, dxbc_length, shdr_handler, &ctx);
    if (!desc->byte_code)
        hr = E_INVALIDARG;

    if (FAILED(hr))
    {
        FIXME("Failed to parse shader, hr %#x.\n", hr);
        shader_free_signature(&desc->input_signature);
        shader_free_signature(&desc->output_signature);
    }

    return hr;
}

static const char *shader_get_string(const char *data, size_t data_size, DWORD offset)
{
    size_t len, max_len;

    if (offset >= data_size)
    {
        WARN("Invalid offset %#x (data size %#lx).\n", offset, (long)data_size);
        return NULL;
    }

    max_len = data_size - offset;
    len = strnlen(data + offset, max_len);

    if (len == max_len)
        return NULL;

    return data + offset;
}

HRESULT shader_parse_signature(const char *data, DWORD data_size, struct wined3d_shader_signature *s)
{
    struct wined3d_shader_signature_element *e;
    const char *ptr = data;
    unsigned int i;
    DWORD count;

    if (!require_space(0, 2, sizeof(DWORD), data_size))
    {
        WARN("Invalid data size %#x.\n", data_size);
        return E_INVALIDARG;
    }

    read_dword(&ptr, &count);
    TRACE("%u elements\n", count);

    skip_dword_unknown(&ptr, 1);

    if (!require_space(ptr - data, count, 6 * sizeof(DWORD), data_size))
    {
        WARN("Invalid count %#x (data size %#x).\n", count, data_size);
        return E_INVALIDARG;
    }

    if (!(e = d3d11_calloc(count, sizeof(*e))))
    {
        ERR("Failed to allocate input signature memory.\n");
        return E_OUTOFMEMORY;
    }

    for (i = 0; i < count; ++i)
    {
        UINT name_offset;

        read_dword(&ptr, &name_offset);
        if (!(e[i].semantic_name = shader_get_string(data, data_size, name_offset)))
        {
            WARN("Invalid name offset %#x (data size %#x).\n", name_offset, data_size);
            HeapFree(GetProcessHeap(), 0, e);
            return E_INVALIDARG;
        }
        read_dword(&ptr, &e[i].semantic_idx);
        read_dword(&ptr, &e[i].sysval_semantic);
        read_dword(&ptr, &e[i].component_type);
        read_dword(&ptr, &e[i].register_idx);
        read_dword(&ptr, &e[i].mask);

        TRACE("semantic: %s, semantic idx: %u, sysval_semantic %#x, "
                "type %u, register idx: %u, use_mask %#x, input_mask %#x\n",
                debugstr_a(e[i].semantic_name), e[i].semantic_idx, e[i].sysval_semantic,
                e[i].component_type, e[i].register_idx, (e[i].mask >> 8) & 0xff, e[i].mask & 0xff);
    }

    s->elements = e;
    s->element_count = count;

    return S_OK;
}

void shader_free_signature(struct wined3d_shader_signature *s)
{
    HeapFree(GetProcessHeap(), 0, s->elements);
}

/* ID3D11VertexShader methods */

static inline struct d3d_vertex_shader *impl_from_ID3D11VertexShader(ID3D11VertexShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_vertex_shader, ID3D11VertexShader_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_vertex_shader_QueryInterface(ID3D11VertexShader *iface,
        REFIID riid, void **object)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VertexShader)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VertexShader_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_ID3D10VertexShader)
            || IsEqualGUID(riid, &IID_ID3D10DeviceChild))
    {
        IUnknown_AddRef(&shader->ID3D10VertexShader_iface);
        *object = &shader->ID3D10VertexShader_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_vertex_shader_AddRef(ID3D11VertexShader *iface)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);
    ULONG refcount = InterlockedIncrement(&shader->refcount);

    TRACE("%p increasing refcount to %u.\n", shader, refcount);

    if (refcount == 1)
    {
        ID3D11Device_AddRef(shader->device);
        wined3d_mutex_lock();
        wined3d_shader_incref(shader->wined3d_shader);
        wined3d_mutex_unlock();
    }

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_vertex_shader_Release(ID3D11VertexShader *iface)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);
    ULONG refcount = InterlockedDecrement(&shader->refcount);

    TRACE("%p decreasing refcount to %u.\n", shader, refcount);

    if (!refcount)
    {
        ID3D11Device *device = shader->device;

        wined3d_mutex_lock();
        wined3d_shader_decref(shader->wined3d_shader);
        wined3d_mutex_unlock();
        /* Release the device last, it may cause the wined3d device to be
         * destroyed. */
        ID3D11Device_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_vertex_shader_GetDevice(ID3D11VertexShader *iface,
        ID3D11Device **device)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = shader->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_vertex_shader_GetPrivateData(ID3D11VertexShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_vertex_shader_SetPrivateData(ID3D11VertexShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_vertex_shader_SetPrivateDataInterface(ID3D11VertexShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D11VertexShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D11VertexShaderVtbl d3d11_vertex_shader_vtbl =
{
    /* IUnknown methods */
    d3d11_vertex_shader_QueryInterface,
    d3d11_vertex_shader_AddRef,
    d3d11_vertex_shader_Release,
    /* ID3D11DeviceChild methods */
    d3d11_vertex_shader_GetDevice,
    d3d11_vertex_shader_GetPrivateData,
    d3d11_vertex_shader_SetPrivateData,
    d3d11_vertex_shader_SetPrivateDataInterface,
};

/* ID3D10VertexShader methods */

static inline struct d3d_vertex_shader *impl_from_ID3D10VertexShader(ID3D10VertexShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_vertex_shader, ID3D10VertexShader_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE d3d10_vertex_shader_QueryInterface(ID3D10VertexShader *iface,
        REFIID riid, void **object)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    return d3d11_vertex_shader_QueryInterface(&shader->ID3D11VertexShader_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d10_vertex_shader_AddRef(ID3D10VertexShader *iface)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p.\n", iface);

    return d3d11_vertex_shader_AddRef(&shader->ID3D11VertexShader_iface);
}

static ULONG STDMETHODCALLTYPE d3d10_vertex_shader_Release(ID3D10VertexShader *iface)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p.\n", iface);

    return d3d11_vertex_shader_Release(&shader->ID3D11VertexShader_iface);
}

/* ID3D10DeviceChild methods */

static void STDMETHODCALLTYPE d3d10_vertex_shader_GetDevice(ID3D10VertexShader *iface, ID3D10Device **device)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    ID3D11Device_QueryInterface(shader->device, &IID_ID3D10Device, (void **)device);
}

static HRESULT STDMETHODCALLTYPE d3d10_vertex_shader_GetPrivateData(ID3D10VertexShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d10_vertex_shader_SetPrivateData(ID3D10VertexShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d10_vertex_shader_SetPrivateDataInterface(ID3D10VertexShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_vertex_shader *shader = impl_from_ID3D10VertexShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D10VertexShaderVtbl d3d10_vertex_shader_vtbl =
{
    /* IUnknown methods */
    d3d10_vertex_shader_QueryInterface,
    d3d10_vertex_shader_AddRef,
    d3d10_vertex_shader_Release,
    /* ID3D10DeviceChild methods */
    d3d10_vertex_shader_GetDevice,
    d3d10_vertex_shader_GetPrivateData,
    d3d10_vertex_shader_SetPrivateData,
    d3d10_vertex_shader_SetPrivateDataInterface,
};

static void STDMETHODCALLTYPE d3d_vertex_shader_wined3d_object_destroyed(void *parent)
{
    struct d3d_vertex_shader *shader = parent;

    wined3d_private_store_cleanup(&shader->private_store);
    HeapFree(GetProcessHeap(), 0, parent);
}

static const struct wined3d_parent_ops d3d_vertex_shader_wined3d_parent_ops =
{
    d3d_vertex_shader_wined3d_object_destroyed,
};

static unsigned int d3d_sm_from_feature_level(D3D_FEATURE_LEVEL feature_level)
{
    switch (feature_level)
    {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            return 5;
        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            return 4;
        case D3D_FEATURE_LEVEL_9_3:
            return 3;
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            return 2;
        default:
            ERR("Unexpected feature_level %#x.\n", feature_level);
    }
    return 0;
}

static HRESULT d3d_vertex_shader_init(struct d3d_vertex_shader *shader, struct d3d_device *device,
        const void *byte_code, SIZE_T byte_code_length)
{
    struct wined3d_shader_desc desc;
    HRESULT hr;

    shader->ID3D11VertexShader_iface.lpVtbl = &d3d11_vertex_shader_vtbl;
    shader->ID3D10VertexShader_iface.lpVtbl = &d3d10_vertex_shader_vtbl;
    shader->refcount = 1;
    wined3d_mutex_lock();
    wined3d_private_store_init(&shader->private_store);

    if (FAILED(hr = shader_extract_from_dxbc(byte_code, byte_code_length, &desc, device->feature_level)))
    {
        WARN("Failed to extract shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return hr;
    }
    desc.max_version = d3d_sm_from_feature_level(device->feature_level);

    hr = wined3d_shader_create_vs(device->wined3d_device, &desc, shader,
            &d3d_vertex_shader_wined3d_parent_ops, &shader->wined3d_shader);
    shader_free_signature(&desc.input_signature);
    shader_free_signature(&desc.output_signature);
    if (FAILED(hr))
    {
        WARN("Failed to create wined3d vertex shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return E_INVALIDARG;
    }
    wined3d_mutex_unlock();

    shader->device = &device->ID3D11Device_iface;
    ID3D11Device_AddRef(shader->device);

    return S_OK;
}

HRESULT d3d_vertex_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d_vertex_shader **shader)
{
    struct d3d_vertex_shader *object;
    HRESULT hr;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d_vertex_shader_init(object, device, byte_code, byte_code_length)))
    {
        WARN("Failed to initialize vertex shader, hr %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created vertex shader %p.\n", object);
    *shader = object;

    return S_OK;
}

struct d3d_vertex_shader *unsafe_impl_from_ID3D11VertexShader(ID3D11VertexShader *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d11_vertex_shader_vtbl);

    return impl_from_ID3D11VertexShader(iface);
}

struct d3d_vertex_shader *unsafe_impl_from_ID3D10VertexShader(ID3D10VertexShader *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d10_vertex_shader_vtbl);

    return impl_from_ID3D10VertexShader(iface);
}

/* ID3D11HullShader methods */

static inline struct d3d11_hull_shader *impl_from_ID3D11HullShader(ID3D11HullShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_hull_shader, ID3D11HullShader_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_hull_shader_QueryInterface(ID3D11HullShader *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11HullShader)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11HullShader_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_hull_shader_AddRef(ID3D11HullShader *iface)
{
    struct d3d11_hull_shader *shader = impl_from_ID3D11HullShader(iface);
    ULONG refcount = InterlockedIncrement(&shader->refcount);

    TRACE("%p increasing refcount to %u.\n", shader, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_hull_shader_Release(ID3D11HullShader *iface)
{
    struct d3d11_hull_shader *shader = impl_from_ID3D11HullShader(iface);
    ULONG refcount = InterlockedDecrement(&shader->refcount);

    TRACE("%p decreasing refcount to %u.\n", shader, refcount);

    if (!refcount)
    {
        ID3D11Device *device = shader->device;

        wined3d_mutex_lock();
        wined3d_shader_decref(shader->wined3d_shader);
        wined3d_mutex_unlock();

        /* Release the device last, it may cause the wined3d device to be
         * destroyed. */
        ID3D11Device_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_hull_shader_GetDevice(ID3D11HullShader *iface,
        ID3D11Device **device)
{
    struct d3d11_hull_shader *shader = impl_from_ID3D11HullShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = shader->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_hull_shader_GetPrivateData(ID3D11HullShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_hull_shader *shader = impl_from_ID3D11HullShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_hull_shader_SetPrivateData(ID3D11HullShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_hull_shader *shader = impl_from_ID3D11HullShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_hull_shader_SetPrivateDataInterface(ID3D11HullShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_hull_shader *shader = impl_from_ID3D11HullShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D11HullShaderVtbl d3d11_hull_shader_vtbl =
{
    /* IUnknown methods */
    d3d11_hull_shader_QueryInterface,
    d3d11_hull_shader_AddRef,
    d3d11_hull_shader_Release,
    /* ID3D11DeviceChild methods */
    d3d11_hull_shader_GetDevice,
    d3d11_hull_shader_GetPrivateData,
    d3d11_hull_shader_SetPrivateData,
    d3d11_hull_shader_SetPrivateDataInterface,
};

static void STDMETHODCALLTYPE d3d11_hull_shader_wined3d_object_destroyed(void *parent)
{
    struct d3d11_hull_shader *shader = parent;

    wined3d_private_store_cleanup(&shader->private_store);
    HeapFree(GetProcessHeap(), 0, parent);
}

static const struct wined3d_parent_ops d3d11_hull_shader_wined3d_parent_ops =
{
    d3d11_hull_shader_wined3d_object_destroyed,
};

static HRESULT d3d11_hull_shader_init(struct d3d11_hull_shader *shader, struct d3d_device *device,
        const void *byte_code, SIZE_T byte_code_length)
{
    struct wined3d_shader_desc desc;
    HRESULT hr;

    shader->ID3D11HullShader_iface.lpVtbl = &d3d11_hull_shader_vtbl;
    shader->refcount = 1;
    wined3d_mutex_lock();
    wined3d_private_store_init(&shader->private_store);

    if (FAILED(hr = shader_extract_from_dxbc(byte_code, byte_code_length, &desc, device->feature_level)))
    {
        WARN("Failed to extract shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return hr;
    }
    desc.max_version = d3d_sm_from_feature_level(device->feature_level);

    hr = wined3d_shader_create_hs(device->wined3d_device, &desc, shader,
            &d3d11_hull_shader_wined3d_parent_ops, &shader->wined3d_shader);
    shader_free_signature(&desc.input_signature);
    shader_free_signature(&desc.output_signature);
    if (FAILED(hr))
    {
        WARN("Failed to create wined3d hull shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return E_INVALIDARG;
    }
    wined3d_mutex_unlock();

    shader->device = &device->ID3D11Device_iface;
    ID3D11Device_AddRef(shader->device);

    return S_OK;
}

HRESULT d3d11_hull_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d11_hull_shader **shader)
{
    struct d3d11_hull_shader *object;
    HRESULT hr;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d11_hull_shader_init(object, device, byte_code, byte_code_length)))
    {
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created hull shader %p.\n", object);
    *shader = object;

    return S_OK;
}

/* ID3D11DomainShader methods */

static inline struct d3d11_domain_shader *impl_from_ID3D11DomainShader(ID3D11DomainShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_domain_shader, ID3D11DomainShader_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_domain_shader_QueryInterface(ID3D11DomainShader *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11DomainShader)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11DomainShader_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_domain_shader_AddRef(ID3D11DomainShader *iface)
{
    struct d3d11_domain_shader *shader = impl_from_ID3D11DomainShader(iface);
    ULONG refcount = InterlockedIncrement(&shader->refcount);

    TRACE("%p increasing refcount to %u.\n", shader, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_domain_shader_Release(ID3D11DomainShader *iface)
{
    struct d3d11_domain_shader *shader = impl_from_ID3D11DomainShader(iface);
    ULONG refcount = InterlockedDecrement(&shader->refcount);

    TRACE("%p decreasing refcount to %u.\n", shader, refcount);

    if (!refcount)
    {
        ID3D11Device *device = shader->device;

        wined3d_mutex_lock();
        wined3d_shader_decref(shader->wined3d_shader);
        wined3d_mutex_unlock();

        /* Release the device last, it may cause the wined3d device to be
         * destroyed. */
        ID3D11Device_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_domain_shader_GetDevice(ID3D11DomainShader *iface,
        ID3D11Device **device)
{
    struct d3d11_domain_shader *shader = impl_from_ID3D11DomainShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = shader->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_domain_shader_GetPrivateData(ID3D11DomainShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_domain_shader *shader = impl_from_ID3D11DomainShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_domain_shader_SetPrivateData(ID3D11DomainShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_domain_shader *shader = impl_from_ID3D11DomainShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_domain_shader_SetPrivateDataInterface(ID3D11DomainShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_domain_shader *shader = impl_from_ID3D11DomainShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D11DomainShaderVtbl d3d11_domain_shader_vtbl =
{
    /* IUnknown methods */
    d3d11_domain_shader_QueryInterface,
    d3d11_domain_shader_AddRef,
    d3d11_domain_shader_Release,
    /* ID3D11DeviceChild methods */
    d3d11_domain_shader_GetDevice,
    d3d11_domain_shader_GetPrivateData,
    d3d11_domain_shader_SetPrivateData,
    d3d11_domain_shader_SetPrivateDataInterface,
};

static void STDMETHODCALLTYPE d3d11_domain_shader_wined3d_object_destroyed(void *parent)
{
    struct d3d11_domain_shader *shader = parent;

    wined3d_private_store_cleanup(&shader->private_store);
    HeapFree(GetProcessHeap(), 0, parent);
}

static const struct wined3d_parent_ops d3d11_domain_shader_wined3d_parent_ops =
{
    d3d11_domain_shader_wined3d_object_destroyed,
};

static HRESULT d3d11_domain_shader_init(struct d3d11_domain_shader *shader, struct d3d_device *device,
        const void *byte_code, SIZE_T byte_code_length)
{
    struct wined3d_shader_desc desc;
    HRESULT hr;

    shader->ID3D11DomainShader_iface.lpVtbl = &d3d11_domain_shader_vtbl;
    shader->refcount = 1;
    wined3d_mutex_lock();
    wined3d_private_store_init(&shader->private_store);

    if (FAILED(hr = shader_extract_from_dxbc(byte_code, byte_code_length, &desc, device->feature_level)))
    {
        WARN("Failed to extract shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return hr;
    }
    desc.max_version = d3d_sm_from_feature_level(device->feature_level);

    hr = wined3d_shader_create_ds(device->wined3d_device, &desc, shader,
            &d3d11_domain_shader_wined3d_parent_ops, &shader->wined3d_shader);
    shader_free_signature(&desc.input_signature);
    shader_free_signature(&desc.output_signature);
    if (FAILED(hr))
    {
        WARN("Failed to create wined3d domain shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return E_INVALIDARG;
    }
    wined3d_mutex_unlock();

    shader->device = &device->ID3D11Device_iface;
    ID3D11Device_AddRef(shader->device);

    return S_OK;
}

HRESULT d3d11_domain_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d11_domain_shader **shader)
{
    struct d3d11_domain_shader *object;
    HRESULT hr;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d11_domain_shader_init(object, device, byte_code, byte_code_length)))
    {
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created domain shader %p.\n", object);
    *shader = object;

    return S_OK;
}

/* ID3D11GeometryShader methods */

static inline struct d3d_geometry_shader *impl_from_ID3D11GeometryShader(ID3D11GeometryShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_geometry_shader, ID3D11GeometryShader_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_geometry_shader_QueryInterface(ID3D11GeometryShader *iface,
        REFIID riid, void **object)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11GeometryShader)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11GeometryShader_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_ID3D10GeometryShader)
            || IsEqualGUID(riid, &IID_ID3D10DeviceChild))
    {
        ID3D10GeometryShader_AddRef(&shader->ID3D10GeometryShader_iface);
        *object = &shader->ID3D10GeometryShader_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_geometry_shader_AddRef(ID3D11GeometryShader *iface)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);
    ULONG refcount = InterlockedIncrement(&shader->refcount);

    TRACE("%p increasing refcount to %u.\n", shader, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_geometry_shader_Release(ID3D11GeometryShader *iface)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);
    ULONG refcount = InterlockedDecrement(&shader->refcount);

    TRACE("%p decreasing refcount to %u.\n", shader, refcount);

    if (!refcount)
    {
        ID3D11Device *device = shader->device;

        wined3d_mutex_lock();
        wined3d_shader_decref(shader->wined3d_shader);
        wined3d_mutex_unlock();

        /* Release the device last, it may cause the wined3d device to be
         * destroyed. */
        ID3D11Device_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_geometry_shader_GetDevice(ID3D11GeometryShader *iface,
        ID3D11Device **device)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = shader->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_geometry_shader_GetPrivateData(ID3D11GeometryShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_geometry_shader_SetPrivateData(ID3D11GeometryShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_geometry_shader_SetPrivateDataInterface(ID3D11GeometryShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D11GeometryShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D11GeometryShaderVtbl d3d11_geometry_shader_vtbl =
{
    /* IUnknown methods */
    d3d11_geometry_shader_QueryInterface,
    d3d11_geometry_shader_AddRef,
    d3d11_geometry_shader_Release,
    /* ID3D11DeviceChild methods */
    d3d11_geometry_shader_GetDevice,
    d3d11_geometry_shader_GetPrivateData,
    d3d11_geometry_shader_SetPrivateData,
    d3d11_geometry_shader_SetPrivateDataInterface,
};

/* ID3D10GeometryShader methods */

static inline struct d3d_geometry_shader *impl_from_ID3D10GeometryShader(ID3D10GeometryShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_geometry_shader, ID3D10GeometryShader_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE d3d10_geometry_shader_QueryInterface(ID3D10GeometryShader *iface,
        REFIID riid, void **object)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    return d3d11_geometry_shader_QueryInterface(&shader->ID3D11GeometryShader_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d10_geometry_shader_AddRef(ID3D10GeometryShader *iface)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p.\n", iface);

    return d3d11_geometry_shader_AddRef(&shader->ID3D11GeometryShader_iface);
}

static ULONG STDMETHODCALLTYPE d3d10_geometry_shader_Release(ID3D10GeometryShader *iface)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p.\n", iface);

    return d3d11_geometry_shader_Release(&shader->ID3D11GeometryShader_iface);
}

/* ID3D10DeviceChild methods */

static void STDMETHODCALLTYPE d3d10_geometry_shader_GetDevice(ID3D10GeometryShader *iface, ID3D10Device **device)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    ID3D11Device_QueryInterface(shader->device, &IID_ID3D10Device, (void **)device);
}

static HRESULT STDMETHODCALLTYPE d3d10_geometry_shader_GetPrivateData(ID3D10GeometryShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d10_geometry_shader_SetPrivateData(ID3D10GeometryShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d10_geometry_shader_SetPrivateDataInterface(ID3D10GeometryShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_geometry_shader *shader = impl_from_ID3D10GeometryShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D10GeometryShaderVtbl d3d10_geometry_shader_vtbl =
{
    /* IUnknown methods */
    d3d10_geometry_shader_QueryInterface,
    d3d10_geometry_shader_AddRef,
    d3d10_geometry_shader_Release,
    /* ID3D10DeviceChild methods */
    d3d10_geometry_shader_GetDevice,
    d3d10_geometry_shader_GetPrivateData,
    d3d10_geometry_shader_SetPrivateData,
    d3d10_geometry_shader_SetPrivateDataInterface,
};

static void STDMETHODCALLTYPE d3d_geometry_shader_wined3d_object_destroyed(void *parent)
{
    struct d3d_geometry_shader *shader = parent;

    wined3d_private_store_cleanup(&shader->private_store);
    HeapFree(GetProcessHeap(), 0, parent);
}

static const struct wined3d_parent_ops d3d_geometry_shader_wined3d_parent_ops =
{
    d3d_geometry_shader_wined3d_object_destroyed,
};

static HRESULT d3d_geometry_shader_init(struct d3d_geometry_shader *shader, struct d3d_device *device,
        const void *byte_code, SIZE_T byte_code_length)
{
    struct wined3d_shader_desc desc;
    HRESULT hr;

    shader->ID3D11GeometryShader_iface.lpVtbl = &d3d11_geometry_shader_vtbl;
    shader->ID3D10GeometryShader_iface.lpVtbl = &d3d10_geometry_shader_vtbl;
    shader->refcount = 1;
    wined3d_mutex_lock();
    wined3d_private_store_init(&shader->private_store);

    if (FAILED(hr = shader_extract_from_dxbc(byte_code, byte_code_length, &desc, device->feature_level)))
    {
        WARN("Failed to extract shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return hr;
    }
    desc.max_version = d3d_sm_from_feature_level(device->feature_level);

    hr = wined3d_shader_create_gs(device->wined3d_device, &desc, shader,
            &d3d_geometry_shader_wined3d_parent_ops, &shader->wined3d_shader);
    shader_free_signature(&desc.input_signature);
    shader_free_signature(&desc.output_signature);
    if (FAILED(hr))
    {
        WARN("Failed to create wined3d geometry shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return E_INVALIDARG;
    }
    wined3d_mutex_unlock();

    shader->device = &device->ID3D11Device_iface;
    ID3D11Device_AddRef(shader->device);

    return S_OK;
}

HRESULT d3d_geometry_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d_geometry_shader **shader)
{
    struct d3d_geometry_shader *object;
    HRESULT hr;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d_geometry_shader_init(object, device, byte_code, byte_code_length)))
    {
        WARN("Failed to initialize geometry shader, hr %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created geometry shader %p.\n", object);
    *shader = object;

    return S_OK;
}

struct d3d_geometry_shader *unsafe_impl_from_ID3D11GeometryShader(ID3D11GeometryShader *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d11_geometry_shader_vtbl);

    return impl_from_ID3D11GeometryShader(iface);
}

struct d3d_geometry_shader *unsafe_impl_from_ID3D10GeometryShader(ID3D10GeometryShader *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d10_geometry_shader_vtbl);

    return impl_from_ID3D10GeometryShader(iface);
}

/* ID3D11PixelShader methods */

static inline struct d3d_pixel_shader *impl_from_ID3D11PixelShader(ID3D11PixelShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_pixel_shader, ID3D11PixelShader_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_pixel_shader_QueryInterface(ID3D11PixelShader *iface,
        REFIID riid, void **object)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11PixelShader)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11PixelShader_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_ID3D10PixelShader)
            || IsEqualGUID(riid, &IID_ID3D10DeviceChild))
    {
        IUnknown_AddRef(&shader->ID3D10PixelShader_iface);
        *object = &shader->ID3D10PixelShader_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_pixel_shader_AddRef(ID3D11PixelShader *iface)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);
    ULONG refcount = InterlockedIncrement(&shader->refcount);

    TRACE("%p increasing refcount to %u.\n", shader, refcount);

    if (refcount == 1)
    {
        ID3D11Device_AddRef(shader->device);
        wined3d_mutex_lock();
        wined3d_shader_incref(shader->wined3d_shader);
        wined3d_mutex_unlock();
    }

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_pixel_shader_Release(ID3D11PixelShader *iface)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);
    ULONG refcount = InterlockedDecrement(&shader->refcount);

    TRACE("%p decreasing refcount to %u.\n", shader, refcount);

    if (!refcount)
    {
        ID3D11Device *device = shader->device;

        wined3d_mutex_lock();
        wined3d_shader_decref(shader->wined3d_shader);
        wined3d_mutex_unlock();
        /* Release the device last, it may cause the wined3d device to be
         * destroyed. */
        ID3D11Device_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_pixel_shader_GetDevice(ID3D11PixelShader *iface,
        ID3D11Device **device)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = shader->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_pixel_shader_GetPrivateData(ID3D11PixelShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_pixel_shader_SetPrivateData(ID3D11PixelShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_pixel_shader_SetPrivateDataInterface(ID3D11PixelShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D11PixelShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D11PixelShaderVtbl d3d11_pixel_shader_vtbl =
{
    /* IUnknown methods */
    d3d11_pixel_shader_QueryInterface,
    d3d11_pixel_shader_AddRef,
    d3d11_pixel_shader_Release,
    /* ID3D11DeviceChild methods */
    d3d11_pixel_shader_GetDevice,
    d3d11_pixel_shader_GetPrivateData,
    d3d11_pixel_shader_SetPrivateData,
    d3d11_pixel_shader_SetPrivateDataInterface,
};

/* ID3D10PixelShader methods */

static inline struct d3d_pixel_shader *impl_from_ID3D10PixelShader(ID3D10PixelShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_pixel_shader, ID3D10PixelShader_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE d3d10_pixel_shader_QueryInterface(ID3D10PixelShader *iface,
        REFIID riid, void **object)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    return d3d11_pixel_shader_QueryInterface(&shader->ID3D11PixelShader_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d10_pixel_shader_AddRef(ID3D10PixelShader *iface)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p.\n", iface);

    return d3d11_pixel_shader_AddRef(&shader->ID3D11PixelShader_iface);
}

static ULONG STDMETHODCALLTYPE d3d10_pixel_shader_Release(ID3D10PixelShader *iface)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p.\n", iface);

    return d3d11_pixel_shader_Release(&shader->ID3D11PixelShader_iface);
}

/* ID3D10DeviceChild methods */

static void STDMETHODCALLTYPE d3d10_pixel_shader_GetDevice(ID3D10PixelShader *iface, ID3D10Device **device)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    ID3D11Device_QueryInterface(shader->device, &IID_ID3D10Device, (void **)device);
}

static HRESULT STDMETHODCALLTYPE d3d10_pixel_shader_GetPrivateData(ID3D10PixelShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d10_pixel_shader_SetPrivateData(ID3D10PixelShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d10_pixel_shader_SetPrivateDataInterface(ID3D10PixelShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d_pixel_shader *shader = impl_from_ID3D10PixelShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D10PixelShaderVtbl d3d10_pixel_shader_vtbl =
{
    /* IUnknown methods */
    d3d10_pixel_shader_QueryInterface,
    d3d10_pixel_shader_AddRef,
    d3d10_pixel_shader_Release,
    /* ID3D10DeviceChild methods */
    d3d10_pixel_shader_GetDevice,
    d3d10_pixel_shader_GetPrivateData,
    d3d10_pixel_shader_SetPrivateData,
    d3d10_pixel_shader_SetPrivateDataInterface,
};

static void STDMETHODCALLTYPE d3d_pixel_shader_wined3d_object_destroyed(void *parent)
{
    struct d3d_pixel_shader *shader = parent;

    wined3d_private_store_cleanup(&shader->private_store);
    HeapFree(GetProcessHeap(), 0, parent);
}

static const struct wined3d_parent_ops d3d_pixel_shader_wined3d_parent_ops =
{
    d3d_pixel_shader_wined3d_object_destroyed,
};

static HRESULT d3d_pixel_shader_init(struct d3d_pixel_shader *shader, struct d3d_device *device,
        const void *byte_code, SIZE_T byte_code_length)
{
    struct wined3d_shader_desc desc;
    HRESULT hr;

    shader->ID3D11PixelShader_iface.lpVtbl = &d3d11_pixel_shader_vtbl;
    shader->ID3D10PixelShader_iface.lpVtbl = &d3d10_pixel_shader_vtbl;
    shader->refcount = 1;
    wined3d_mutex_lock();
    wined3d_private_store_init(&shader->private_store);

    if (FAILED(hr = shader_extract_from_dxbc(byte_code, byte_code_length, &desc, device->feature_level)))
    {
        WARN("Failed to extract shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return hr;
    }
    desc.max_version = d3d_sm_from_feature_level(device->feature_level);

    hr = wined3d_shader_create_ps(device->wined3d_device, &desc, shader,
            &d3d_pixel_shader_wined3d_parent_ops, &shader->wined3d_shader);
    shader_free_signature(&desc.input_signature);
    shader_free_signature(&desc.output_signature);
    if (FAILED(hr))
    {
        WARN("Failed to create wined3d pixel shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return E_INVALIDARG;
    }
    wined3d_mutex_unlock();

    shader->device = &device->ID3D11Device_iface;
    ID3D11Device_AddRef(shader->device);

    return S_OK;
}

HRESULT d3d_pixel_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d_pixel_shader **shader)
{
    struct d3d_pixel_shader *object;
    HRESULT hr;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d_pixel_shader_init(object, device, byte_code, byte_code_length)))
    {
        WARN("Failed to initialize pixel shader, hr %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created pixel shader %p.\n", object);
    *shader = object;

    return S_OK;
}

struct d3d_pixel_shader *unsafe_impl_from_ID3D11PixelShader(ID3D11PixelShader *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d11_pixel_shader_vtbl);

    return impl_from_ID3D11PixelShader(iface);
}

struct d3d_pixel_shader *unsafe_impl_from_ID3D10PixelShader(ID3D10PixelShader *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d10_pixel_shader_vtbl);

    return impl_from_ID3D10PixelShader(iface);
}

/* ID3D11ComputeShader methods */

static inline struct d3d11_compute_shader *impl_from_ID3D11ComputeShader(ID3D11ComputeShader *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_compute_shader, ID3D11ComputeShader_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_compute_shader_QueryInterface(ID3D11ComputeShader *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11ComputeShader)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11ComputeShader_AddRef(*object = iface);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_compute_shader_AddRef(ID3D11ComputeShader *iface)
{
    struct d3d11_compute_shader *shader = impl_from_ID3D11ComputeShader(iface);
    ULONG refcount = InterlockedIncrement(&shader->refcount);

    TRACE("%p increasing refcount to %u.\n", shader, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_compute_shader_Release(ID3D11ComputeShader *iface)
{
    struct d3d11_compute_shader *shader = impl_from_ID3D11ComputeShader(iface);
    ULONG refcount = InterlockedDecrement(&shader->refcount);

    TRACE("%p decreasing refcount to %u.\n", shader, refcount);

    if (!refcount)
    {
        ID3D11Device *device = shader->device;

        wined3d_mutex_lock();
        wined3d_shader_decref(shader->wined3d_shader);
        wined3d_mutex_unlock();

        /* Release the device last, it may cause the wined3d device to be
         * destroyed. */
        ID3D11Device_Release(device);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_compute_shader_GetDevice(ID3D11ComputeShader *iface,
        ID3D11Device **device)
{
    struct d3d11_compute_shader *shader = impl_from_ID3D11ComputeShader(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    ID3D11Device_AddRef(*device = shader->device);
}

static HRESULT STDMETHODCALLTYPE d3d11_compute_shader_GetPrivateData(ID3D11ComputeShader *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_compute_shader *shader = impl_from_ID3D11ComputeShader(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_compute_shader_SetPrivateData(ID3D11ComputeShader *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_compute_shader *shader = impl_from_ID3D11ComputeShader(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&shader->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_compute_shader_SetPrivateDataInterface(ID3D11ComputeShader *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_compute_shader *shader = impl_from_ID3D11ComputeShader(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&shader->private_store, guid, data);
}

static const struct ID3D11ComputeShaderVtbl d3d11_compute_shader_vtbl =
{
    /* IUnknown methods */
    d3d11_compute_shader_QueryInterface,
    d3d11_compute_shader_AddRef,
    d3d11_compute_shader_Release,
    /* ID3D11DeviceChild methods */
    d3d11_compute_shader_GetDevice,
    d3d11_compute_shader_GetPrivateData,
    d3d11_compute_shader_SetPrivateData,
    d3d11_compute_shader_SetPrivateDataInterface,
};

static void STDMETHODCALLTYPE d3d11_compute_shader_wined3d_object_destroyed(void *parent)
{
    struct d3d11_compute_shader *shader = parent;

    wined3d_private_store_cleanup(&shader->private_store);
    HeapFree(GetProcessHeap(), 0, parent);
}

static const struct wined3d_parent_ops d3d11_compute_shader_wined3d_parent_ops =
{
    d3d11_compute_shader_wined3d_object_destroyed,
};

static HRESULT d3d11_compute_shader_init(struct d3d11_compute_shader *shader, struct d3d_device *device,
        const void *byte_code, SIZE_T byte_code_length)
{
    struct wined3d_shader_desc desc;
    HRESULT hr;

    shader->ID3D11ComputeShader_iface.lpVtbl = &d3d11_compute_shader_vtbl;
    shader->refcount = 1;
    wined3d_mutex_lock();
    wined3d_private_store_init(&shader->private_store);

    if (FAILED(hr = shader_extract_from_dxbc(byte_code, byte_code_length, &desc, device->feature_level)))
    {
        WARN("Failed to extract shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return hr;
    }
    desc.max_version = d3d_sm_from_feature_level(device->feature_level);

    hr = wined3d_shader_create_cs(device->wined3d_device, &desc, shader,
            &d3d11_compute_shader_wined3d_parent_ops, &shader->wined3d_shader);
    shader_free_signature(&desc.input_signature);
    shader_free_signature(&desc.output_signature);
    if (FAILED(hr))
    {
        WARN("Failed to create wined3d compute shader, hr %#x.\n", hr);
        wined3d_private_store_cleanup(&shader->private_store);
        wined3d_mutex_unlock();
        return E_INVALIDARG;
    }
    wined3d_mutex_unlock();

    ID3D11Device_AddRef(shader->device = &device->ID3D11Device_iface);

    return S_OK;
}

HRESULT d3d11_compute_shader_create(struct d3d_device *device, const void *byte_code, SIZE_T byte_code_length,
        struct d3d11_compute_shader **shader)
{
    struct d3d11_compute_shader *object;
    HRESULT hr;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d11_compute_shader_init(object, device, byte_code, byte_code_length)))
    {
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created compute shader %p.\n", object);
    *shader = object;

    return S_OK;
}

/* ID3D11ClassLinkage methods */

static inline struct d3d11_class_linkage *impl_from_ID3D11ClassLinkage(ID3D11ClassLinkage *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_class_linkage, ID3D11ClassLinkage_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_class_linkage_QueryInterface(ID3D11ClassLinkage *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11ClassLinkage)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11ClassLinkage_AddRef(*object = iface);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_class_linkage_AddRef(ID3D11ClassLinkage *iface)
{
    struct d3d11_class_linkage *class_linkage = impl_from_ID3D11ClassLinkage(iface);
    ULONG refcount = InterlockedIncrement(&class_linkage->refcount);

    TRACE("%p increasing refcount to %u.\n", class_linkage, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_class_linkage_Release(ID3D11ClassLinkage *iface)
{
    struct d3d11_class_linkage *class_linkage = impl_from_ID3D11ClassLinkage(iface);
    ULONG refcount = InterlockedDecrement(&class_linkage->refcount);

    TRACE("%p decreasing refcount to %u.\n", class_linkage, refcount);

    if (!refcount)
    {
        wined3d_private_store_cleanup(&class_linkage->private_store);
        HeapFree(GetProcessHeap(), 0, class_linkage);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_class_linkage_GetDevice(ID3D11ClassLinkage *iface,
        ID3D11Device **device)
{
    FIXME("iface %p, device %p stub!\n", iface, device);
}

static HRESULT STDMETHODCALLTYPE d3d11_class_linkage_GetPrivateData(ID3D11ClassLinkage *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_class_linkage *class_linkage = impl_from_ID3D11ClassLinkage(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&class_linkage->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_class_linkage_SetPrivateData(ID3D11ClassLinkage *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_class_linkage *class_linkage = impl_from_ID3D11ClassLinkage(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&class_linkage->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_class_linkage_SetPrivateDataInterface(ID3D11ClassLinkage *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_class_linkage *class_linkage = impl_from_ID3D11ClassLinkage(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&class_linkage->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_class_linkage_GetClassInstance(ID3D11ClassLinkage *iface,
        const char *instance_name, UINT instance_index, ID3D11ClassInstance **class_instance)
{
    FIXME("iface %p, instance_name %s, instance_index %u, class_instance %p stub!\n",
            iface, debugstr_a(instance_name), instance_index, class_instance);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_class_linkage_CreateClassInstance(ID3D11ClassLinkage *iface,
        const char *type_name, UINT cb_offset, UINT cb_vector_offset, UINT texture_offset,
        UINT sampler_offset, ID3D11ClassInstance **class_instance)
{
    FIXME("iface %p, type_name %s, cb_offset %u, cb_vector_offset %u, texture_offset %u, "
            "sampler_offset %u, class_instance %p stub!\n",
            iface, debugstr_a(type_name), cb_offset, cb_vector_offset, texture_offset,
            sampler_offset, class_instance);

    return E_NOTIMPL;
}

static const struct ID3D11ClassLinkageVtbl d3d11_class_linkage_vtbl =
{
    /* IUnknown methods */
    d3d11_class_linkage_QueryInterface,
    d3d11_class_linkage_AddRef,
    d3d11_class_linkage_Release,
    /* ID3D11DeviceChild methods */
    d3d11_class_linkage_GetDevice,
    d3d11_class_linkage_GetPrivateData,
    d3d11_class_linkage_SetPrivateData,
    d3d11_class_linkage_SetPrivateDataInterface,
    /* ID3D11ClassLinkage methods */
    d3d11_class_linkage_GetClassInstance,
    d3d11_class_linkage_CreateClassInstance,
};

HRESULT d3d11_class_linkage_create(struct d3d_device *device, struct d3d11_class_linkage **class_linkage)
{
    struct d3d11_class_linkage *object;

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D11ClassLinkage_iface.lpVtbl = &d3d11_class_linkage_vtbl;
    object->refcount = 1;
    wined3d_private_store_init(&object->private_store);

    TRACE("Created class linkage %p.\n", object);
    *class_linkage = object;

    return S_OK;
}
