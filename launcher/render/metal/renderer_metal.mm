// Metal backend (macOS, docs/LAUNCHER.md "macOS"): the same one instanced draw as Direct3D 11 and Vulkan, from the
// same ui.hlsl (MSL through glslang + SPIRV-Cross, tools/hlsl_msl.py; compiled here when the renderer starts - the
// Command Line Tools have no Metal compiler, the OS has one). Objective-C++ with ARC.
//
// - The window's CAMetalLayer (platform_macos.mm makes it), BGRA8 UNORM in sRGB: the values as they are, as the
//   other backends' UNORM swap chains.
// - A latency of one frame: rendererWaitForFrame waits until the GPU has finished the previous frame before input is
//   read, as D3D11's frame latency waitable.
// - Instances in a per-frame shared buffer (memcpy, no staging), the glyph atlas updated by its dirty rect.
// - No window: an offscreen RGBA8 texture (--screenshot, --self-test, --hidden).
#include "render/renderer.h"

#include "render/draw.h"
#include "render/font.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "ui_ps.msl.h"
#include "ui_vs.msl.h"

#include <algorithm>
#include <cstring>

#if !__has_feature(objc_arc)
#error "renderer_metal.mm is built with ARC (-fobjc-arc)"
#endif

namespace render
{
    RendererStats rendererStats;

    namespace
    {
        constexpr int kFrames = 2;
        constexpr NSUInteger kInstanceBuffer = 30; // tools/hlsl_msl.py: vertex buffers at 30, the constants at 0

        struct FrameConstants
        {
            float viewport[2];
            float atlasInvSize[2];
        };

        struct State
        {
            id<MTLDevice> device = nil;
            id<MTLCommandQueue> queue = nil;
            id<MTLRenderPipelineState> pipeline = nil;
            id<MTLSamplerState> sampler = nil;
            CAMetalLayer *layer = nil;
            id<MTLTexture> offscreen = nil;
            id<CAMetalDrawable> drawable = nil;
            id<MTLCommandBuffer> pending = nil; // rendered, not yet committed (rendererPresent commits)
            id<MTLBuffer> instances[kFrames] = {nil, nil};
            size_t instanceCapacity[kFrames] = {0, 0};
            id<MTLCommandBuffer> inFlight[kFrames] = {nil, nil};
            int frame = 0;
            id<MTLTexture> atlas = nil;
            int atlasSize = 0;
            uint32_t atlasGeneration = 0;
            int width = 0, height = 0;
            bool loseDevice = false;
        };
        State mtl;

        bool fail(std::string &error, const char *what, NSError *e)
        {
            error = std::string("Metal: ") + what + (e ? std::string(": ") + [[e localizedDescription] UTF8String] : "");
            return false;
        }

        id<MTLFunction> compile(const char *source, std::string &error)
        {
            MTLCompileOptions *options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion3_0;
            NSError *e = nil;
            id<MTLLibrary> lib = [mtl.device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&e];
            if (!lib)
            {
                fail(error, "shader", e);
                return nil;
            }
            return [lib newFunctionWithName:@"vcs_main"];
        }

        bool createPipeline(std::string &error)
        {
            id<MTLFunction> vs = compile(g_ui_vs_msl, error), ps = vs ? compile(g_ui_ps_msl, error) : nil;
            if (!vs || !ps)
                return false;
            // every attribute is per instance; the vertex shader makes the corners from the vertex id. Attributes in the
            // order of ui.hlsl's Instance (glslang numbers them so)
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            struct Attr
            {
                MTLVertexFormat format;
                size_t offset;
            };
            const Attr attrs[] = {
                {MTLVertexFormatFloat4, offsetof(Instance, rect)},
                {MTLVertexFormatFloat4, offsetof(Instance, uv)},
                {MTLVertexFormatFloat4, offsetof(Instance, radii)},
                {MTLVertexFormatFloat4, offsetof(Instance, clip)},
                {MTLVertexFormatUChar4Normalized, offsetof(Instance, color0)},
                {MTLVertexFormatUChar4Normalized, offsetof(Instance, color1)},
                {MTLVertexFormatUChar4Normalized, offsetof(Instance, border)},
                {MTLVertexFormatUInt, offsetof(Instance, kind)},
                {MTLVertexFormatFloat4, offsetof(Instance, params)},
            };
            for (NSUInteger i = 0; i < std::size(attrs); ++i)
            {
                vd.attributes[i].format = attrs[i].format;
                vd.attributes[i].offset = attrs[i].offset;
                vd.attributes[i].bufferIndex = kInstanceBuffer;
            }
            vd.layouts[kInstanceBuffer].stride = sizeof(Instance);
            vd.layouts[kInstanceBuffer].stepFunction = MTLVertexStepFunctionPerInstance;
            vd.layouts[kInstanceBuffer].stepRate = 1;

            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = vs;
            rd.fragmentFunction = ps;
            rd.vertexDescriptor = vd;
            MTLRenderPipelineColorAttachmentDescriptor *ca = rd.colorAttachments[0];
            ca.pixelFormat = mtl.layer ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
            ca.blendingEnabled = YES; // premultiplied alpha
            ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
            ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            NSError *e = nil;
            mtl.pipeline = [mtl.device newRenderPipelineStateWithDescriptor:rd error:&e];
            if (!mtl.pipeline)
                return fail(error, "pipeline", e);
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            mtl.sampler = [mtl.device newSamplerStateWithDescriptor:sd];
            return true;
        }

