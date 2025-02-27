#include "cuda-device.h"
#include "cuda-buffer.h"
#include "cuda-command-queue.h"
#include "cuda-pipeline.h"
#include "cuda-query.h"
#include "cuda-shader-object-layout.h"
#include "cuda-shader-object.h"
#include "cuda-shader-program.h"
#include "cuda-texture.h"
#include "cuda-texture-view.h"
#include "cuda-acceleration-structure.h"

namespace rhi::cuda {

int DeviceImpl::_calcSMCountPerMultiProcessor(int major, int minor)
{
    // Defines for GPU Architecture types (using the SM version to determine
    // the # of cores per SM
    struct SMInfo
    {
        int sm; // 0xMm (hexadecimal notation), M = SM Major version, and m = SM minor version
        int coreCount;
    };

    static const SMInfo infos[] = {
        {0x30, 192},
        {0x32, 192},
        {0x35, 192},
        {0x37, 192},
        {0x50, 128},
        {0x52, 128},
        {0x53, 128},
        {0x60, 64},
        {0x61, 128},
        {0x62, 128},
        {0x70, 64},
        {0x72, 64},
        {0x75, 64}
    };

    const int sm = ((major << 4) + minor);
    for (Index i = 0; i < SLANG_COUNT_OF(infos); ++i)
    {
        if (infos[i].sm == sm)
        {
            return infos[i].coreCount;
        }
    }

    const auto& last = infos[SLANG_COUNT_OF(infos) - 1];

    // It must be newer presumably
    SLANG_RHI_ASSERT(sm > last.sm);

    // Default to the last entry
    return last.coreCount;
}

Result DeviceImpl::_findMaxFlopsDeviceIndex(int* outDeviceIndex)
{
    int smPerMultiproc = 0;
    int maxPerfDevice = -1;
    int deviceCount = 0;
    int devicesProhibited = 0;

    uint64_t maxComputePerf = 0;
    SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGetCount(&deviceCount));

    // Find the best CUDA capable GPU device
    for (int currentDevice = 0; currentDevice < deviceCount; ++currentDevice)
    {
        CUdevice device;
        SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGet(&device, currentDevice));
        int computeMode = -1, major = 0, minor = 0;
        SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGetAttribute(&computeMode, CU_DEVICE_ATTRIBUTE_COMPUTE_MODE, device));
        SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device));
        SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device));

        // If this GPU is not running on Compute Mode prohibited,
        // then we can add it to the list
        if (computeMode != CU_COMPUTEMODE_PROHIBITED)
        {
            if (major == 9999 && minor == 9999)
            {
                smPerMultiproc = 1;
            }
            else
            {
                smPerMultiproc = _calcSMCountPerMultiProcessor(major, minor);
            }

            int multiProcessorCount = 0, clockRate = 0;
            SLANG_CUDA_RETURN_ON_FAIL(
                cuDeviceGetAttribute(&multiProcessorCount, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, device)
            );
            SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGetAttribute(&clockRate, CU_DEVICE_ATTRIBUTE_CLOCK_RATE, device));
            uint64_t compute_perf = uint64_t(multiProcessorCount) * smPerMultiproc * clockRate;

            if (compute_perf > maxComputePerf)
            {
                maxComputePerf = compute_perf;
                maxPerfDevice = currentDevice;
            }
        }
        else
        {
            devicesProhibited++;
        }
    }

    if (maxPerfDevice < 0)
    {
        return SLANG_FAIL;
    }

    *outDeviceIndex = maxPerfDevice;
    return SLANG_OK;
}

Result DeviceImpl::_initCuda(CUDAReportStyle reportType)
{
    if (!rhiCudaApiInit())
    {
        return SLANG_FAIL;
    }
    CUresult res = cuInit(0);
    SLANG_CUDA_RETURN_WITH_REPORT_ON_FAIL(res, reportType);
    return SLANG_OK;
}

DeviceImpl::~DeviceImpl()
{
    m_queue.setNull();

#if SLANG_RHI_ENABLE_OPTIX
    if (m_ctx.optixContext)
    {
        optixDeviceContextDestroy(m_ctx.optixContext);
    }
#endif

    if (m_ctx.context)
    {
        cuCtxDestroy(m_ctx.context);
    }
}

Result DeviceImpl::getNativeDeviceHandles(DeviceNativeHandles* outHandles)
{
    outHandles->handles[0].type = NativeHandleType::CUdevice;
    outHandles->handles[0].value = m_ctx.device;
#if SLANG_RHI_ENABLE_OPTIX
    outHandles->handles[1].type = NativeHandleType::OptixDeviceContext;
    outHandles->handles[1].value = (uint64_t)m_ctx.optixContext;
#else
    outHandles->handles[1] = {};
#endif
    outHandles->handles[2] = {};
    return SLANG_OK;
}

