/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Native libmpv integration acceptance. Use generated media, an isolated
 * native VapourSynth core and vo=null; no Python or product profile is used.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <VapourSynth4.h>

#include "mpv/client.h"
#include "mpv/qbutt_vapoursynth.h"

namespace {

struct Evidence {
    std::atomic<int> created = 0;
    std::atomic<int> destroyed = 0;
    std::atomic<int> frames = 0;
    std::atomic<int> active = 0;
    std::atomic<int> invalid_metadata = 0;
    std::atomic<int> width = 0;
    std::atomic<int> height = 0;
    std::atomic<int64_t> sar_num = 0;
    std::atomic<int64_t> sar_den = 0;
} evidence;

struct Graph {
    VSNode *input;
};

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const VSFrame *VS_CC get_frame(int n, int reason, void *instance, void **,
                             VSFrameContext *context, VSCore *core,
                             const VSAPI *api)
{
    Graph *graph = static_cast<Graph *>(instance);
    int left = n / 2;
    if (reason == arInitial) {
        api->requestFrameFilter(left, graph->input, context);
        if (n % 2)
            api->requestFrameFilter(left + 1, graph->input, context);
    } else if (reason == arAllFramesReady) {
        evidence.active++;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        const VSFrame *source = api->getFrameFilter(left, graph->input, context);
        if (!source) {
            evidence.active--;
            return nullptr;
        }
        if (n % 2) {
            const VSFrame *right = api->getFrameFilter(left + 1,
                                                       graph->input, context);
            if (!right) {
                api->freeFrame(source);
                evidence.active--;
                return nullptr;
            }
            api->freeFrame(right);
        }

        const VSMap *props = api->getFramePropertiesRO(source);
        int64_t sar_num = api->mapGetInt(props, "_SARNum", 0, nullptr);
        int64_t sar_den = api->mapGetInt(props, "_SARDen", 0, nullptr);
        const VSVideoFormat *format = api->getVideoFrameFormat(source);
        if (api->mapGetDataSize(props, "_MP_IMAGE", 0, nullptr) <= 0 ||
            api->mapGetInt(props, "_DurationDen", 0, nullptr) <= 0 ||
            sar_num <= 0 || sar_den <= 0 ||
            format->subSamplingW || format->subSamplingH)
        {
            if (evidence.invalid_metadata++ < 5) {
                std::fprintf(stderr,
                    "INVALID n=%d private=%d duration=%lld/%lld SAR=%lld/%lld chroma=%d/%d\n",
                    n, api->mapGetDataSize(props, "_MP_IMAGE", 0, nullptr),
                    static_cast<long long>(api->mapGetInt(props, "_DurationNum", 0, nullptr)),
                    static_cast<long long>(api->mapGetInt(props, "_DurationDen", 0, nullptr)),
                    static_cast<long long>(sar_num), static_cast<long long>(sar_den),
                    format->subSamplingW, format->subSamplingH);
            }
        }
        evidence.width = api->getFrameWidth(source, 0);
        evidence.height = api->getFrameHeight(source, 0);
        evidence.sar_num = sar_num;
        evidence.sar_den = sar_den;

        // The fixture deliberately duplicates pixels. It tests mpv's native
        // graph boundary, scheduling and metadata, not an inference backend.
        VSFrame *output = api->copyFrame(source, core);
        api->freeFrame(source);
        VSMap *output_props = api->getFramePropertiesRW(output);
        api->mapSetInt(output_props, "_DurationNum", 1001, maReplace);
        api->mapSetInt(output_props, "_DurationDen", 48000, maReplace);
        evidence.frames++;
        evidence.active--;
        return output;
    }
    return nullptr;
}

void VS_CC free_graph(void *instance, VSCore *, const VSAPI *api)
{
    Graph *graph = static_cast<Graph *>(instance);
    api->freeNode(graph->input);
    delete graph;
    evidence.destroyed++;
}

VSNode *create_graph(const VSAPI *api, VSCore *core, VSNode *input,
                     const char *config, char *error, size_t error_size)
{
    if (!std::strcmp(config, "fail-construction")) {
        std::snprintf(error, error_size, "requested native constructor failure");
        return nullptr;
    }
    if (std::strcmp(config, "native-config-is-unchanged")) {
        std::snprintf(error, error_size, "native config changed");
        return nullptr;
    }

    VSVideoInfo info = *api->getVideoInfo(input);
    info.fpsNum = 48000;
    info.fpsDen = 1001;
    info.numFrames *= 2;
    Graph *graph = new Graph{api->addNodeRef(input)};
    VSFilterDependency dependency{input, rpGeneral};
    VSNode *output = api->createVideoFilter2(
        "QbuttNativeAcceptance", &info, get_frame, free_graph, fmParallel,
        &dependency, 1, graph, core);
    if (!output) {
        api->freeNode(graph->input);
        delete graph;
        std::snprintf(error, error_size, "createVideoFilter2 failed");
        return nullptr;
    }
    evidence.created++;
    return output;
}

class Session {
public:
    mpv_handle *handle = mpv_create();
    std::string log;
    bool ended = false;
    bool allow_error = false;