        void makeOffscreen()
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:NSUInteger(mtl.width)
                                                                                         height:NSUInteger(mtl.height)
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            mtl.offscreen = [mtl.device newTextureWithDescriptor:td];
        }

        bool create(std::string &error)
        {
            mtl.device = MTLCreateSystemDefaultDevice();
            if (!mtl.device)
            {
                error = "Metal: no graphics device";
                return false;
            }
            mtl.queue = [mtl.device newCommandQueue];
            rendererStats.api = "Metal";
            rendererStats.adapter = [[mtl.device name] UTF8String];
            rendererStats.warp = false;
            rendererStats.tearing = false;
            if (mtl.layer)
            {
                mtl.layer.device = mtl.device;
                mtl.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                mtl.layer.framebufferOnly = YES;
                mtl.layer.maximumDrawableCount = 2;
                mtl.layer.drawableSize = CGSizeMake(mtl.width, mtl.height);
                CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                mtl.layer.colorspace = srgb;
                CGColorSpaceRelease(srgb);
            }
            else
                makeOffscreen();
            return createPipeline(error);
        }

        void destroy()
        {
            for (__strong id<MTLCommandBuffer> &c : mtl.inFlight)
            {
                [c waitUntilCompleted];
                c = nil;
            }
            CAMetalLayer *layer = mtl.layer;
            const int w = mtl.width, h = mtl.height;
            mtl = State();
            mtl.layer = layer;
            mtl.width = w;
            mtl.height = h;
        }

        // only the part of the atlas new glyphs touched; the texture is written in place once the frame that last
        // drew with it has finished (rendererWaitForFrame waited for it)
        void uploadAtlas()
        {
            if (!mtl.atlas || mtl.atlasGeneration != atlas.generation || mtl.atlasSize != atlas.size)
            {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                              width:NSUInteger(atlas.size)
                                                                                             height:NSUInteger(atlas.size)
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                mtl.atlas = [mtl.device newTextureWithDescriptor:td];
                mtl.atlasGeneration = atlas.generation;
                mtl.atlasSize = atlas.size;
                atlas.markAllDirty();
            }
            if (atlas.dirtyX0 >= atlas.dirtyX1 || atlas.dirtyY0 >= atlas.dirtyY1)
                return;
            // the previous frame may still sample it: wait for every frame in flight (only when glyphs were added)
            for (id<MTLCommandBuffer> c : mtl.inFlight)
                [c waitUntilCompleted];
            const uint8_t *src = atlas.pixels.data() + size_t(atlas.dirtyY0) * size_t(atlas.size) + size_t(atlas.dirtyX0);
            [mtl.atlas replaceRegion:MTLRegionMake2D(NSUInteger(atlas.dirtyX0), NSUInteger(atlas.dirtyY0),
                                                     NSUInteger(atlas.dirtyX1 - atlas.dirtyX0), NSUInteger(atlas.dirtyY1 - atlas.dirtyY0))
                         mipmapLevel:0
                           withBytes:src
                         bytesPerRow:NSUInteger(atlas.size)];
            rendererStats.uploadBytes += size_t(atlas.dirtyX1 - atlas.dirtyX0) * size_t(atlas.dirtyY1 - atlas.dirtyY0);
            atlas.dirtyX0 = atlas.dirtyY0 = atlas.size;
            atlas.dirtyX1 = atlas.dirtyY1 = 0;
        }