Result DeviceImpl::initialize(const DeviceDesc& desc)
{
    SLANG_RETURN_ON_FAIL(slangContext.initialize(
        desc.slang,
        desc.extendedDescCount,
        desc.extendedDescs,
        SLANG_PTX,
        "sm_5_1",
        std::array{slang::PreprocessorMacroDesc{"__CUDA_COMPUTE__", "1"}}
    ));

    SLANG_RETURN_ON_FAIL(Device::initialize(desc));

    SLANG_RETURN_ON_FAIL(_initCuda(reportType));

    int selectedDeviceIndex = -1;
    if (desc.adapterLUID)
    {
        int deviceCount = -1;
        cuDeviceGetCount(&deviceCount);
        for (int deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex)
        {
            if (cuda::getAdapterLUID(deviceIndex) == *desc.adapterLUID)
            {
                selectedDeviceIndex = deviceIndex;
                break;
            }
        }
    }
    else
    {
        SLANG_RETURN_ON_FAIL(_findMaxFlopsDeviceIndex(&selectedDeviceIndex));
    }

    SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGet(&m_ctx.device, selectedDeviceIndex));

    SLANG_CUDA_RETURN_WITH_REPORT_ON_FAIL(cuCtxCreate(&m_ctx.context, 0, m_ctx.device), reportType);

    {
        // Not clear how to detect half support on CUDA. For now we'll assume we have it
        m_features.push_back("half");

        // CUDA has support for realtime clock
        m_features.push_back("realtime-clock");

        // Allows use of a ptr like type
        m_features.push_back("has-ptr");
    }

#if SLANG_RHI_ENABLE_OPTIX
    {
        SLANG_OPTIX_RETURN_ON_FAIL(optixInit());

        static auto logCallback = [](unsigned int level, const char* tag, const char* message, void* /*cbdata */)
        {
            printf("[%2u][%12s]: %s\n", level, tag, message);
            fflush(stdout);
        };

        OptixDeviceContextOptions options = {};
        options.logCallbackFunction = logCallback;
        options.logCallbackLevel = 4;
#ifdef _DEBUG
        options.validationMode = OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL;
#endif
        SLANG_OPTIX_RETURN_ON_FAIL(optixDeviceContextCreate(m_ctx.context, &options, &m_ctx.optixContext));

        m_features.push_back("ray-tracing");
    }
#endif

    // Initialize DeviceInfo
    {
        m_info.deviceType = DeviceType::CUDA;
        m_info.apiName = "CUDA";
        static const float kIdentity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        ::memcpy(m_info.identityProjectionMatrix, kIdentity, sizeof(kIdentity));
        char deviceName[256];
        cuDeviceGetName(deviceName, sizeof(deviceName), m_ctx.device);
        m_adapterName = deviceName;
        m_info.adapterName = m_adapterName.data();
        m_info.timestampFrequency = 1000000;
    }

    // Get device limits.
    {
        CUresult lastResult = CUDA_SUCCESS;
        auto getAttribute = [&](CUdevice_attribute attribute) -> int
        {
            int value;
            CUresult result = cuDeviceGetAttribute(&value, attribute, m_ctx.device);
            if (result != CUDA_SUCCESS)
                lastResult = result;
            return value;
        };

        DeviceLimits limits = {};

        limits.maxTextureDimension1D = getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE1D_WIDTH);
        limits.maxTextureDimension2D = min({
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE2D_WIDTH),
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE2D_HEIGHT),
        });
        limits.maxTextureDimension3D = min({
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE3D_WIDTH),
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE3D_HEIGHT),
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE3D_DEPTH),
        });
        limits.maxTextureDimensionCube = getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACECUBEMAP_WIDTH);
        limits.maxTextureArrayLayers = min({
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE1D_LAYERED_LAYERS),
            getAttribute(CU_DEVICE_ATTRIBUTE_MAXIMUM_SURFACE2D_LAYERED_LAYERS),
        });

        // limits.maxVertexInputElements
        // limits.maxVertexInputElementOffset
        // limits.maxVertexStreams
        // limits.maxVertexStreamStride

        limits.maxComputeThreadsPerGroup = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK);
        limits.maxComputeThreadGroupSize[0] = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X);
        limits.maxComputeThreadGroupSize[1] = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y);
        limits.maxComputeThreadGroupSize[2] = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z);
        limits.maxComputeDispatchThreadGroups[0] = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X);
        limits.maxComputeDispatchThreadGroups[1] = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y);
        limits.maxComputeDispatchThreadGroups[2] = getAttribute(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z);

        // limits.maxViewports
        // limits.maxViewportDimensions
        // limits.maxFramebufferDimensions

        // limits.maxShaderVisibleSamplers

        m_info.limits = limits;

        SLANG_CUDA_RETURN_ON_FAIL(lastResult);
    }

    m_queue = new CommandQueueImpl(this, QueueType::Graphics);

    return SLANG_OK;
}

Result DeviceImpl::getCUDAFormat(Format format, CUarray_format* outFormat)
{
    // TODO: Expand to cover all available formats that can be supported in CUDA
    switch (format)
    {
    case Format::R32G32B32A32_FLOAT:
    case Format::R32G32B32_FLOAT:
    case Format::R32G32_FLOAT:
    case Format::R32_FLOAT:
    case Format::D32_FLOAT:
        *outFormat = CU_AD_FORMAT_FLOAT;
        return SLANG_OK;
    case Format::R16G16B16A16_FLOAT:
    case Format::R16G16_FLOAT:
    case Format::R16_FLOAT:
        *outFormat = CU_AD_FORMAT_HALF;
        return SLANG_OK;
    case Format::R32G32B32A32_UINT:
    case Format::R32G32B32_UINT:
    case Format::R32G32_UINT:
    case Format::R32_UINT:
        *outFormat = CU_AD_FORMAT_UNSIGNED_INT32;
        return SLANG_OK;
    case Format::R16G16B16A16_UINT:
    case Format::R16G16_UINT:
    case Format::R16_UINT:
        *outFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        return SLANG_OK;
    case Format::R8G8B8A8_UINT:
    case Format::R8G8_UINT:
    case Format::R8_UINT:
    case Format::R8G8B8A8_UNORM:
        *outFormat = CU_AD_FORMAT_UNSIGNED_INT8;
        return SLANG_OK;
    case Format::R32G32B32A32_SINT:
    case Format::R32G32B32_SINT:
    case Format::R32G32_SINT:
    case Format::R32_SINT:
        *outFormat = CU_AD_FORMAT_SIGNED_INT32;
        return SLANG_OK;
    case Format::R16G16B16A16_SINT:
    case Format::R16G16_SINT:
    case Format::R16_SINT:
        *outFormat = CU_AD_FORMAT_SIGNED_INT16;
        return SLANG_OK;
    case Format::R8G8B8A8_SINT:
    case Format::R8G8_SINT:
    case Format::R8_SINT:
        *outFormat = CU_AD_FORMAT_SIGNED_INT8;
        return SLANG_OK;
    default:
        SLANG_RHI_ASSERT_FAILURE("Only support R32_FLOAT/R8G8B8A8_UNORM formats for now");
        return SLANG_FAIL;
    }
}

