// VCS disc streaming without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 1; docs/game-internals.md 9).
//
// The game reads everything after boot through CDSTREAM.IRX's stream server (SIF RPC 0x65686577, EE client 0x505B48),
// called from three EE functions:
//   FUN_00221818(channel, EE dest, LSN, size)  fn 1: start a read - size in sectors, or -bytes; returns 1 (accepted).
//                                               Afterwards FUN_00221678(the reply's error word) shows a disc error.
//   FUN_002218f0(channel)                        fn 2: 0 when the channel's read is done, 0xFA while busy.
//   FUN_00221968()                               fn 5: 0 (the IOP never answered anything else).
// Here they are replaced: reads are queued for a host thread that copies from the disc image into EE RAM; a poll
// says busy until the copy is done. The game never touches a buffer before its poll says done, as with the IOP.
#include "game/vcs_stream.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_iop_native.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include "runtime/ps2_host_std.h"

namespace vcs
{
    namespace
    {
        constexpr uint32_t kSid = 0x65686577u;
        constexpr uint32_t kRead = 0x00221818u;
        constexpr uint32_t kPoll = 0x002218F0u;
        constexpr uint32_t kFn5 = 0x00221968u;
        constexpr uint32_t kBusy = 0xFAu;
        constexpr uint32_t kChannels = 16u; // the game uses 0..5 (init asks for 6)

        struct Request
        {
            uint32_t channel, dest, lsn, bytes;
        };

        struct Reader
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::condition_variable doneCv; // a read finished
            std::deque<Request> queue;
            std::atomic<uint32_t> pending[kChannels]{}; // reads queued or in flight per channel
            uint8_t *rdram = nullptr;
            bool started = false;
            // statistics
            std::atomic<uint64_t> reads{0}, bytes{0}, nanoseconds{0};
        };

        Reader &reader()
        {
            static Reader *r = new Reader(); // never destroyed: the thread may still wait on it at exit
            return *r;
        }

        void run()
        {
            Reader &r = reader();
            for (;;)
            {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(r.mutex);
                    r.cv.wait(lock, [&] { return !r.queue.empty(); });
                    request = r.queue.front();
                    r.queue.pop_front();
                }
                const auto start = ps2x::hostNow();
                IPS2DiscImage *disc = ps2_disc::image();
                const uint32_t dest = request.dest & 0x01FFFFFFu;
                const uint32_t bytes = std::min<uint32_t>(request.bytes, 0x02000000u - dest);
                if (!disc || !disc->readBytes(static_cast<uint64_t>(request.lsn) * 2048u, bytes, r.rdram + dest))
                    std::fprintf(stderr, "[stream] read of %u bytes at LSN 0x%x failed\n", bytes, request.lsn);
                r.reads.fetch_add(1, std::memory_order_relaxed);
                r.bytes.fetch_add(bytes, std::memory_order_relaxed);
                r.nanoseconds.fetch_add(static_cast<uint64_t>(
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow() - start).count()),
                                        std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(r.mutex);
                    r.pending[request.channel % kChannels].fetch_sub(1, std::memory_order_release); // the data before "done"
                }
                r.doneCv.notify_all();
            }
        }

        void report()
        {
            static const bool log = std::getenv("VCS_STREAM_LOG") != nullptr;
            static auto last = ps2x::hostNow();
            if (!log || ps2x::hostNow() - last < std::chrono::seconds(5))
                return;
            last = ps2x::hostNow();
            Reader &r = reader();
            std::fprintf(stderr, "[stream] %llu reads, %.1f MB, %.1f ms reading (last 5 s)\n",
                         static_cast<unsigned long long>(r.reads.exchange(0)), r.bytes.exchange(0) / 1048576.0,
                         r.nanoseconds.exchange(0) / 1e6);
        }

        // FUN_00221818(channel, dest, lsn, size)
        PS2_GUEST_CC void streamRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            Reader &r = reader();
            const int32_t size = static_cast<int32_t>(GPR_U32(ctx, 7));
            Request request{GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6),
                            size < 0 ? static_cast<uint32_t>(-size) : static_cast<uint32_t>(size) * 2048u};
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                if (!r.started)
                {
                    r.rdram = rdram;
                    r.started = true;
                    std::thread(run).detach();
                }
                r.pending[request.channel % kChannels].fetch_add(1, std::memory_order_relaxed);
                r.queue.push_back(request);
            }
            r.cv.notify_one();
            report();
            SET_GPR_U64(ctx, 2, 1); // accepted (the reply word)
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_002218f0(channel): a read still in flight is waited for up to 20 ms - the game polls right after starting
        // most reads and otherwise loses a frame per read to "busy" (boot took 1 s longer than through the IOP)
        PS2_GUEST_CC void streamPoll(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            Reader &r = reader();
            std::atomic<uint32_t> &pending = r.pending[GPR_U32(ctx, 4) % kChannels];
            bool busy = pending.load(std::memory_order_acquire) != 0u;
            if (busy)
            {
                std::unique_lock<std::mutex> lock(r.mutex);
                busy = !ps2x::waitFor(r.doneCv, lock, std::chrono::milliseconds(20),
                                          [&] { return pending.load(std::memory_order_acquire) == 0u; });
            }
            SET_GPR_U64(ctx, 2, busy ? kBusy : 0u);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_00221968()
        PS2_GUEST_CC void streamFn5(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U64(ctx, 2, 0);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    bool installStreamHooks(PS2Runtime &runtime)
    {
        const char *v = std::getenv("VCS_NATIVE_STREAM");
        if (v && *v == '0')
        {
            std::fprintf(stderr, "[stream] through the IOP (VCS_NATIVE_STREAM=0)\n");
            return false;
        }
        runtime.replaceFunction(kRead, &streamRead);
        runtime.replaceFunction(kPoll, &streamPoll);
        runtime.replaceFunction(kFn5, &streamFn5);
        return true;
    }

    void retireIop(bool nativeComplete)
    {
        const char *v = std::getenv("PS2X_IOP");
        if ((v && *v == '1') || (!v && !nativeComplete))
        {
            ps2_iop_native::setEmulatedIop(true);
            std::fprintf(stderr, "[iop] emulated IOP on%s\n", nativeComplete ? " (PS2X_IOP=1)" : " (sound or streaming through it)");
            return;
        }
        ps2_iop_native::setEmulatedIop(false);
        // The only call that still reaches the stream server: fn 0 at boot (CDSTREAM 0x0000: its read queues and CD
        // thread for 6 channels - the IOP's own sound code reads through channels 2 and 3), no reply. Reads, polls and
        // fn 5 never get to it (the EE functions are replaced).
        ps2_iop_native::registerServer(kSid, [](uint8_t *, const ps2x::iop::RpcRequest &request)
                                       {
                                           if (request.function != 0)
                                               std::fprintf(stderr, "[stream] unexpected call %u to the stream server\n",
                                                            request.function);
                                       });
        std::fprintf(stderr, "[iop] emulated IOP off: no IRX module loaded or run (PS2X_IOP=1 runs it)\n");
    }
}