        size_t uploadInstances()
        {
            const size_t count = draw.instances.size();
            if (!count)
                return 0;
            const size_t bytes = count * sizeof(Instance);
            __strong id<MTLBuffer> &buf = mtl.instances[mtl.frame];
            if (!buf || mtl.instanceCapacity[mtl.frame] < bytes)
            {
                size_t capacity = std::max<size_t>(mtl.instanceCapacity[mtl.frame] * 2, 4096 * sizeof(Instance));
                while (capacity < bytes)
                    capacity *= 2;
                buf = [mtl.device newBufferWithLength:capacity options:MTLResourceStorageModeShared];
                mtl.instanceCapacity[mtl.frame] = capacity;
            }
            std::memcpy([buf contents], draw.instances.data(), bytes);
            rendererStats.uploadBytes += bytes;
            return count;
        }
    }

    bool rendererInit(void *window, int width, int height, std::string &error)
    {
        @autoreleasepool
        {
            mtl.layer = window ? (__bridge CAMetalLayer *)window : nil;
            mtl.width = std::max(width, 1);
            mtl.height = std::max(height, 1);
            if (create(error))
                return true;
            destroy();
            return false;
        }
    }

    void rendererShutdown()
    {
        destroy();
        mtl = State();
    }

    // the frame this slot held must be done before its instance buffer is written again (and, with a window, a
    // drawable is waited for at the start of rendering): latency of one
    void rendererWaitForFrame()
    {
        if (id<MTLCommandBuffer> c = mtl.inFlight[mtl.frame])
            [c waitUntilCompleted];
        mtl.inFlight[mtl.frame] = nil;
    }

    void rendererRender(core::Color clear, int width, int height)
    {
        @autoreleasepool
        {
            rendererStats.instances = rendererStats.drawCalls = 0;
            rendererStats.uploadBytes = 0;
            if (!mtl.device || width <= 0 || height <= 0)
                return;
            rendererWaitForFrame();
            if (width != mtl.width || height != mtl.height)
            {
                mtl.width = width;
                mtl.height = height;
                if (mtl.layer)
                    mtl.layer.drawableSize = CGSizeMake(width, height);
                else
                    makeOffscreen();
            }
            uploadAtlas();
            const size_t count = uploadInstances();
            id<MTLTexture> target = mtl.offscreen;
            if (mtl.layer)
            {
                mtl.drawable = [mtl.layer nextDrawable];
                if (!mtl.drawable)
                    return;
                target = mtl.drawable.texture;
            }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(double(clear & 0xFF) / 255.0, double((clear >> 8) & 0xFF) / 255.0,
                                                                  double((clear >> 16) & 0xFF) / 255.0, double((clear >> 24) & 0xFF) / 255.0);
            id<MTLCommandBuffer> cmd = [mtl.queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            if (count && mtl.atlas)
            {
                const FrameConstants c = {{float(mtl.width), float(mtl.height)},
                                          {1.0f / float(std::max(atlas.size, 1)), 1.0f / float(std::max(atlas.size, 1))}};
                [enc setRenderPipelineState:mtl.pipeline];
                [enc setCullMode:MTLCullModeNone];
                [enc setViewport:MTLViewport{0, 0, double(mtl.width), double(mtl.height), 0, 1}];
                [enc setVertexBytes:&c length:sizeof(c) atIndex:0];
                [enc setFragmentBytes:&c length:sizeof(c) atIndex:0];
                [enc setVertexBuffer:mtl.instances[mtl.frame] offset:0 atIndex:kInstanceBuffer];
                [enc setFragmentTexture:mtl.atlas atIndex:0];
                [enc setFragmentSamplerState:mtl.sampler atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:count];
                rendererStats.instances = int(count);
                rendererStats.drawCalls = 1;
            }
            [enc endEncoding];
            mtl.pending = cmd;
        }
    }

    void rendererPresent(bool vsync)
    {
        @autoreleasepool
        {
            if (mtl.layer && mtl.layer.displaySyncEnabled != vsync)
                mtl.layer.displaySyncEnabled = vsync;
            if (mtl.pending)
            {
                if (mtl.drawable)
                    [mtl.pending presentDrawable:mtl.drawable];
                [mtl.pending commit];
                mtl.inFlight[mtl.frame] = mtl.pending;
                mtl.pending = nil;
                mtl.frame = (mtl.frame + 1) % kFrames;
            }
            mtl.drawable = nil;
            if (mtl.loseDevice)
            {
                // a debug switch: start over from what the CPU has, as after a driver reset
                mtl.loseDevice = false;
                destroy();
                std::string error;
                if (!create(error))
                    destroy();
                atlas.markAllDirty();
            }
        }
    }

    bool rendererReadback(std::vector<uint8_t> &rgba, int &width, int &height)
    {
        @autoreleasepool
        {
            if (!mtl.device || !mtl.offscreen)
                return false; // a window's drawable is framebuffer-only: screenshots render offscreen
            id<MTLCommandBuffer> cmd = mtl.pending ? mtl.pending : [mtl.queue commandBuffer];
            mtl.pending = nil;
            const NSUInteger w = [mtl.offscreen width], h = [mtl.offscreen height];
            id<MTLBuffer> staging = [mtl.device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit copyFromTexture:mtl.offscreen
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:staging
                       destinationOffset:0
                  destinationBytesPerRow:w * 4
                destinationBytesPerImage:w * h * 4];
            [blit endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            width = int(w);
            height = int(h);
            rgba.resize(size_t(w) * size_t(h) * 4);
            const uint8_t *src = static_cast<const uint8_t *>([staging contents]);
            for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
            {
                rgba[i * 4 + 0] = src[i * 4 + 0];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 2];
                rgba[i * 4 + 3] = 255; // opaque
            }
            return true;
        }
    }

    void rendererSimulateDeviceLoss() { mtl.loseDevice = true; }
}
