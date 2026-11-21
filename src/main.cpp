#include "RendererEngine.hpp"
#include "ShmWriter.hpp"

#include "Fs/CBinaryStream.h"
#include "Utils/Logging.h"

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace
{

std::mutex g_stdoutMutex;
HANDLE     g_pipeOut = INVALID_HANDLE_VALUE;

std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

HANDLE OpenNamedPipeWithRetry(const std::string& pipePathUtf8, DWORD access) {
    std::wstring wide = Utf8ToWideLocal(pipePathUtf8);
    for (int attempt = 0; attempt < 40; ++attempt) {
        HANDLE h = CreateFileW(wide.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY || err == ERROR_FILE_NOT_FOUND) {
            Sleep(25);
            continue;
        }
        return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}

void EmitJson(const json& obj) {
    std::string line = obj.dump(-1, ' ', false, json::error_handler_t::replace);
    line.push_back('\n');
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    if (g_pipeOut != INVALID_HANDLE_VALUE) {
        const char* ptr = line.data();
        std::size_t rem = line.size();
        while (rem > 0) {
            DWORD written = 0;
            DWORD chunk   = static_cast<DWORD>(std::min<std::size_t>(rem, 65536));
            if (! WriteFile(g_pipeOut, ptr, chunk, &written, nullptr) || written == 0) break;
            ptr += written;
            rem -= written;
        }
        FlushFileBuffers(g_pipeOut);
    } else {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }
}

void EmitReady(uint32_t width, uint32_t height, const std::string& backend) {
    EmitJson({
        { "event", "ready" },
        { "width", width },
        { "height", height },
        { "backend", backend.empty() ? "vulkan-x64" : backend },
    });
}

void EmitStatus(const std::string& state) {
    EmitJson({
        { "event", "status" },
        { "state", state },
    });
}

void EmitError(const std::string& code, const std::string& message) {
    EmitJson({
        { "event", "error" },
        { "code", code },
        { "message", message },
    });
}

uint32_t ClampDim(int value, uint32_t fallback) {
    if (value <= 0) return fallback;
    return static_cast<uint32_t>(std::clamp(value, 64, 2048));
}

uint32_t ClampFps(int value, uint32_t fallback) {
    if (value <= 0) return fallback;
    return static_cast<uint32_t>(std::clamp(value, 1, 60));
}

int RunDumpMode(int argc, char** argv) {
    lcdfusion_wpe::LoadRequest req {};
    req.width  = 480;
    req.height = 480;
    req.fps    = 30;
    std::string dumpPath = "dump.png";
    int         frames   = 10;
    bool        testAudio = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto        nextVal = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        if (arg == "--dir")
            req.dir = nextVal();
        else if (arg == "--assets")
            req.assets = nextVal();
        else if (arg == "--props")
            req.initialPropsJson = nextVal();
        else if (arg == "--test-audio")
            testAudio = true;
        else if (arg == "--width")
            req.width = ClampDim(std::atoi(nextVal().c_str()), 480);
        else if (arg == "--height")
            req.height = ClampDim(std::atoi(nextVal().c_str()), 480);
        else if (arg == "--frames")
            frames = std::clamp(std::atoi(nextVal().c_str()), 1, 600);
        else if (arg == "--dump")
            dumpPath = nextVal();
    }

    if (req.dir.empty()) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  wpe-renderer --ipc | --ipc-pipe <base>\n"
                     "  wpe-renderer --dir <wallpaper_dir> [--assets <wpe_assets_dir>] "
                     "[--width 480] [--height 480] [--frames 10] [--props <json>] [--test-audio] [--dump out.png]\n");
        return 2;
    }

    lcdfusion_wpe::RendererEngine engine;
    std::vector<uint8_t>          lastBgra;
    uint32_t                      lastW = 0, lastH = 0;

    engine.SetFrameConsumer([&](const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) {
        lastW = w;
        lastH = h;
        lastBgra.resize(static_cast<std::size_t>(w) * h * 4u);
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(lastBgra.data() + static_cast<std::size_t>(y) * w * 4u,
                        bgra + static_cast<std::size_t>(y) * stride,
                        static_cast<std::size_t>(w) * 4u);
        }
    });

    std::string err, code;
    if (! engine.Load(req, err, code)) {
        std::fprintf(stderr, "Load failed [%s]: %s\n", code.c_str(), err.c_str());
        return 1;
    }
    if (testAudio) {
        engine.SetPaused(true);
        engine.InjectTestAudioTone(220.0f, 1.0f);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        if (testAudio) {
            engine.InjectTestAudioTone(220.0f, 1.0f);
        }
        if (! engine.RenderFrame(1.0 / 30.0)) {
            std::fprintf(stderr, "RenderFrame failed at frame %d\n", f);
            return 1;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (lastBgra.empty() || lastW == 0 || lastH == 0) {
        std::fprintf(stderr, "No frame pixels captured!\n");
        return 1;
    }

    // Convert BGRA -> RGBA for PNG dump
    std::vector<uint8_t> rgba(lastBgra.size());
    for (std::size_t i = 0; i < lastBgra.size(); i += 4) {
        rgba[i + 0] = lastBgra[i + 2];
        rgba[i + 1] = lastBgra[i + 1];
        rgba[i + 2] = lastBgra[i + 0];
        rgba[i + 3] = lastBgra[i + 3];
    }

    int ok = stbi_write_png(dumpPath.c_str(),
                            static_cast<int>(lastW),
                            static_cast<int>(lastH),
                            4,
                            rgba.data(),
                            static_cast<int>(lastW * 4u));
    if (! ok) {
        std::fprintf(stderr, "Failed to write PNG: %s\n", dumpPath.c_str());
        return 1;
    }

    std::fprintf(stderr,
                 "Rendered %d frames (%ux%u) in %.1f ms (%.1f ms/frame) -> %s\n",
                 frames,
                 lastW,
                 lastH,
                 ms,
                 ms / frames,
                 dumpPath.c_str());
    engine.Shutdown();
    return 0;
}

int RunIpcMode(const std::string& pipeBaseName) {
    HANDLE hPipeIn = INVALID_HANDLE_VALUE;
    if (! pipeBaseName.empty()) {
        std::string inPath  = "\\\\.\\pipe\\" + pipeBaseName + "_in";
        std::string outPath = "\\\\.\\pipe\\" + pipeBaseName + "_out";
        hPipeIn             = OpenNamedPipeWithRetry(inPath, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
        if (hPipeIn == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "Failed to open IPC input pipe: %s (err=%lu)\n", inPath.c_str(), GetLastError());
            return 3;
        }
        g_pipeOut = OpenNamedPipeWithRetry(outPath, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE);
        if (g_pipeOut == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "Failed to open IPC output pipe: %s (err=%lu)\n", outPath.c_str(), GetLastError());
            CloseHandle(hPipeIn);
            return 3;
        }
    }

    std::mutex              cmdMutex;
    std::condition_variable cmdCv;
    std::deque<json>        cmdQueue;
    bool                    stdinClosed = false;

    auto processLine = [&](std::string line) {
        // Strip CR if present
        if (! line.empty() && line.back() == '\r') line.pop_back();
        // Strip UTF-8 BOM if accidentally sent on first line
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (line.empty()) return;

        try {
            auto obj = json::parse(line);
            if (obj.is_object()) {
                {
                    std::lock_guard<std::mutex> lock(cmdMutex);
                    cmdQueue.push_back(std::move(obj));
                }
                cmdCv.notify_one();
            }
        } catch (const std::exception& ex) {
            EmitError("BAD_JSON", ex.what());
        }
    };

    std::thread readerThread([&]() {
        if (hPipeIn != INVALID_HANDLE_VALUE) {
            std::string acc;
            char        buf[4096];
            DWORD       bytesRead = 0;
            while (ReadFile(hPipeIn, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                acc.append(buf, bytesRead);
                std::size_t nl = 0;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    processLine(acc.substr(0, nl));
                    acc.erase(0, nl + 1);
                }
            }
            if (! acc.empty()) processLine(acc);
        } else {
            std::string line;
            while (std::getline(std::cin, line)) {
                processLine(std::move(line));
            }
        }
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            stdinClosed = true;
        }
        cmdCv.notify_one();
    });

    lcdfusion_wpe::RendererEngine engine;
    lcdfusion_wpe::ShmWriter      shm;

    engine.SetFrameConsumer([&](const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) {
        shm.PublishFrame(bgra, w, h, stride);
    });

    bool     quit      = false;
    bool     paused    = false;
    uint32_t targetFps = 30;
    auto     lastFrame = std::chrono::steady_clock::now();
    auto     nextFrame = lastFrame;

    while (! quit) {
        // Drain all queued commands first
        std::deque<json> pending;
        {
            std::unique_lock<std::mutex> lock(cmdMutex);
            if (cmdQueue.empty() && ! stdinClosed) {
                if (! engine.IsLoaded() || paused) {
                    cmdCv.wait(lock, [&]() { return ! cmdQueue.empty() || stdinClosed; });
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (now < nextFrame) {
                        cmdCv.wait_until(
                            lock, nextFrame, [&]() { return ! cmdQueue.empty() || stdinClosed; });
                    }
                }
            }
            pending.swap(cmdQueue);
            if (pending.empty() && stdinClosed) {
                quit = true;
            }
        }

        for (std::size_t idx = 0; idx < pending.size(); ++idx) {
            const auto& msg = pending[idx];
            std::string cmd = msg.value("cmd", "");
            if (cmd == "quit") {
                quit = true;
                break;
            } else if (cmd == "resize") {
                // Coalesce superseded resize commands in the same batch so we don't try to open
                // already-disposed intermediate SHMs or perform wasted Vulkan swapchain rebuilds.
                bool superseded = false;
                for (std::size_t j = idx + 1; j < pending.size(); ++j) {
                    std::string nextCmd = pending[j].value("cmd", "");
                    if (nextCmd == "resize" || nextCmd == "load" || nextCmd == "quit") {
                        superseded = true;
                        break;
                    }
                }
                if (superseded) continue;
            }

            if (cmd == "load") {
                lcdfusion_wpe::LoadRequest req {};
                req.dir     = msg.value("dir", "");
                req.assets  = msg.value("assets", "");
                req.shmName = msg.value("shm", "");
                req.width   = ClampDim(msg.value("width", 480), 480);
                req.height  = ClampDim(msg.value("height", 480), 480);
                req.fps     = ClampFps(msg.value("fps", 30), 30);
                if (msg.contains("props") && msg["props"].is_object()) {
                    req.initialPropsJson = msg["props"].dump();
                }
                targetFps   = req.fps;

                std::string err, code;
                if (! shm.Open(req.shmName, req.width, req.height, err)) {
                    EmitError("SHM_OPEN_FAILED", err);
                    continue;
                }

                if (! engine.Load(req, err, code)) {
                    shm.SetState(lcdfusion_wpe::kShmStateError);
                    EmitError(code.empty() ? "LOAD_FAILED" : code, err);
                    continue;
                }

                paused = false;
                engine.SetPaused(false);
                shm.SetState(lcdfusion_wpe::kShmStateRunning);
                lastFrame = std::chrono::steady_clock::now();
                nextFrame = lastFrame;

                // Render initial frame immediately so host has valid pixels right after ready
                engine.RenderFrame(1.0 / static_cast<double>(targetFps));
                EmitReady(req.width, req.height, engine.BackendName());
            } else if (cmd == "resize") {
                uint32_t    newW    = ClampDim(msg.value("width", static_cast<int>(engine.Width())), 480);
                uint32_t    newH    = ClampDim(msg.value("height", static_cast<int>(engine.Height())), 480);
                uint32_t    newFps  = ClampFps(msg.value("fps", static_cast<int>(targetFps)), targetFps);
                std::string newShm  = msg.value("shm", "");
                targetFps           = newFps;

                std::string err;
                if (! newShm.empty()) {
                    if (! shm.Open(newShm, newW, newH, err)) {
                        EmitError("SHM_OPEN_FAILED", err);
                        continue;
                    }
                }
                if (engine.IsLoaded()) {
                    if (! engine.Resize(newW, newH, err)) {
                        shm.SetState(lcdfusion_wpe::kShmStateError);
                        EmitError("RESIZE_FAILED", err);
                        continue;
                    }
                    shm.SetState(paused ? lcdfusion_wpe::kShmStatePaused
                                        : lcdfusion_wpe::kShmStateRunning);
                    if (! paused) {
                        engine.RenderFrame(1.0 / static_cast<double>(targetFps));
                    }
                }
                lastFrame = std::chrono::steady_clock::now();
                nextFrame = lastFrame;
                EmitReady(newW, newH, engine.BackendName());
            } else if (cmd == "pause") {
                bool val = msg.value("value", false);
                paused   = val;
                engine.SetPaused(paused);
                shm.SetState(paused ? lcdfusion_wpe::kShmStatePaused
                                    : lcdfusion_wpe::kShmStateRunning);
                if (! paused) {
                    lastFrame = std::chrono::steady_clock::now();
                    nextFrame = lastFrame;
                }
                EmitStatus(paused ? "paused" : "running");
            } else if (cmd == "props") {
                if (msg.contains("values") && msg["values"].is_object()) {
                    std::string err;
                    if (! engine.SetProperties(msg["values"].dump(), err)) {
                        EmitError("PROPS_FAILED", err);
                        continue;
                    }
                    if (engine.IsLoaded() && ! paused) {
                        engine.RenderFrame(1.0 / static_cast<double>(targetFps));
                    }
                }
                EmitStatus("props_applied");
            }
        }

        if (quit) break;

        if (engine.IsLoaded() && ! paused) {
            auto now = std::chrono::steady_clock::now();
            if (now >= nextFrame) {
                double dt = std::chrono::duration<double>(now - lastFrame).count();
                dt        = std::clamp(dt, 1.0 / 240.0, 0.25);
                lastFrame = now;

                engine.RenderFrame(dt);

                auto framePeriod =
                    std::chrono::microseconds(1000000LL / static_cast<long long>(targetFps));
                nextFrame = now + framePeriod;
            }
        }
    }

    shm.Close();
    engine.Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_stdoutMutex);
        if (g_pipeOut != INVALID_HANDLE_VALUE) {
            CloseHandle(g_pipeOut);
            g_pipeOut = INVALID_HANDLE_VALUE;
        }
    }
    if (hPipeIn != INVALID_HANDLE_VALUE) {
        CancelIoEx(hPipeIn, nullptr);
        CloseHandle(hPipeIn);
        hPipeIn = INVALID_HANDLE_VALUE;
    }
    if (readerThread.joinable()) {
        // On normal quit command stdin/pipe might still be open; detach if host hasn't closed pipe yet.
        readerThread.detach();
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    bool        ipcMode      = false;
    std::string ipcPipeBase;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ipc") == 0) {
            ipcMode = true;
        } else if (std::strcmp(argv[i], "--ipc-pipe") == 0 && i + 1 < argc) {
            ipcMode     = true;
            ipcPipeBase = argv[++i];
        }
    }

    if (ipcMode) {
        return RunIpcMode(ipcPipeBase);
    }
    return RunDumpMode(argc, argv);
}