Result DeviceImpl::createTexture(const TextureDesc& desc, const SubresourceData* initData, ITexture** outTexture)
{
    TextureDesc srcDesc = fixupTextureDesc(desc);

    RefPtr<TextureImpl> tex = new TextureImpl(srcDesc);

    CUresourcetype resourceType;

    // The size of the element/texel in bytes
    size_t elementSize = 0;

    // Our `TextureDesc` uses an enumeration to specify
    // the "shape"/rank of a texture (1D, 2D, 3D, Cube), but CUDA's
    // `cuMipmappedArrayCreate` seemingly relies on a policy where
    // the extents of the array in dimenions above the rank are
    // specified as zero (e.g., a 1D texture requires `height==0`).
    //
    // We will start by massaging the extents as specified by the
    // user into a form that CUDA wants/expects, based on the
    // texture shape as specified in the `desc`.
    //
    int width = desc.size.width;
    int height = desc.size.height;
    int depth = desc.size.depth;
    switch (desc.type)
    {
    case TextureType::Texture1D:
        height = 0;
        depth = 0;
        break;

    case TextureType::Texture2D:
        depth = 0;
        break;

    case TextureType::Texture3D:
        break;

    case TextureType::TextureCube:
        depth = 1;
        break;
    }

    {
        CUarray_format format = CU_AD_FORMAT_FLOAT;
        int numChannels = 0;

        SLANG_RETURN_ON_FAIL(getCUDAFormat(desc.format, &format));
        const FormatInfo& info = getFormatInfo(desc.format);
        numChannels = info.channelCount;

        switch (format)
        {
        case CU_AD_FORMAT_FLOAT:
        {
            elementSize = sizeof(float) * numChannels;
            break;
        }
        case CU_AD_FORMAT_HALF:
        {
            elementSize = sizeof(uint16_t) * numChannels;
            break;
        }
        case CU_AD_FORMAT_UNSIGNED_INT8:
        {
            elementSize = sizeof(uint8_t) * numChannels;
            break;
        }
        default:
        {
            SLANG_RHI_ASSERT_FAILURE("Only support R32_FLOAT/R8G8B8A8_UNORM formats for now");
            return SLANG_FAIL;
        }
        }

        if (desc.mipLevelCount > 1)
        {
            resourceType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;

            CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
            memset(&arrayDesc, 0, sizeof(arrayDesc));

            arrayDesc.Width = width;
            arrayDesc.Height = height;
            arrayDesc.Depth = depth;
            arrayDesc.Format = format;
            arrayDesc.NumChannels = numChannels;
            arrayDesc.Flags = 0;

            if (desc.arrayLength > 1)
            {
                if (desc.type == TextureType::Texture1D || desc.type == TextureType::Texture2D ||
                    desc.type == TextureType::TextureCube)
                {
                    arrayDesc.Flags |= CUDA_ARRAY3D_LAYERED;
                    arrayDesc.Depth = desc.arrayLength;
                }
                else
                {
                    SLANG_RHI_ASSERT_FAILURE("Arrays only supported for 1D and 2D");
                    return SLANG_FAIL;
                }
            }

            if (desc.type == TextureType::TextureCube)
            {
                arrayDesc.Flags |= CUDA_ARRAY3D_CUBEMAP;
                arrayDesc.Depth *= 6;
            }

            SLANG_CUDA_RETURN_ON_FAIL(cuMipmappedArrayCreate(&tex->m_cudaMipMappedArray, &arrayDesc, desc.mipLevelCount)
            );
        }
        else
        {
            resourceType = CU_RESOURCE_TYPE_ARRAY;

            if (desc.arrayLength > 1)
            {
                if (desc.type == TextureType::Texture1D || desc.type == TextureType::Texture2D ||
                    desc.type == TextureType::TextureCube)
                {
                    SLANG_RHI_ASSERT_FAILURE("Only 1D, 2D and Cube arrays supported");
                    return SLANG_FAIL;
                }

                CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
                memset(&arrayDesc, 0, sizeof(arrayDesc));

                // Set the depth as the array length
                arrayDesc.Depth = desc.arrayLength;
                if (desc.type == TextureType::TextureCube)
                {
                    arrayDesc.Depth *= 6;
                }

                arrayDesc.Height = height;
                arrayDesc.Width = width;
                arrayDesc.Format = format;
                arrayDesc.NumChannels = numChannels;

                if (desc.type == TextureType::TextureCube)
                {
                    arrayDesc.Flags |= CUDA_ARRAY3D_CUBEMAP;
                }

                SLANG_CUDA_RETURN_ON_FAIL(cuArray3DCreate(&tex->m_cudaArray, &arrayDesc));
            }
            else if (desc.type == TextureType::Texture3D || desc.type == TextureType::TextureCube)
            {
                CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
                memset(&arrayDesc, 0, sizeof(arrayDesc));

                arrayDesc.Depth = depth;
                arrayDesc.Height = height;
                arrayDesc.Width = width;
                arrayDesc.Format = format;
                arrayDesc.NumChannels = numChannels;

                arrayDesc.Flags = 0;

                // Handle cube texture
                if (desc.type == TextureType::TextureCube)
                {
                    arrayDesc.Depth = 6;
                    arrayDesc.Flags |= CUDA_ARRAY3D_CUBEMAP;
                }

                SLANG_CUDA_RETURN_ON_FAIL(cuArray3DCreate(&tex->m_cudaArray, &arrayDesc));
            }
            else
            {
                CUDA_ARRAY_DESCRIPTOR arrayDesc;
                memset(&arrayDesc, 0, sizeof(arrayDesc));

                arrayDesc.Height = height;
                arrayDesc.Width = width;
                arrayDesc.Format = format;
                arrayDesc.NumChannels = numChannels;

                // Allocate the array, will work for 1D or 2D case
                SLANG_CUDA_RETURN_ON_FAIL(cuArrayCreate(&tex->m_cudaArray, &arrayDesc));
            }
        }
    }

    // Work space for holding data for uploading if it needs to be rearranged
    if (initData)
    {
        std::vector<uint8_t> workspace;
        for (int mipLevel = 0; mipLevel < desc.mipLevelCount; ++mipLevel)
        {
            int mipWidth = width >> mipLevel;
            int mipHeight = height >> mipLevel;
            int mipDepth = depth >> mipLevel;

            mipWidth = (mipWidth == 0) ? 1 : mipWidth;
            mipHeight = (mipHeight == 0) ? 1 : mipHeight;
            mipDepth = (mipDepth == 0) ? 1 : mipDepth;

            // If it's a cubemap then the depth is always 6
            if (desc.type == TextureType::TextureCube)
            {
                mipDepth = 6;
            }

            auto dstArray = tex->m_cudaArray;
            if (tex->m_cudaMipMappedArray)
            {
                // Get the array for the mip level
                SLANG_CUDA_RETURN_ON_FAIL(cuMipmappedArrayGetLevel(&dstArray, tex->m_cudaMipMappedArray, mipLevel));
            }
            SLANG_RHI_ASSERT(dstArray);

            // Check using the desc to see if it's plausible
            {
                CUDA_ARRAY_DESCRIPTOR arrayDesc;
                SLANG_CUDA_RETURN_ON_FAIL(cuArrayGetDescriptor(&arrayDesc, dstArray));

                SLANG_RHI_ASSERT(mipWidth == arrayDesc.Width);
                SLANG_RHI_ASSERT(mipHeight == arrayDesc.Height || (mipHeight == 1 && arrayDesc.Height == 0));
            }

            const void* srcDataPtr = nullptr;

            if (desc.arrayLength > 1)
            {
                SLANG_RHI_ASSERT(
                    desc.type == TextureType::Texture1D || desc.type == TextureType::Texture2D ||
                    desc.type == TextureType::TextureCube
                );

                // TODO(JS): Here I assume that arrays are just held contiguously within a
                // 'face' This seems reasonable and works with the Copy3D.
                const size_t faceSizeInBytes = elementSize * mipWidth * mipHeight;

                Index faceCount = desc.arrayLength;
                if (desc.type == TextureType::TextureCube)
                {
                    faceCount *= 6;
                }

                const size_t mipSizeInBytes = faceSizeInBytes * faceCount;
                workspace.resize(mipSizeInBytes);

                // We need to add the face data from each mip
                // We iterate over face count so we copy all of the cubemap faces
                for (Index j = 0; j < faceCount; j++)
                {
                    const auto srcData = initData[mipLevel + j * desc.mipLevelCount].data;
                    // Copy over to the workspace to make contiguous
                    ::memcpy(workspace.data() + faceSizeInBytes * j, srcData, faceSizeInBytes);
                }

                srcDataPtr = workspace.data();
            }
            else
            {
                if (desc.type == TextureType::TextureCube)
                {
                    size_t faceSizeInBytes = elementSize * mipWidth * mipHeight;

                    workspace.resize(faceSizeInBytes * 6);
                    // Copy the data over to make contiguous
                    for (Index j = 0; j < 6; j++)
                    {
                        const auto srcData = initData[mipLevel + j * desc.mipLevelCount].data;
                        ::memcpy(workspace.data() + faceSizeInBytes * j, srcData, faceSizeInBytes);
                    }
                    srcDataPtr = workspace.data();
                }
                else
                {
                    const auto srcData = initData[mipLevel].data;
                    srcDataPtr = srcData;
                }
            }

            if (desc.arrayLength > 1)
            {
                SLANG_RHI_ASSERT(
                    desc.type == TextureType::Texture1D || desc.type == TextureType::Texture2D ||
                    desc.type == TextureType::TextureCube
                );

                CUDA_MEMCPY3D copyParam;
                memset(&copyParam, 0, sizeof(copyParam));

                copyParam.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                copyParam.dstArray = dstArray;

                copyParam.srcMemoryType = CU_MEMORYTYPE_HOST;
                copyParam.srcHost = srcDataPtr;
                copyParam.srcPitch = mipWidth * elementSize;
                copyParam.WidthInBytes = copyParam.srcPitch;
                copyParam.Height = mipHeight;
                // Set the depth to the array length
                copyParam.Depth = desc.arrayLength;

                if (desc.type == TextureType::TextureCube)
                {
                    copyParam.Depth *= 6;
                }

                SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy3D(&copyParam));
            }
            else
            {
                switch (desc.type)
                {
                case TextureType::Texture1D:
                case TextureType::Texture2D:
                {
                    CUDA_MEMCPY2D copyParam;
                    memset(&copyParam, 0, sizeof(copyParam));
                    copyParam.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                    copyParam.dstArray = dstArray;
                    copyParam.srcMemoryType = CU_MEMORYTYPE_HOST;
                    copyParam.srcHost = srcDataPtr;
                    copyParam.srcPitch = mipWidth * elementSize;
                    copyParam.WidthInBytes = copyParam.srcPitch;
                    copyParam.Height = mipHeight;
                    SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy2D(&copyParam));
                    break;
                }
                case TextureType::Texture3D:
                case TextureType::TextureCube:
                {
                    CUDA_MEMCPY3D copyParam;
                    memset(&copyParam, 0, sizeof(copyParam));

                    copyParam.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                    copyParam.dstArray = dstArray;

                    copyParam.srcMemoryType = CU_MEMORYTYPE_HOST;
                    copyParam.srcHost = srcDataPtr;
                    copyParam.srcPitch = mipWidth * elementSize;
                    copyParam.WidthInBytes = copyParam.srcPitch;
                    copyParam.Height = mipHeight;
                    copyParam.Depth = mipDepth;

                    SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy3D(&copyParam));
                    break;
                }

                default:
                {
                    SLANG_RHI_ASSERT_FAILURE("Not implemented");
                    break;
                }
                }
            }
        }
    }
    // Set up texture sampling parameters, and create final texture obj

    {
        CUDA_RESOURCE_DESC resDesc;
        memset(&resDesc, 0, sizeof(CUDA_RESOURCE_DESC));
        resDesc.resType = resourceType;

        if (tex->m_cudaArray)
        {
            resDesc.res.array.hArray = tex->m_cudaArray;
        }
        if (tex->m_cudaMipMappedArray)
        {
            resDesc.res.mipmap.hMipmappedArray = tex->m_cudaMipMappedArray;
        }

        // If the texture might be used as a UAV, then we need to allocate
        // a CUDA "surface" for it.
        //
        // Note: We cannot do this unconditionally, because it will fail
        // on surfaces that are not usable as UAVs (e.g., those with
        // mipmaps).
        //
        // TODO: We should really only be allocating the array at the
        // time we create a resource, and then allocate the surface or
        // texture objects as part of view creation.
        //
        if (is_set(desc.usage, TextureUsage::UnorderedAccess))
        {
            // On CUDA surfaces only support a single MIP map
            SLANG_RHI_ASSERT(desc.mipLevelCount == 1);

            SLANG_CUDA_RETURN_ON_FAIL(cuSurfObjectCreate(&tex->m_cudaSurfObj, &resDesc));
        }

        // Create handle for sampling.
        CUDA_TEXTURE_DESC texDesc;
        memset(&texDesc, 0, sizeof(CUDA_TEXTURE_DESC));
        texDesc.addressMode[0] = CU_TR_ADDRESS_MODE_WRAP;
        texDesc.addressMode[1] = CU_TR_ADDRESS_MODE_WRAP;
        texDesc.addressMode[2] = CU_TR_ADDRESS_MODE_WRAP;
        texDesc.filterMode = CU_TR_FILTER_MODE_LINEAR;
        texDesc.flags = CU_TRSF_NORMALIZED_COORDINATES;

        SLANG_CUDA_RETURN_ON_FAIL(cuTexObjectCreate(&tex->m_cudaTexObj, &resDesc, &texDesc, nullptr));
    }

    returnComPtr(outTexture, tex);
    return SLANG_OK;
}

