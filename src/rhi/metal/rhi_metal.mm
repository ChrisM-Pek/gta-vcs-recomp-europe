// Metal backend of rhi.h (macOS on Apple Silicon, docs/MACOS.md). The only file that sees Metal. Objective-C++ with
// ARC. It keeps D3D11's behaviour, which the renderer was written against (rhi_d3d11.cpp), as the Vulkan backend does:
//
// - Metal's clip space is D3D's (y up, depth 0..1, pixel centres at +0.5): no flip.
// - Shaders are MSL text (tools/hlsl_msl.py: HLSL -> SPIR-V -> SPIRV-Cross), compiled here at run time with fast math
//   off - the Command Line Tools have no Metal compiler, the OS does. Bindings follow the D3D registers: cbuffer bN ->
//   [[buffer(N)]] in both stages, tN -> [[texture(N)]], sN -> [[sampler(N)]]; vertex buffers at buffer 30.
// - Unbound texture slots read zeros (a 1x1 black texture and a default sampler), as D3D's null views do; so does a
//   texture that is also the pass's target.
// - State (pipeline, buffers, textures, viewport, scissor) stays set across draws and passes, as in D3D11; beginPass
//   unbinds the textures and resets the viewport and scissor to the target. Each new render encoder gets it all again.
// - Dynamic buffers rewritten whole (WRITE_DISCARD): each update takes a new slice of the frame's upload ring, so
//   draws encoded earlier keep their data; a buffer drawn in a later frame without an update is copied again from its
//   CPU copy.
// - A pass's render encoder starts at its first draw; copies, uploads and readbacks inside a pass end it and the next
//   draw starts another that loads the contents (counted as suspends in PS2X_RHI_STATS: on a tile-based GPU each one
//   stores and reloads the target).
// - New render targets and depth textures start cleared to zero (D3D drivers hand them out that way).
// - Objects freed while the GPU may still use them stay alive: command buffers retain what they use. Freed objects
//   unbind themselves.
#include "rhi/rhi.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if !__has_feature(objc_arc)
#error "rhi_metal.mm is built with ARC (-fobjc-arc)"
#endif

namespace rhi
{
    namespace
    {
        constexpr uint32_t kFrames = 2;             // frames in flight
        constexpr uint32_t kUniformSlots = 4;       // cbuffer b0..b3
        constexpr uint32_t kTextureSlots = 8;       // t0..t7 / s0..s7
        constexpr uint32_t kVertexBufferIndex = 30; // tools/hlsl_msl.py
        constexpr size_t kRingChunk = 8u << 20;
        constexpr size_t kRingAlign = 256u;

        MTLPixelFormat pixelFormat(Format f)
        {
            switch (f)
            {
            case Format::RGBA8: return MTLPixelFormatRGBA8Unorm;
            case Format::R32F: return MTLPixelFormatR32Float;
            case Format::D32F: return MTLPixelFormatDepth32Float;
            default: return MTLPixelFormatInvalid;
            }
        }

        uint32_t bytesPerPixel(Format f) { return f == Format::Unknown ? 0u : 4u; }

        MTLBlendFactor blendFactor(BlendFactor f)
        {
            switch (f)
            {
            case BlendFactor::Zero: return MTLBlendFactorZero;
            case BlendFactor::One: return MTLBlendFactorOne;
            case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
            case BlendFactor::InvSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
            case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
            case BlendFactor::InvDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
            case BlendFactor::Src1Alpha: return MTLBlendFactorSource1Alpha;
            case BlendFactor::InvSrc1Alpha: return MTLBlendFactorOneMinusSource1Alpha;
            case BlendFactor::Constant: return MTLBlendFactorBlendColor;
            case BlendFactor::InvConstant: return MTLBlendFactorOneMinusBlendColor;
            }
            return MTLBlendFactorOne;
        }