    Session(const std::string &core, bool paused = false,
            const char *config = "native-config-is-unchanged")
    {
        require(handle != nullptr, "mpv_create failed");
        allow_error = !std::strcmp(config, "fail-construction");
        require(mpv_qbutt_set_vapoursynth_graph_factory(handle, nullptr) ==
                MPV_ERROR_INVALID_PARAMETER, "NULL constructor accepted");
        require(mpv_qbutt_set_vapoursynth_graph_factory(handle, create_graph) == 0,
                "native constructor registration failed");
        require(mpv_qbutt_set_vapoursynth_graph_factory(handle, create_graph) ==
                MPV_ERROR_INVALID_PARAMETER, "constructor replaced");
        option("config", "no");
        option("terminal", "no");
        option("load-scripts", "no");
        option("osc", "no");
        option("ytdl", "no");
        option("vo", "null");
        option("ao", "null");
        option("hwdec", "no");
        option("idle", "yes");
        option("keep-open", "no");
        option("untimed", "yes");
        option("pause", paused ? "yes" : "no");
        filter = "@native:vapoursynth-native=file=%" +
            std::to_string(core.size()) + "%" + core +
            ":buffered-frames=4:concurrent-frames=4:user-data=%" +
            std::to_string(std::strlen(config)) + "%" + config;
        option("vf", filter.c_str());
        require(mpv_request_log_messages(handle, "warn") == 0,
                "request logs failed");
        require(mpv_initialize(handle) == 0, "mpv_initialize failed");
        require(mpv_qbutt_set_vapoursynth_graph_factory(handle, create_graph) ==
                MPV_ERROR_INVALID_PARAMETER, "late registration accepted");
    }

    ~Session()
    {
        mpv_terminate_destroy(handle);
    }

    void option(const char *name, const char *value)
    {
        require(mpv_set_option_string(handle, name, value) == 0,
                "set option failed");
    }

    void command(const char *name, const char *argument = nullptr,
                 const char *argument2 = nullptr)
    {
        const char *arguments[] = {name, argument, argument2, nullptr};
        require(mpv_command(handle, arguments) == 0, "mpv_command failed");
    }

    void pump(double timeout = 0.01)
    {
        mpv_event *event = mpv_wait_event(handle, timeout);
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *message = static_cast<mpv_event_log_message *>(event->data);
            log += message->text;
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            require(allow_error || end->error == 0,
                    "playback ended with an error");
            ended = true;
        }
    }

    template<typename Predicate>
    void until(Predicate predicate)
    {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(20);
        while (!predicate()) {
            require(std::chrono::steady_clock::now() < deadline,
                    "native scenario timed out");
            pump();
        }
    }

    void finish()
    {
        until([&] { return ended; });
    }

    std::string filter;
};

void check_drained()
{
    require(evidence.active == 0, "frame callback survived teardown");
    require(evidence.created == evidence.destroyed,
            "native graph survived teardown");
    require(evidence.invalid_metadata == 0, "source metadata was not preserved");
}

bool python_loaded()
{
#ifdef _WIN32
    return GetModuleHandleW(L"python3.dll") ||
        GetModuleHandleW(L"python312.dll") ||
        GetModuleHandleW(L"python313.dll") ||
        GetModuleHandleW(L"python314.dll") ||
        GetModuleHandleW(L"vsscript.dll");
#else
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libpython") != std::string::npos ||
            line.find("vsscript") != std::string::npos)
            return true;
    }
    return false;
#endif
}

} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc == 3, "Usage: qbutt-vapoursynth-native-acceptance <native VS core> <generated 24/1.001fps media>");
        const std::string core = argv[1];
        const char *media = argv[2];
        {
            Session first(core, true);
            Session second(core, true);
            first.command("loadfile", media);
            second.command("loadfile", media);
            first.until([] { return evidence.created >= 2 && evidence.frames >= 2; });
            require(!python_loaded(), "Python or VSScript loaded during playback");
        }
        check_drained();
        std::printf("PASS independent-core registration and native metadata\n");

        int created = evidence.created;
        {
            Session session(core, true);
            session.command("loadfile", media);
            session.until([&] { return evidence.created > created && evidence.frames > 2; });
            created = evidence.created;
            session.command("seek", "1", "absolute+exact");
            session.until([&] { return evidence.created > created; });
            require(!python_loaded(), "Python or VSScript loaded after seek");
            int unpause = 0;
            require(mpv_set_property(session.handle, "pause", MPV_FORMAT_FLAG,
                                     &unpause) == 0, "unpause failed");
            session.finish();
        }
        check_drained();
        std::printf("PASS seek, dynamic EOF and graph generations\n");

        created = evidence.created;
        {
            Session session(core);
            session.command("loadfile", media);
            session.until([&] { return evidence.created > created && evidence.active > 0; });
            session.command("vf", "remove", "@native");
            session.until([] { return evidence.created == evidence.destroyed; });
            require(mpv_set_property_string(session.handle, "vf",
                                             session.filter.c_str()) == 0,
                    "native filter re-add failed");
            created = evidence.created;
            session.command("seek", "0", "absolute+exact");
            session.until([&] { return evidence.created > created && evidence.active > 0; });
            session.command("stop");
        }
        check_drained();
        std::printf("PASS in-flight filter removal, re-add and stop cancellation\n");

        created = evidence.created;
        {
            Session session(core, false, "fail-construction");
            session.command("loadfile", media);
            session.until([&] {
                return session.log.find("requested native constructor failure") !=
                    std::string::npos;
            });
        }
        require(evidence.created == created, "failed constructor produced a graph");
        check_drained();
        require(!python_loaded(), "Python or VSScript loaded");
        std::printf("PASS constructor failure and Python/VSScript absence\n");
        std::printf("EVIDENCE created=%d destroyed=%d frames=%d active=%d invalid-metadata=%d source=%dx%d SAR=%lld:%lld\n",
                    int(evidence.created), int(evidence.destroyed),
                    int(evidence.frames), int(evidence.active),
                    int(evidence.invalid_metadata), int(evidence.width),
                    int(evidence.height), static_cast<long long>(evidence.sar_num),
                    static_cast<long long>(evidence.sar_den));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