Result DeviceImpl::createBuffer(const BufferDesc& descIn, const void* initData, IBuffer** outBuffer)
{
    auto desc = fixupBufferDesc(descIn);
    RefPtr<BufferImpl> buffer = new BufferImpl(desc);
    SLANG_CUDA_RETURN_ON_FAIL(cuMemAllocManaged((CUdeviceptr*)(&buffer->m_cudaMemory), desc.size, CU_MEM_ATTACH_GLOBAL)
    );
    if (initData)
    {
        SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy((CUdeviceptr)buffer->m_cudaMemory, (CUdeviceptr)initData, desc.size));
    }
    returnComPtr(outBuffer, buffer);
    return SLANG_OK;
}

Result DeviceImpl::createBufferFromSharedHandle(NativeHandle handle, const BufferDesc& desc, IBuffer** outBuffer)
{
    if (!handle)
    {
        *outBuffer = nullptr;
        return SLANG_OK;
    }

    RefPtr<BufferImpl> buffer = new BufferImpl(desc);

    // CUDA manages sharing of buffers through the idea of an
    // "external memory" object, which represents the relationship
    // with another API's objects. In order to create this external
    // memory association, we first need to fill in a descriptor struct.
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC externalMemoryHandleDesc;
    memset(&externalMemoryHandleDesc, 0, sizeof(externalMemoryHandleDesc));
    switch (handle.type)
    {
    case NativeHandleType::D3D12Resource:
        externalMemoryHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
        break;
    case NativeHandleType::Win32:
        externalMemoryHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        break;
    default:
        return SLANG_FAIL;
    }
    externalMemoryHandleDesc.handle.win32.handle = (void*)handle.value;
    externalMemoryHandleDesc.size = desc.size;
    externalMemoryHandleDesc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;

    // Once we have filled in the descriptor, we can request
    // that CUDA create the required association between the
    // external buffer and its own memory.
    CUexternalMemory externalMemory;
    SLANG_CUDA_RETURN_ON_FAIL(cuImportExternalMemory(&externalMemory, &externalMemoryHandleDesc));
    buffer->m_cudaExternalMemory = externalMemory;

    // The CUDA "external memory" handle is not itself a device
    // pointer, so we need to query for a suitable device address
    // for the buffer with another call.
    //
    // Just as for the external memory, we fill in a descriptor
    // structure (although in this case we only need to specify
    // the size).
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc;
    memset(&bufferDesc, 0, sizeof(bufferDesc));
    bufferDesc.size = desc.size;

    // Finally, we can "map" the buffer to get a device address.
    void* deviceAddress;
    SLANG_CUDA_RETURN_ON_FAIL(cuExternalMemoryGetMappedBuffer((CUdeviceptr*)&deviceAddress, externalMemory, &bufferDesc)
    );
    buffer->m_cudaMemory = deviceAddress;

    returnComPtr(outBuffer, buffer);
    return SLANG_OK;
}