        MTLBlendOperation blendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return MTLBlendOperationAdd;
            case BlendOp::Subtract: return MTLBlendOperationSubtract;
            case BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
            case BlendOp::Min: return MTLBlendOperationMin;
            case BlendOp::Max: return MTLBlendOperationMax;
            }
            return MTLBlendOperationAdd;
        }

        MTLCompareFunction compare(Compare c)
        {
            switch (c)
            {
            case Compare::Never: return MTLCompareFunctionNever;
            case Compare::Less: return MTLCompareFunctionLess;
            case Compare::Equal: return MTLCompareFunctionEqual;
            case Compare::LessEqual: return MTLCompareFunctionLessEqual;
            case Compare::Greater: return MTLCompareFunctionGreater;
            case Compare::NotEqual: return MTLCompareFunctionNotEqual;
            case Compare::GreaterEqual: return MTLCompareFunctionGreaterEqual;
            case Compare::Always: return MTLCompareFunctionAlways;
            }
            return MTLCompareFunctionAlways;
        }

        MTLVertexFormat vertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return MTLVertexFormatFloat;
            case VertexFormat::Float2: return MTLVertexFormatFloat2;
            case VertexFormat::Float3: return MTLVertexFormatFloat3;
            case VertexFormat::Float4: return MTLVertexFormatFloat4;
            case VertexFormat::UByte4Norm: return MTLVertexFormatUChar4Normalized;
            case VertexFormat::UInt1: return MTLVertexFormatUInt;
            }
            return MTLVertexFormatFloat4;
        }

        MTLPrimitiveType primitive(Topology t)
        {
            switch (t)
            {
            case Topology::Triangles: return MTLPrimitiveTypeTriangle;
            case Topology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
            case Topology::Lines: return MTLPrimitiveTypeLine;
            case Topology::Points: return MTLPrimitiveTypePoint;
            }
            return MTLPrimitiveTypeTriangle;
        }

        // the RHI's write mask is R = bit 0 ... A = bit 3 (D3D's); Metal's is A = bit 0 ... R = bit 3
        MTLColorWriteMask writeMask(uint8_t m)
        {
            MTLColorWriteMask out = MTLColorWriteMaskNone;
            if (m & 1u)
                out |= MTLColorWriteMaskRed;
            if (m & 2u)
                out |= MTLColorWriteMaskGreen;
            if (m & 4u)
                out |= MTLColorWriteMaskBlue;
            if (m & 8u)
                out |= MTLColorWriteMaskAlpha;
            return out;
        }

        NSString *nsString(const char *s) { return s ? [NSString stringWithUTF8String:s] : nil; }

        class MetalDevice;

        class MtlTexture final : public Texture
        {
        public:
            MtlTexture(MetalDevice *owner, const TextureDesc &desc) : owner(owner) { m_desc = desc; }
            ~MtlTexture() override;
            MetalDevice *owner;
            id<MTLTexture> texture = nil;
        };

        class MtlBuffer final : public Buffer
        {
        public:
            MtlBuffer(MetalDevice *owner, const BufferDesc &desc) : owner(owner) { m_desc = desc; }
            ~MtlBuffer() override;
            MetalDevice *owner;
            id<MTLBuffer> buffer = nil;      // static buffers (dynamic = false): their own storage
            std::vector<uint8_t> shadow;     // dynamic buffers: the CPU copy, placed in the frame's ring when used
            id<MTLBuffer> placed = nil;      // ... where the current contents are this frame
            size_t placedOffset = 0;
            uint64_t placedFrame = ~0ull;
        };

        class MtlSampler final : public Sampler
        {
        public:
            id<MTLSamplerState> state = nil;
        };

        class MtlPipeline final : public Pipeline
        {
        public:
            MtlPipeline(MetalDevice *owner) : owner(owner) {}
            ~MtlPipeline() override;
            MetalDevice *owner;
            PipelineDesc desc;
            id<MTLFunction> vs = nil, ps = nil;
            MTLVertexDescriptor *vertexDescriptor = nil;
            MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
            std::unordered_map<uint32_t, id<MTLRenderPipelineState>> variants; // by (colour, depth, samples)
            bool failed = false;
        };

        struct Ring
        {
            struct Chunk
            {
                id<MTLBuffer> buffer;
                size_t used = 0;
            };
            std::vector<Chunk> chunks;
            size_t current = 0;

            void reset()
            {
                for (Chunk &c : chunks)
                    c.used = 0;
                current = 0;
                // keep at most four chunks (32 MB) a frame between bursts
                if (chunks.size() > 4u)
                    chunks.resize(4u);
            }
        };

        class MetalDevice final : public Device
        {
        public:
            MetalDevice() = default;

            bool init(const DeviceDesc &desc, std::string &error)
            {
                id<MTLDevice> device = nil;
                NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
                for (id<MTLDevice> d in all)
                    if (desc.gpu && gpuIdOf(d) == desc.gpu)
                        device = d;
                if (!device)
                    device = MTLCreateSystemDefaultDevice();
                if (!device)
                {
                    error = "no Metal device";
                    return false;
                }
                m_device = device;
                m_adapterName = [[device name] UTF8String];
                m_queue = [device newCommandQueueWithMaxCommandBufferCount:64];
                m_frameSemaphore = dispatch_semaphore_create(kFrames);
                m_debug = desc.debug;
                m_stats = std::getenv("PS2X_RHI_STATS") != nullptr;
                m_width = std::max(1u, desc.width);
                m_height = std::max(1u, desc.height);
                logGpus(all, device);

                if (desc.metalLayer && !desc.offscreen)
                {
                    m_layer = (__bridge CAMetalLayer *)desc.metalLayer;
                    m_layer.device = device;
                    m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                    m_layer.framebufferOnly = YES;
                    m_layer.maximumDrawableCount = 3;
                    m_layer.displaySyncEnabled = YES;
                    m_layer.drawableSize = CGSizeMake(m_width, m_height);
                    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                    m_layer.colorspace = srgb; // the values as they are, shown as sRGB: as D3D11's and Vulkan's UNORM swapchains
                    CGColorSpaceRelease(srgb);
                }
                else
                    makeOffscreenBack();

                // what an unbound slot reads (D3D's null views read zeros)
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:1
                                                                                             height:1
                                                                                          mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead;
                m_dummyTexture = [device newTextureWithDescriptor:td];
                const uint32_t black = 0;
                [m_dummyTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
                MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
                m_defaultSampler = [device newSamplerStateWithDescriptor:sd];
                m_zeroBuffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                std::memset([m_zeroBuffer contents], 0, 4096);
                return true;
            }

            ~MetalDevice() override
            {
                endEncoders();
                if (m_cmd)
                {
                    [m_cmd commit];
                    [m_cmd waitUntilCompleted];
                    m_cmd = nil;
                }
                // let the frames in flight finish
                for (uint32_t i = 0; i < kFrames; ++i)
                    dispatch_semaphore_wait(m_frameSemaphore, DISPATCH_TIME_FOREVER);
                for (uint32_t i = 0; i < kFrames; ++i)
                    dispatch_semaphore_signal(m_frameSemaphore);
            }

            Backend backend() const override { return Backend::Metal; }
            const std::string &adapterName() const override { return m_adapterName; }

            static int gpuIdOf(id<MTLDevice> d)
            {
                const uint64_t r = [d registryID];
                return gpuId(0x106Bu, static_cast<uint32_t>(r ^ (r >> 32)), 0);
            }

            void logGpus(NSArray<id<MTLDevice>> *all, id<MTLDevice> used)
            {
                std::string line = "[rhi] graphics cards, fastest first:";
                for (id<MTLDevice> d in all)
                    line += std::string(" ") + [[d name] UTF8String] + ([d hasUnifiedMemory] ? " (integrated)" : "") + " [" +
                            std::to_string(gpuIdOf(d)) + "]";
                line += std::string("; using ") + [[used name] UTF8String];
                std::fprintf(stderr, "%s\n", line.c_str());
            }

            // ---- resources ----------------------------------------------------------------------------------------
            std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *data, uint32_t rowPitch) override
            {
                auto t = std::make_unique<MtlTexture>(this, desc);
                const uint32_t w = std::max(1u, desc.width), h = std::max(1u, desc.height);
                const bool msaa = desc.samples > 1u;
                const bool target = (desc.usage & (UsageRenderTarget | UsageDepth)) != 0u;
                const bool mips = desc.mipLevels != 1u && !msaa;
                uint32_t levels = 1u;
                if (mips)
                    levels = desc.mipLevels ? desc.mipLevels : 1u + static_cast<uint32_t>(std::floor(std::log2(std::max(w, h))));
                MTLTextureDescriptor *td = [MTLTextureDescriptor new];
                td.textureType = msaa ? MTLTextureType2DMultisample : MTLTextureType2D;
                td.pixelFormat = pixelFormat(desc.format);
                td.width = w;
                td.height = h;
                td.mipmapLevelCount = levels;
                td.sampleCount = msaa ? sampleCount(desc.samples) : 1u;
                td.usage = MTLTextureUsageShaderRead;
                if (target || mips)
                    td.usage |= MTLTextureUsageRenderTarget;
                td.storageMode = (target || msaa) ? MTLStorageModePrivate : MTLStorageModeShared;
                t->texture = [m_device newTextureWithDescriptor:td];
                if (!t->texture)
                {
                    std::fprintf(stderr, "[rhi] metal: texture %ux%u failed\n", w, h);
                    return nullptr;
                }
                if (desc.name)
                    t->texture.label = nsString(desc.name);
                const uint32_t bpp = bytesPerPixel(desc.format);
                if (data && !target)
                    [t->texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                                  mipmapLevel:0
                                    withBytes:data
                                  bytesPerRow:rowPitch ? rowPitch : w * bpp];
                if (target)
                    clearNew(*t); // new targets read zero, as D3D drivers give them
                if (target && data)
                    updateTexture(*t, 0, 0, w, h, data, rowPitch);
                if (mips && data)
                    generateMips(*t);
                return t;
            }

            uint32_t sampleCount(uint32_t wanted)
            {
                uint32_t n = wanted;
                while (n > 1u && ![m_device supportsTextureSampleCount:n])
                    n >>= 1u;
                if (n != wanted && !m_msaaClampReported)
                {
                    m_msaaClampReported = true;
                    std::fprintf(stderr, "[rhi] metal: %ux MSAA is not supported here: %ux\n", wanted, n);
                }
                return std::max(1u, n);
            }

            void updateTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void *data,
                               uint32_t rowPitch) override
            {
                auto &t = static_cast<MtlTexture &>(texture);
                if (!width || !height)
                    return;
                const uint32_t bpp = bytesPerPixel(t.desc().format);
                const uint32_t pitch = rowPitch ? rowPitch : width * bpp;
                const size_t row = static_cast<size_t>(width) * bpp;
                id<MTLBuffer> buf;
                size_t offset;
                uint8_t *dst = ringAlloc(row * height, buf, offset);
                for (uint32_t r = 0; r < height; ++r)
                    std::memcpy(dst + r * row, static_cast<const uint8_t *>(data) + static_cast<size_t>(r) * pitch, row);
                id<MTLBlitCommandEncoder> blit = blitEncoder();
                [blit copyFromBuffer:buf
                           sourceOffset:offset
                      sourceBytesPerRow:row
                    sourceBytesPerImage:row * height
                             sourceSize:MTLSizeMake(width, height, 1)
                              toTexture:t.texture
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(x, y, 0)];
                m_uploadBytes += row * height;
            }

            bool readTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *out,
                             uint32_t rowPitch) override
            {
                auto &t = static_cast<MtlTexture &>(texture);
                if (!width || !height || t.desc().samples > 1u)
                    return false;
                const uint32_t bpp = bytesPerPixel(t.desc().format);
                const size_t row = static_cast<size_t>(width) * bpp;
                id<MTLBuffer> staging = [m_device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> blit = blitEncoder();
                [blit copyFromTexture:t.texture
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(x, y, 0)
                                  sourceSize:MTLSizeMake(width, height, 1)
                                    toBuffer:staging
                           destinationOffset:0
                      destinationBytesPerRow:row
                    destinationBytesPerImage:row * height];
                flushAndWait();
                const uint32_t pitch = rowPitch ? rowPitch : static_cast<uint32_t>(row);
                const uint8_t *src = static_cast<const uint8_t *>([staging contents]);
                for (uint32_t r = 0; r < height; ++r)
                    std::memcpy(static_cast<uint8_t *>(out) + static_cast<size_t>(r) * pitch, src + r * row, row);
                return true;
            }

            void copyTexture(Texture &dst, uint32_t dstX, uint32_t dstY, Texture &src, uint32_t srcX, uint32_t srcY, uint32_t width,
                             uint32_t height) override
            {
                if (!width || !height)
                    return;
                id<MTLBlitCommandEncoder> blit = blitEncoder();
                [blit copyFromTexture:static_cast<MtlTexture &>(src).texture
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(srcX, srcY, 0)
                           sourceSize:MTLSizeMake(width, height, 1)
                            toTexture:static_cast<MtlTexture &>(dst).texture
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(dstX, dstY, 0)];
            }

            void resolveTexture(Texture &dst, Texture &src) override
            {
                auto &s = static_cast<MtlTexture &>(src);
                auto &d = static_cast<MtlTexture &>(dst);
                if (s.desc().samples <= 1u || [s.texture sampleCount] <= 1u)
                {
                    copyTexture(dst, 0, 0, src, 0, 0, s.desc().width, s.desc().height);
                    return;
                }
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                if (s.desc().format == Format::D32F)
                {
                    rp.depthAttachment.texture = s.texture;
                    rp.depthAttachment.resolveTexture = d.texture;
                    rp.depthAttachment.loadAction = MTLLoadActionLoad;
                    rp.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
                }
                else
                {
                    rp.colorAttachments[0].texture = s.texture;
                    rp.colorAttachments[0].resolveTexture = d.texture;
                    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                }
                ensureCommandBuffer();
                endEncoders();
                [[m_cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
                ++m_suspendsThisFrame;
            }

            void generateMips(Texture &texture) override
            {
                auto &t = static_cast<MtlTexture &>(texture);
                if ([t.texture mipmapLevelCount] > 1u)
                    [blitEncoder() generateMipmapsForTexture:t.texture];
            }

            bool freeThreadedCreation() const override { return true; } // MTLDevice is thread-safe; a new texture is no one's yet

            std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t levelCount) override
            {
                TextureDesc sized = desc;
                sized.width = std::max(1u, desc.width);
                sized.height = std::max(1u, desc.height);
                sized.mipLevels = std::min(std::max(1u, levelCount), 16u);
                auto t = std::make_unique<MtlTexture>(this, sized);
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:sized.width
                                                                                             height:sized.height
                                                                                          mipmapped:NO];
                td.mipmapLevelCount = sized.mipLevels;
                td.usage = MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                t->texture = [m_device newTextureWithDescriptor:td];
                if (!t->texture)
                    return nullptr;
                if (desc.name)
                    t->texture.label = nsString(desc.name);
                for (uint32_t i = 0; i < sized.mipLevels; ++i)
                {
                    const uint32_t w = std::max(1u, sized.width >> i), h = std::max(1u, sized.height >> i);
                    [t->texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:i withBytes:levels[i] bytesPerRow:w * 4u];
                }
                return t;
            }

            std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *data) override
            {
                auto b = std::make_unique<MtlBuffer>(this, desc);
                const size_t size = std::max<size_t>(desc.size, 16u);
                if (desc.dynamic)
                {
                    b->shadow.assign(size, 0);
                    if (data)
                        std::memcpy(b->shadow.data(), data, desc.size);
                    return b;
                }
                b->buffer = data ? [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared]
                                 : [m_device newBufferWithLength:size options:MTLResourceStorageModeShared];
                if (!b->buffer)
                    return nullptr;
                if (!data)
                    std::memset([b->buffer contents], 0, size);
                if (desc.name)
                    b->buffer.label = nsString(desc.name);
                return b;
            }

            void updateBuffer(Buffer &buffer, const void *data, size_t size) override
            {
                auto &b = static_cast<MtlBuffer &>(buffer);
                size = std::min(size, b.desc().size);
                if (b.desc().dynamic)
                {
                    std::memcpy(b.shadow.data(), data, size);
                    placeBuffer(b, size);
                    m_uploadBytes += size;
                    markBufferRebind(&b);
                    return;
                }
                // a static buffer: in command order (D3D's UpdateSubresource)
                id<MTLBuffer> src;
                size_t offset;
                std::memcpy(ringAlloc(size, src, offset), data, size);
                [blitEncoder() copyFromBuffer:src sourceOffset:offset toBuffer:b.buffer destinationOffset:0 size:(size + 3u) & ~size_t(3)];
                m_uploadBytes += size;
            }

            std::unique_ptr<Sampler> createSampler(const SamplerDesc &desc) override
            {
                auto s = std::make_unique<MtlSampler>();
                MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
                const bool linear = desc.filter == Filter::Linear;
                sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                sd.mipFilter = linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
                if (linear && desc.anisotropy > 1u)
                    sd.maxAnisotropy = std::min(desc.anisotropy, 16u);
                sd.sAddressMode = desc.wrapU == Wrap::Repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
                sd.tAddressMode = desc.wrapV == Wrap::Repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
                sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
                s->state = [m_device newSamplerStateWithDescriptor:sd];
                return s->state ? std::move(s) : nullptr;
            }

            id<MTLFunction> function(const ShaderCode &code, const char *pipelineName)
            {
                if (!code.data || !code.size)
                    return nil;
                auto it = m_functions.find(code.data);
                if (it != m_functions.end())
                    return it->second;
                NSString *source = [[NSString alloc] initWithBytes:code.data
                                                            length:strnlen(static_cast<const char *>(code.data), code.size)
                                                          encoding:NSUTF8StringEncoding];
                MTLCompileOptions *options = [MTLCompileOptions new];
                // the PS2's numbers, not fast math's (it reassociates, flushes and ignores NaN / infinity)
                if (@available(macOS 15.0, *))
                    options.mathMode = MTLMathModeSafe;
                else
                {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
                }
                options.languageVersion = MTLLanguageVersion3_0;
                NSError *error = nil;
                const auto start = std::chrono::steady_clock::now();
                id<MTLLibrary> library = [m_device newLibraryWithSource:source options:options error:&error];
                m_compileMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                id<MTLFunction> fn = library ? [library newFunctionWithName:@"vcs_main"] : nil;
                if (!fn)
                    std::fprintf(stderr, "[rhi] metal: shader of pipeline '%s' rejected: %s\n", pipelineName ? pipelineName : "?",
                                 error ? [[error localizedDescription] UTF8String] : "no vcs_main");
                m_functions[code.data] = fn;
                return fn;
            }

            std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &desc) override
            {
                auto p = std::make_unique<MtlPipeline>(this);
                p->desc = desc;
                p->vs = function(desc.vertex, desc.name);
                p->ps = function(desc.pixel, desc.name);
                if (!p->vs || !p->ps)
                    return nullptr;
                if (!desc.attributes.empty())
                {
                    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
                    for (size_t i = 0; i < desc.attributes.size(); ++i)
                    {
                        vd.attributes[i].format = vertexFormat(desc.attributes[i].format);
                        vd.attributes[i].offset = desc.attributes[i].offset;
                        vd.attributes[i].bufferIndex = kVertexBufferIndex;
                    }
                    vd.layouts[kVertexBufferIndex].stride = desc.stride;
                    vd.layouts[kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
                    p->vertexDescriptor = vd;
                }
                p->primitive = primitive(desc.topology);
                if (std::getenv("VCS_METAL_EAGER_PIPELINES")) // the usual pass formats now, not at the first draw
                {
                    variant(*p, MTLPixelFormatRGBA8Unorm, MTLPixelFormatInvalid, 1);
                    variant(*p, MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float, 1);
                }
                return p;
            }

            id<MTLRenderPipelineState> variant(MtlPipeline &p, MTLPixelFormat color, MTLPixelFormat depth, uint32_t samples)
            {
                const uint32_t key = (static_cast<uint32_t>(color) << 16) ^ (static_cast<uint32_t>(depth) << 4) ^ samples;
                auto it = p.variants.find(key);
                if (it != p.variants.end())
                    return it->second;
                MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = p.vs;
                rd.fragmentFunction = p.ps;
                rd.vertexDescriptor = p.vertexDescriptor;
                rd.rasterSampleCount = samples;
                if (p.desc.name)
                    rd.label = nsString(p.desc.name);
                MTLRenderPipelineColorAttachmentDescriptor *ca = rd.colorAttachments[0];
                ca.pixelFormat = color;
                if (color != MTLPixelFormatInvalid)
                {
                    const BlendState &b = p.desc.blend;
                    ca.blendingEnabled = b.enable;
                    ca.sourceRGBBlendFactor = blendFactor(b.srcColor);
                    ca.destinationRGBBlendFactor = blendFactor(b.dstColor);
                    ca.rgbBlendOperation = blendOp(b.colorOp);
                    ca.sourceAlphaBlendFactor = blendFactor(b.srcAlpha);
                    ca.destinationAlphaBlendFactor = blendFactor(b.dstAlpha);
                    ca.alphaBlendOperation = blendOp(b.alphaOp);
                    ca.writeMask = writeMask(b.writeMask);
                }
                rd.depthAttachmentPixelFormat = depth;
                // points read the vertex shader's point size; any other class with one written is refused, so the
                // rest stay unspecified
                if (p.primitive == MTLPrimitiveTypePoint)
                    rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
                NSError *error = nil;
                const auto start = std::chrono::steady_clock::now();
                id<MTLRenderPipelineState> state = [m_device newRenderPipelineStateWithDescriptor:rd error:&error];
                m_compileMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (!state)
                    std::fprintf(stderr, "[rhi] metal: pipeline '%s' (colour %d, depth %d, x%u) failed: %s\n",
                                 p.desc.name ? p.desc.name : "?", static_cast<int>(color), static_cast<int>(depth), samples,
                                 error ? [[error localizedDescription] UTF8String] : "?");
                p.variants[key] = state;
                return state;
            }

            id<MTLDepthStencilState> depthState(const DepthState &d, bool haveDepth)
            {
                const bool test = haveDepth && (d.test || d.write);
                const bool write = haveDepth && d.write;
                const MTLCompareFunction func = test && d.test ? compare(d.func) : MTLCompareFunctionAlways;
                const uint32_t key = (test ? 1u : 0u) | (write ? 2u : 0u) | (static_cast<uint32_t>(func) << 2);
                auto it = m_depthStates.find(key);
                if (it != m_depthStates.end())
                    return it->second;
                MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
                dd.depthCompareFunction = func;
                dd.depthWriteEnabled = write;
                id<MTLDepthStencilState> s = [m_device newDepthStencilStateWithDescriptor:dd];
                m_depthStates[key] = s;
                return s;
            }

            // ---- drawing ------------------------------------------------------------------------------------------
            void beginPass(const PassDesc &pass) override
            {
                if (m_inPass)
                    endPass();
                ensureCommandBuffer();
                m_pass = pass;
                m_inPass = true;
                m_passStarted = false;
                m_passSkip = false;
                m_passColor = nil;
                m_passDepth = pass.depth ? static_cast<MtlTexture *>(pass.depth)->texture : nil;
                uint32_t w = m_width, h = m_height;
                if (!pass.depthOnly)
                {
                    if (pass.color)
                    {
                        m_passColor = static_cast<MtlTexture *>(pass.color)->texture;
                        w = pass.color->desc().width;
                        h = pass.color->desc().height;
                    }
                    else if (m_layer)
                    {
                        if (!m_drawable)
                            m_drawable = [m_layer nextDrawable];
                        if (!m_drawable)
                            m_passSkip = true; // occluded or minimised: nothing to draw into this frame
                        else
                        {
                            m_passColor = m_drawable.texture;
                            w = static_cast<uint32_t>([m_passColor width]);
                            h = static_cast<uint32_t>([m_passColor height]);
                        }
                    }
                    else
                        m_passColor = m_offscreenBack;
                }
                else if (pass.depth)
                {
                    w = pass.depth->desc().width;
                    h = pass.depth->desc().height;
                }
                m_passWidth = w;
                m_passHeight = h;
                m_passColorFormat = m_passColor ? [m_passColor pixelFormat] : MTLPixelFormatInvalid;
                m_passDepthFormat = m_passDepth ? [m_passDepth pixelFormat] : MTLPixelFormatInvalid;
                m_passSamples = m_passColor ? static_cast<uint32_t>([m_passColor sampleCount])
                                            : m_passDepth ? static_cast<uint32_t>([m_passDepth sampleCount]) : 1u;
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                    m_textures[i] = {};
                m_texturesDirty = (1u << kTextureSlots) - 1u;
                setViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                setScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
                ++m_passesThisFrame;
            }

            void endPass() override
            {
                if (!m_inPass)
                    return;
                if (!m_passStarted && !m_passSkip && (m_pass.clearColor || m_pass.clearDepth))
                    startRendering(); // a pass with clears and no draws
                endEncoders();
                m_inPass = false;
                m_passColor = nil;
                m_passDepth = nil;
            }

            void setViewport(float x, float y, float width, float height) override
            {
                m_viewport = {x, y, width, height, 0.0, 1.0};
                m_viewportDirty = true;
            }

            void setScissor(int x, int y, int width, int height) override
            {
                // Metal wants it inside the target: clamped (an empty one draws nothing)
                const int x0 = std::clamp(x, 0, static_cast<int>(m_passWidth));
                const int y0 = std::clamp(y, 0, static_cast<int>(m_passHeight));
                const int x1 = std::clamp(x + std::max(width, 0), 0, static_cast<int>(m_passWidth));
                const int y1 = std::clamp(y + std::max(height, 0), 0, static_cast<int>(m_passHeight));
                m_scissorEmpty = x1 <= x0 || y1 <= y0;
                m_scissor = {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(std::max(x1 - x0, 1)),
                             static_cast<NSUInteger>(std::max(y1 - y0, 1))};
                m_scissorDirty = true;
            }

            void setBlendConstant(const float rgba[4]) override
            {
                std::memcpy(m_blendConstant, rgba, sizeof(m_blendConstant));
                m_blendDirty = true;
            }

            void setPipeline(Pipeline &pipeline) override
            {
                m_pipeline = &static_cast<MtlPipeline &>(pipeline);
                m_pipelineDirty = true;
            }

            void setVertexBuffer(Buffer &buffer, uint32_t offset) override
            {
                m_vertexBuffer = &static_cast<MtlBuffer &>(buffer);
                m_vertexOffset = offset;
                m_vertexDirty = true;
            }

            void setIndexBuffer(Buffer &buffer, bool uint32Indices) override
            {
                m_indexBuffer = &static_cast<MtlBuffer &>(buffer);
                m_index32 = uint32Indices;
            }

            void setTexture(uint32_t slot, Texture *texture, Sampler *sampler) override
            {
                if (slot >= kTextureSlots)
                    return;
                m_textures[slot].texture = texture ? static_cast<MtlTexture *>(texture)->texture : nil;
                m_textures[slot].sampler = sampler ? static_cast<MtlSampler *>(sampler)->state : nil;
                m_texturesDirty |= 1u << slot;
            }

            void setConstants(uint32_t slot, Buffer &buffer) override
            {
                if (slot >= kUniformSlots)
                    return;
                m_constants[slot] = &static_cast<MtlBuffer &>(buffer);
                m_constantsDirty |= 1u << slot;
            }

            void draw(uint32_t vertexCount, uint32_t firstVertex) override
            {
                if (!vertexCount || !prepareDraw())
                    return;
                [m_enc drawPrimitives:m_pipeline->primitive vertexStart:firstVertex vertexCount:vertexCount];
                ++m_drawsThisFrame;
            }

            void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex) override
            {
                if (!indexCount || !m_indexBuffer || !prepareDraw())
                    return;
                const size_t indexSize = m_index32 ? 4u : 2u;
                id<MTLBuffer> buf;
                size_t offset;
                resolveBuffer(*m_indexBuffer, buf, offset);
                offset += static_cast<size_t>(firstIndex) * indexSize;
                if (offset & 3u) // Metal wants a 4-byte aligned index offset: those indices again, aligned
                {
                    const uint8_t *src = static_cast<const uint8_t *>([buf contents]) + offset;
                    id<MTLBuffer> aligned;
                    size_t at;
                    std::memcpy(ringAlloc(indexCount * indexSize, aligned, at), src, indexCount * indexSize);
                    buf = aligned;
                    offset = at;
                }
                [m_enc drawIndexedPrimitives:m_pipeline->primitive
                                  indexCount:indexCount
                                   indexType:m_index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                 indexBuffer:buf
                           indexBufferOffset:offset
                               instanceCount:1
                                  baseVertex:baseVertex
                                baseInstance:0];
                ++m_drawsThisFrame;
            }

            void pushMarker(const char *) override {}
            void popMarker() override {}

            // ---- swapchain ----------------------------------------------------------------------------------------
            void backbufferSize(uint32_t &width, uint32_t &height) const override
            {
                width = m_width;
                height = m_height;
            }

            void resize(uint32_t width, uint32_t height) override
            {
                if (!width || !height || (width == m_width && height == m_height))
                    return;
                m_width = width;
                m_height = height;
                if (m_layer)
                    m_layer.drawableSize = CGSizeMake(width, height);
                else
                    makeOffscreenBack();
            }

            void present(PresentMode mode) override
            {
                if (m_inPass)
                    endPass();
                ensureCommandBuffer();
                endEncoders();
                if (m_layer)
                {
                    const BOOL sync = mode == PresentMode::Vsync;
                    if (m_layer.displaySyncEnabled != sync)
                        m_layer.displaySyncEnabled = sync;
                    if (m_drawable)
                        [m_cmd presentDrawable:m_drawable];
                }
                dispatch_semaphore_t sem = m_frameSemaphore;
                [m_cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
                  dispatch_semaphore_signal(sem);
                }];
                [m_cmd commit];
                m_cmd = nil;
                m_drawable = nil;
                m_frameOpen = false;
                m_pipelineDirty = true;
                reportStats();
            }

            // ---- Dear ImGui: drawn through the RHI (render/ui_renderer.cpp) ----------------------------------------
            bool uiInit() override { return false; }
            void uiNewFrame() override {}
            void uiRender(void *) override {}
            void uiShutdown() override {}

            // ---- freed objects unbind themselves -------------------------------------------------------------------
            void forget(MtlBuffer *b)
            {
                if (m_vertexBuffer == b)
                    m_vertexBuffer = nullptr;
                if (m_indexBuffer == b)
                    m_indexBuffer = nullptr;
                for (MtlBuffer *&c : m_constants)
                    if (c == b)
                        c = nullptr;
            }

            void forget(MtlPipeline *p)
            {
                if (m_pipeline == p)
                    m_pipeline = nullptr;
            }

        private:
            struct BoundTexture
            {
                id<MTLTexture> texture = nil;
                id<MTLSamplerState> sampler = nil;
            };

            void makeOffscreenBack()
            {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:m_width
                                                                                             height:m_height
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModePrivate;
                m_offscreenBack = [m_device newTextureWithDescriptor:td];
                m_offscreenBack.label = @"back buffer (offscreen)";
            }

            void ensureCommandBuffer()
            {
                if (m_cmd)
                    return;
                if (!m_frameOpen)
                {
                    // at most kFrames frames in flight: the ring of the frame kFrames ago is free once it has passed
                    dispatch_semaphore_wait(m_frameSemaphore, DISPATCH_TIME_FOREVER);
                    m_frameOpen = true;
                    m_frameIndex = (m_frameIndex + 1u) % kFrames;
                    ++m_frameSerial;
                    m_rings[m_frameIndex].reset();
                    m_vertexDirty = true;
                    m_constantsDirty = (1u << kUniformSlots) - 1u;
                }
                m_cmd = [m_queue commandBuffer];
            }

            void endEncoders()
            {
                if (m_enc)
                {
                    [m_enc endEncoding];
                    m_enc = nil;
                }
                if (m_blit)
                {
                    [m_blit endEncoding];
                    m_blit = nil;
                }
            }

            id<MTLBlitCommandEncoder> blitEncoder()
            {
                ensureCommandBuffer();
                if (m_enc)
                {
                    [m_enc endEncoding];
                    m_enc = nil;
                    ++m_suspendsThisFrame;
                }
                if (!m_blit)
                    m_blit = [m_cmd blitCommandEncoder];
                return m_blit;
            }

            // everything encoded so far, run to the end (readbacks); the frame goes on in a new command buffer
            void flushAndWait()
            {
                if (!m_cmd)
                    return;
                endEncoders();
                [m_cmd commit];
                [m_cmd waitUntilCompleted];
                m_cmd = nil;
                ensureCommandBuffer();
            }

            void clearNew(MtlTexture &t)
            {
                ensureCommandBuffer();
                endEncoders();
                if (m_inPass && m_passStarted)
                    ++m_suspendsThisFrame;
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                if (t.desc().format == Format::D32F)
                {
                    rp.depthAttachment.texture = t.texture;
                    rp.depthAttachment.loadAction = MTLLoadActionClear;
                    rp.depthAttachment.clearDepth = 0.0;
                    rp.depthAttachment.storeAction = MTLStoreActionStore;
                }
                else
                {
                    rp.colorAttachments[0].texture = t.texture;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                [[m_cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
            }

            bool startRendering()
            {
                if (m_enc)
                    return true;
                if (!m_inPass || m_passSkip || (!m_passColor && !m_passDepth))
                    return false;
                ensureCommandBuffer();
                if (m_blit)
                {
                    [m_blit endEncoding];
                    m_blit = nil;
                }
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                const bool first = !m_passStarted;
                if (m_passColor)
                {
                    rp.colorAttachments[0].texture = m_passColor;
                    rp.colorAttachments[0].loadAction = first && m_pass.clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(m_pass.clearRgba[0], m_pass.clearRgba[1], m_pass.clearRgba[2],
                                                                          m_pass.clearRgba[3]);
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                if (m_passDepth)
                {
                    rp.depthAttachment.texture = m_passDepth;
                    rp.depthAttachment.loadAction = first && m_pass.clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
                    rp.depthAttachment.clearDepth = m_pass.clearDepthValue;
                    rp.depthAttachment.storeAction = MTLStoreActionStore;
                }
                m_enc = [m_cmd renderCommandEncoderWithDescriptor:rp];
                if (m_pass.name)
                    m_enc.label = nsString(m_pass.name);
                m_passStarted = true;
                // a new encoder has no state
                m_pipelineDirty = m_viewportDirty = m_scissorDirty = m_blendDirty = m_vertexDirty = true;
                m_constantsDirty = (1u << kUniformSlots) - 1u;
                m_texturesDirty = (1u << kTextureSlots) - 1u;
                return true;
            }

            bool prepareDraw()
            {
                if (!m_pipeline || m_scissorEmpty || !startRendering())
                    return false;
                if (m_pipelineDirty)
                {
                    id<MTLRenderPipelineState> state = variant(*m_pipeline, m_passColorFormat, m_passDepthFormat, m_passSamples);
                    if (!state)
                        return false;
                    [m_enc setRenderPipelineState:state];
                    [m_enc setDepthStencilState:depthState(m_pipeline->desc.depth, m_passDepth != nil)];
                    [m_enc setDepthClipMode:m_pipeline->desc.depthClip ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
                    [m_enc setCullMode:MTLCullModeNone];
                    m_pipelineDirty = false;
                }
                if (m_viewportDirty)
                {
                    [m_enc setViewport:m_viewport];
                    m_viewportDirty = false;
                }
                if (m_scissorDirty)
                {
                    [m_enc setScissorRect:m_scissor];
                    m_scissorDirty = false;
                }
                if (m_blendDirty)
                {
                    [m_enc setBlendColorRed:m_blendConstant[0] green:m_blendConstant[1] blue:m_blendConstant[2] alpha:m_blendConstant[3]];
                    m_blendDirty = false;
                }
                if (m_vertexDirty)
                {
                    if (m_vertexBuffer)
                    {
                        id<MTLBuffer> buf;
                        size_t offset;
                        resolveBuffer(*m_vertexBuffer, buf, offset);
                        [m_enc setVertexBuffer:buf offset:offset + m_vertexOffset atIndex:kVertexBufferIndex];
                    }
                    m_vertexDirty = false;
                }
                for (uint32_t slot = 0; m_constantsDirty && slot < kUniformSlots; ++slot)
                {
                    if (!(m_constantsDirty & (1u << slot)))
                        continue;
                    id<MTLBuffer> buf = m_zeroBuffer;
                    size_t offset = 0;
                    if (m_constants[slot])
                        resolveBuffer(*m_constants[slot], buf, offset);
                    [m_enc setVertexBuffer:buf offset:offset atIndex:slot];
                    [m_enc setFragmentBuffer:buf offset:offset atIndex:slot];
                }
                m_constantsDirty = 0;
                for (uint32_t slot = 0; m_texturesDirty && slot < kTextureSlots; ++slot)
                {
                    if (!(m_texturesDirty & (1u << slot)))
                        continue;
                    id<MTLTexture> t = m_textures[slot].texture;
                    if (!t || t == m_passColor || t == m_passDepth) // D3D unbinds a texture that is the target: zeros
                        t = m_dummyTexture;
                    [m_enc setFragmentTexture:t atIndex:slot];
                    [m_enc setFragmentSamplerState:m_textures[slot].sampler ? m_textures[slot].sampler : m_defaultSampler atIndex:slot];
                }
                m_texturesDirty = 0;
                return true;
            }

            uint8_t *ringAlloc(size_t size, __strong id<MTLBuffer> &buffer, size_t &offset)
            {
                ensureCommandBuffer();
                Ring &ring = m_rings[m_frameIndex];
                size = std::max<size_t>(size, 4u);
                while (ring.current < ring.chunks.size())
                {
                    Ring::Chunk &c = ring.chunks[ring.current];
                    const size_t at = (c.used + kRingAlign - 1u) & ~(kRingAlign - 1u);
                    if (at + size <= [c.buffer length])
                    {
                        c.used = at + size;
                        buffer = c.buffer;
                        offset = at;
                        return static_cast<uint8_t *>([c.buffer contents]) + at;
                    }
                    ++ring.current;
                }
                Ring::Chunk c;
                c.buffer = [m_device newBufferWithLength:std::max(kRingChunk, size) options:MTLResourceStorageModeShared];
                c.buffer.label = @"upload ring";
                c.used = size;
                ring.chunks.push_back(c);
                ring.current = ring.chunks.size() - 1u;
                buffer = c.buffer;
                offset = 0;
                return static_cast<uint8_t *>([c.buffer contents]);
            }

            void placeBuffer(MtlBuffer &b, size_t size)
            {
                id<MTLBuffer> buf;
                size_t offset;
                // constant buffers whole (the shader may read past what was written), others what was written
                const size_t bytes = b.desc().usage == BufferUsage::Constant ? b.shadow.size() : std::max<size_t>(size, 4u);
                std::memcpy(ringAlloc(bytes, buf, offset), b.shadow.data(), std::min(bytes, b.shadow.size()));
                b.placed = buf;
                b.placedOffset = offset;
                b.placedFrame = m_frameSerial;
            }

            void resolveBuffer(MtlBuffer &b, __strong id<MTLBuffer> &buf, size_t &offset)
            {
                if (!b.desc().dynamic)
                {
                    buf = b.buffer;
                    offset = 0;
                    return;
                }
                if (b.placedFrame != m_frameSerial) // drawn again in a later frame without an update: copied again
                    placeBuffer(b, b.shadow.size());
                buf = b.placed;
                offset = b.placedOffset;
            }

            void markBufferRebind(MtlBuffer *b)
            {
                if (m_vertexBuffer == b)
                    m_vertexDirty = true;
                for (uint32_t i = 0; i < kUniformSlots; ++i)
                    if (m_constants[i] == b)
                        m_constantsDirty |= 1u << i;
            }

            void reportStats()
            {
                ++m_statFrames;
                m_statDraws += m_drawsThisFrame;
                m_statSuspends += m_suspendsThisFrame;
                m_statPasses += m_passesThisFrame;
                m_drawsThisFrame = m_suspendsThisFrame = m_passesThisFrame = 0;
                if (!m_stats)
                    return;
                const auto now = std::chrono::steady_clock::now();
                if (m_statStart.time_since_epoch().count() == 0)
                    m_statStart = now;
                const double s = std::chrono::duration<double>(now - m_statStart).count();
                if (s < 5.0)
                    return;
                const double f = std::max<double>(1.0, static_cast<double>(m_statFrames));
                std::fprintf(stderr, "[rhi] metal per frame: %.0f draws, %.1f passes, %.1f suspended, %.1f KB uploaded; %.0f ms "
                                     "compiling shaders so far\n",
                             m_statDraws / f, m_statPasses / f, m_statSuspends / f, m_uploadBytes / f / 1024.0, m_compileMs);
                m_statFrames = m_statDraws = m_statSuspends = m_statPasses = 0;
                m_uploadBytes = 0;
                m_statStart = now;
            }

            id<MTLDevice> m_device = nil;
            id<MTLCommandQueue> m_queue = nil;
            CAMetalLayer *m_layer = nil;
            id<CAMetalDrawable> m_drawable = nil;
            id<MTLTexture> m_offscreenBack = nil;
            id<MTLTexture> m_dummyTexture = nil;
            id<MTLSamplerState> m_defaultSampler = nil;
            id<MTLBuffer> m_zeroBuffer = nil;
            dispatch_semaphore_t m_frameSemaphore = nullptr;
            std::string m_adapterName;
            bool m_debug = false;
            bool m_stats = false;
            bool m_msaaClampReported = false;
            uint32_t m_width = 1, m_height = 1;

            std::unordered_map<const void *, id<MTLFunction>> m_functions;
            std::unordered_map<uint32_t, id<MTLDepthStencilState>> m_depthStates;
            double m_compileMs = 0.0;

            // the frame
            id<MTLCommandBuffer> m_cmd = nil;
            id<MTLRenderCommandEncoder> m_enc = nil;
            id<MTLBlitCommandEncoder> m_blit = nil;
            bool m_frameOpen = false;
            uint32_t m_frameIndex = 0;
            uint64_t m_frameSerial = 0;
            std::array<Ring, kFrames> m_rings;

            // the pass
            PassDesc m_pass;
            bool m_inPass = false, m_passStarted = false, m_passSkip = false;
            id<MTLTexture> m_passColor = nil, m_passDepth = nil;
            MTLPixelFormat m_passColorFormat = MTLPixelFormatInvalid, m_passDepthFormat = MTLPixelFormatInvalid;
            uint32_t m_passSamples = 1, m_passWidth = 1, m_passHeight = 1;

            // bound state (D3D11's: kept across draws and passes)
            MtlPipeline *m_pipeline = nullptr;
            MtlBuffer *m_vertexBuffer = nullptr;
            uint32_t m_vertexOffset = 0;
            MtlBuffer *m_indexBuffer = nullptr;
            bool m_index32 = false;
            std::array<MtlBuffer *, kUniformSlots> m_constants{};
            std::array<BoundTexture, kTextureSlots> m_textures{};
            MTLViewport m_viewport{};
            MTLScissorRect m_scissor{};
            bool m_scissorEmpty = false;
            float m_blendConstant[4] = {};
            bool m_pipelineDirty = true, m_viewportDirty = true, m_scissorDirty = true, m_blendDirty = true, m_vertexDirty = true;
            uint32_t m_constantsDirty = 0, m_texturesDirty = 0;

            // PS2X_RHI_STATS
            uint64_t m_drawsThisFrame = 0, m_suspendsThisFrame = 0, m_passesThisFrame = 0;
            uint64_t m_statFrames = 0, m_statDraws = 0, m_statSuspends = 0, m_statPasses = 0;
            double m_uploadBytes = 0.0;
            std::chrono::steady_clock::time_point m_statStart{};
        };

        MtlTexture::~MtlTexture() = default; // bound textures are held by id: nothing dangles
        MtlBuffer::~MtlBuffer()
        {
            if (owner)
                owner->forget(this);
        }
        MtlPipeline::~MtlPipeline()
        {
            if (owner)
                owner->forget(this);
        }
    }

    std::unique_ptr<Device> createMetalDevice(const DeviceDesc &desc, std::string &error)
    {
        @autoreleasepool
        {
            auto device = std::make_unique<MetalDevice>();
            if (!device->init(desc, error))
                return nullptr;
            return device;
        }
    }

    std::vector<Gpu> listMetalGpus()
    {
        std::vector<Gpu> gpus;
        @autoreleasepool
        {
            for (id<MTLDevice> d in MTLCopyAllDevices())
            {
                Gpu g;
                g.name = [[d name] UTF8String];
                g.vendorId = 0x106Bu; // Apple
                const uint64_t r = [d registryID];
                g.deviceId = static_cast<uint32_t>(r ^ (r >> 32));
                g.integrated = [d hasUnifiedMemory];
                gpus.push_back(g);
            }
        }
        numberGpus(gpus);
        return gpus;
    }
}