Result DeviceImpl::createTextureFromSharedHandle(
    NativeHandle handle,
    const TextureDesc& desc,
    const size_t size,
    ITexture** outTexture
)
{
    if (!handle)
    {
        *outTexture = nullptr;
        return SLANG_OK;
    }

    RefPtr<TextureImpl> texture = new TextureImpl(desc);

    // CUDA manages sharing of buffers through the idea of an
    // "external memory" object, which represents the relationship
    // with another API's objects. In order to create this external
    // memory association, we first need to fill in a descriptor struct.
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC externalMemoryHandleDesc;
    memset(&externalMemoryHandleDesc, 0, sizeof(externalMemoryHandleDesc));
    switch (handle.type)
    {
    case NativeHandleType::D3D12Resource:
        externalMemoryHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
        break;
    case NativeHandleType::Win32:
        externalMemoryHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        break;
    default:
        return SLANG_FAIL;
    }
    externalMemoryHandleDesc.handle.win32.handle = (void*)handle.value;
    externalMemoryHandleDesc.size = size;
    externalMemoryHandleDesc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;

    CUexternalMemory externalMemory;
    SLANG_CUDA_RETURN_ON_FAIL(cuImportExternalMemory(&externalMemory, &externalMemoryHandleDesc));
    texture->m_cudaExternalMemory = externalMemory;

    const FormatInfo formatInfo = getFormatInfo(desc.format);
    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
    arrayDesc.Depth = desc.size.depth;
    arrayDesc.Height = desc.size.height;
    arrayDesc.Width = desc.size.width;
    arrayDesc.NumChannels = formatInfo.channelCount;
    getCUDAFormat(desc.format, &arrayDesc.Format);
    arrayDesc.Flags = 0; // TODO: Flags? CUDA_ARRAY_LAYERED/SURFACE_LDST/CUBEMAP/TEXTURE_GATHER

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC externalMemoryMipDesc;
    memset(&externalMemoryMipDesc, 0, sizeof(externalMemoryMipDesc));
    externalMemoryMipDesc.offset = 0;
    externalMemoryMipDesc.arrayDesc = arrayDesc;
    externalMemoryMipDesc.numLevels = desc.mipLevelCount;

    CUmipmappedArray mipArray;
    SLANG_CUDA_RETURN_ON_FAIL(cuExternalMemoryGetMappedMipmappedArray(&mipArray, externalMemory, &externalMemoryMipDesc)
    );
    texture->m_cudaMipMappedArray = mipArray;

    CUarray cuArray;
    SLANG_CUDA_RETURN_ON_FAIL(cuMipmappedArrayGetLevel(&cuArray, mipArray, 0));
    texture->m_cudaArray = cuArray;

    CUDA_RESOURCE_DESC surfDesc;
    memset(&surfDesc, 0, sizeof(surfDesc));
    surfDesc.resType = CU_RESOURCE_TYPE_ARRAY;
    surfDesc.res.array.hArray = cuArray;

    CUsurfObject surface;
    SLANG_CUDA_RETURN_ON_FAIL(cuSurfObjectCreate(&surface, &surfDesc));
    texture->m_cudaSurfObj = surface;

    // Create handle for sampling.
    CUDA_TEXTURE_DESC texDesc;
    memset(&texDesc, 0, sizeof(CUDA_TEXTURE_DESC));
    texDesc.addressMode[0] = CU_TR_ADDRESS_MODE_WRAP;
    texDesc.addressMode[1] = CU_TR_ADDRESS_MODE_WRAP;
    texDesc.addressMode[2] = CU_TR_ADDRESS_MODE_WRAP;
    texDesc.filterMode = CU_TR_FILTER_MODE_LINEAR;
    texDesc.flags = CU_TRSF_NORMALIZED_COORDINATES;

    SLANG_CUDA_RETURN_ON_FAIL(cuTexObjectCreate(&texture->m_cudaTexObj, &surfDesc, &texDesc, nullptr));

    returnComPtr(outTexture, texture);
    return SLANG_OK;
}

Result DeviceImpl::createTextureView(ITexture* texture, const TextureViewDesc& desc, ITextureView** outView)
{
    RefPtr<TextureViewImpl> view = new TextureViewImpl(desc);
    view->m_texture = checked_cast<TextureImpl*>(texture);
    if (view->m_desc.format == Format::Unknown)
        view->m_desc.format = view->m_texture->m_desc.format;
    view->m_desc.subresourceRange = view->m_texture->resolveSubresourceRange(desc.subresourceRange);
    returnComPtr(outView, view);
    return SLANG_OK;
}

Result DeviceImpl::createQueryPool(const QueryPoolDesc& desc, IQueryPool** outPool)
{
    switch (desc.type)
    {
    case QueryType::Timestamp:
    {
        RefPtr<QueryPoolImpl> pool = new QueryPoolImpl();
        SLANG_RETURN_ON_FAIL(pool->init(desc));
        returnComPtr(outPool, pool);
        return SLANG_OK;
    }
    case QueryType::AccelerationStructureCompactedSize:
    {
        RefPtr<PlainBufferProxyQueryPoolImpl> pool = new PlainBufferProxyQueryPoolImpl();
        SLANG_RETURN_ON_FAIL(pool->init(desc, this));
        returnComPtr(outPool, pool);
        return SLANG_OK;
        break;
    }
    default:
        return SLANG_FAIL;
    }
}

Result DeviceImpl::createShaderObjectLayout(
    slang::ISession* session,
    slang::TypeLayoutReflection* typeLayout,
    ShaderObjectLayout** outLayout
)
{
    RefPtr<ShaderObjectLayoutImpl> cudaLayout;
    cudaLayout = new ShaderObjectLayoutImpl(this, session, typeLayout);
    returnRefPtrMove(outLayout, cudaLayout);
    return SLANG_OK;
}

Result DeviceImpl::createShaderObject(ShaderObjectLayout* layout, IShaderObject** outObject)
{
    RefPtr<ShaderObjectImpl> result = new ShaderObjectImpl();
    SLANG_RETURN_ON_FAIL(result->init(this, dynamic_cast<ShaderObjectLayoutImpl*>(layout)));
    returnComPtr(outObject, result);
    return SLANG_OK;
}

Result DeviceImpl::createMutableShaderObject(ShaderObjectLayout* layout, IShaderObject** outObject)
{
    RefPtr<MutableShaderObjectImpl> result = new MutableShaderObjectImpl();
    SLANG_RETURN_ON_FAIL(result->init(this, dynamic_cast<ShaderObjectLayoutImpl*>(layout)));
    returnComPtr(outObject, result);
    return SLANG_OK;
}

Result DeviceImpl::createRootShaderObject(IShaderProgram* program, ShaderObjectBase** outObject)
{
    auto cudaProgram = dynamic_cast<ShaderProgramImpl*>(program);
    auto cudaLayout = cudaProgram->layout;

    RefPtr<RootShaderObjectImpl> result = new RootShaderObjectImpl();
    SLANG_RETURN_ON_FAIL(result->init(this, cudaLayout));
    returnRefPtrMove(outObject, result);
    return SLANG_OK;
}

Result DeviceImpl::createShaderProgram(
    const ShaderProgramDesc& desc,
    IShaderProgram** outProgram,
    ISlangBlob** outDiagnosticBlob
)
{
    // If this is a specializable program, we just keep a reference to the slang program and
    // don't actually create any kernels. This program will be specialized later when we know
    // the shader object bindings.
    RefPtr<ShaderProgramImpl> cudaProgram = new ShaderProgramImpl();
    cudaProgram->init(desc);
    if (desc.slangGlobalScope->getSpecializationParamCount() != 0)
    {
        cudaProgram->layout = new RootShaderObjectLayoutImpl(this, desc.slangGlobalScope->getLayout());
        returnComPtr(outProgram, cudaProgram);
        return SLANG_OK;
    }

    ComPtr<ISlangBlob> kernelCode;
    ComPtr<ISlangBlob> diagnostics;
    auto compileResult = getEntryPointCodeFromShaderCache(
        desc.slangGlobalScope,
        (SlangInt)0,
        0,
        kernelCode.writeRef(),
        diagnostics.writeRef()
    );
    if (diagnostics)
    {
        handleMessage(
            compileResult == SLANG_OK ? DebugMessageType::Warning : DebugMessageType::Error,
            DebugMessageSource::Slang,
            (char*)diagnostics->getBufferPointer()
        );
        if (outDiagnosticBlob)
            returnComPtr(outDiagnosticBlob, diagnostics);
    }
    SLANG_RETURN_ON_FAIL(compileResult);

    SLANG_CUDA_RETURN_ON_FAIL(cuModuleLoadData(&cudaProgram->cudaModule, kernelCode->getBufferPointer()));
    cudaProgram->kernelName = string::from_cstr(desc.slangGlobalScope->getLayout()->getEntryPointByIndex(0)->getName());
    SLANG_CUDA_RETURN_ON_FAIL(
        cuModuleGetFunction(&cudaProgram->cudaKernel, cudaProgram->cudaModule, cudaProgram->kernelName.data())
    );

    auto slangGlobalScope = desc.slangGlobalScope;
    if (slangGlobalScope)
    {
        cudaProgram->slangGlobalScope = slangGlobalScope;

        auto slangProgramLayout = slangGlobalScope->getLayout();
        if (!slangProgramLayout)
            return SLANG_FAIL;

        RefPtr<RootShaderObjectLayoutImpl> cudaLayout;
        cudaLayout = new RootShaderObjectLayoutImpl(this, slangProgramLayout);
        cudaLayout->programLayout = slangProgramLayout;
        cudaProgram->layout = cudaLayout;
    }

    returnComPtr(outProgram, cudaProgram);
    return SLANG_OK;
}

void* DeviceImpl::map(IBuffer* buffer)
{
    return checked_cast<BufferImpl*>(buffer)->m_cudaMemory;
}

void DeviceImpl::unmap(IBuffer* buffer)
{
    SLANG_UNUSED(buffer);
}

const DeviceInfo& DeviceImpl::getDeviceInfo() const
{
    return m_info;
}

Result DeviceImpl::createTransientResourceHeap(
    const ITransientResourceHeap::Desc& desc,
    ITransientResourceHeap** outHeap
)
{
    RefPtr<TransientResourceHeapImpl> result = new TransientResourceHeapImpl();
    SLANG_RETURN_ON_FAIL(result->init(this, desc));
    returnComPtr(outHeap, result);
    return SLANG_OK;
}

Result DeviceImpl::getQueue(QueueType type, ICommandQueue** outQueue)
{
    if (type != QueueType::Graphics)
        return SLANG_FAIL;
    m_queue->establishStrongReferenceToDevice();
    returnComPtr(outQueue, m_queue);
    return SLANG_OK;
}

Result DeviceImpl::createSampler(SamplerDesc const& desc, ISampler** outSampler)
{
    SLANG_UNUSED(desc);
    *outSampler = nullptr;
    return SLANG_OK;
}

Result DeviceImpl::createInputLayout(InputLayoutDesc const& desc, IInputLayout** outLayout)
{
    SLANG_UNUSED(desc);
    SLANG_UNUSED(outLayout);
    return SLANG_E_NOT_AVAILABLE;
}

Result DeviceImpl::createRenderPipeline(const RenderPipelineDesc& desc, IPipeline** outPipeline)
{
    SLANG_UNUSED(desc);
    SLANG_UNUSED(outPipeline);
    return SLANG_E_NOT_AVAILABLE;
}

Result DeviceImpl::readTexture(ITexture* texture, ISlangBlob** outBlob, size_t* outRowPitch, size_t* outPixelSize)
{
    auto textureImpl = checked_cast<TextureImpl*>(texture);

    const TextureDesc& desc = textureImpl->m_desc;
    auto width = desc.size.width;
    auto height = desc.size.height;
    const FormatInfo& formatInfo = getFormatInfo(desc.format);
    size_t pixelSize = formatInfo.blockSizeInBytes / formatInfo.pixelsPerBlock;
    size_t rowPitch = width * pixelSize;
    size_t size = height * rowPitch;

    auto blob = OwnedBlob::create(size);

    CUDA_MEMCPY2D copyParam;
    memset(&copyParam, 0, sizeof(copyParam));

    copyParam.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copyParam.srcArray = textureImpl->m_cudaArray;

    copyParam.dstMemoryType = CU_MEMORYTYPE_HOST;
    copyParam.dstHost = (void*)blob->getBufferPointer();
    copyParam.dstPitch = rowPitch;
    copyParam.WidthInBytes = copyParam.dstPitch;
    copyParam.Height = height;
    SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy2D(&copyParam));

    *outRowPitch = rowPitch;
    *outPixelSize = pixelSize;

    returnComPtr(outBlob, blob);
    return SLANG_OK;
}

Result DeviceImpl::readBuffer(IBuffer* buffer, size_t offset, size_t size, ISlangBlob** outBlob)
{
    auto bufferImpl = checked_cast<BufferImpl*>(buffer);

    auto blob = OwnedBlob::create(size);
    cuMemcpy((CUdeviceptr)blob->getBufferPointer(), (CUdeviceptr)((uint8_t*)bufferImpl->m_cudaMemory + offset), size);

    returnComPtr(outBlob, blob);
    return SLANG_OK;
}

Result DeviceImpl::getAccelerationStructureSizes(
    const AccelerationStructureBuildDesc& desc,
    AccelerationStructureSizes* outSizes
)
{
#if SLANG_RHI_ENABLE_OPTIX
    AccelerationStructureBuildInputBuilder builder;
    builder.build(desc, m_debugCallback);
    OptixAccelBufferSizes sizes;
    SLANG_OPTIX_RETURN_ON_FAIL(optixAccelComputeMemoryUsage(
        m_ctx.optixContext,
        &builder.buildOptions,
        builder.buildInputs.data(),
        builder.buildInputs.size(),
        &sizes
    ));
    outSizes->accelerationStructureSize = sizes.outputSizeInBytes;
    outSizes->scratchSize = sizes.tempSizeInBytes;
    outSizes->updateScratchSize = sizes.tempUpdateSizeInBytes;

    return SLANG_OK;
#else
    return SLANG_E_NOT_AVAILABLE;
#endif
}

Result DeviceImpl::createAccelerationStructure(
    const AccelerationStructureDesc& desc,
    IAccelerationStructure** outAccelerationStructure
)
{
#if SLANG_RHI_ENABLE_OPTIX
    RefPtr<AccelerationStructureImpl> result = new AccelerationStructureImpl(this, desc);
    SLANG_CUDA_RETURN_ON_FAIL(cuMemAlloc(&result->m_buffer, desc.size));
    SLANG_CUDA_RETURN_ON_FAIL(cuMemAlloc(&result->m_propertyBuffer, 8));
    returnComPtr(outAccelerationStructure, result);
    return SLANG_OK;
#else
    return SLANG_E_NOT_AVAILABLE;
#endif
}

} // namespace rhi::cuda
