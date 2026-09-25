#include "profiler_live.h"

#ifdef MELEE_VITA_PROFILER

#include <vitaprofiler.h>
#include <vitaprofiler_stream.h>
#include <vitaprofiler_tcp_vita.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <string.h>

#ifndef MELEE_VITA_PROFILER_HOST_A
#define MELEE_VITA_PROFILER_HOST_A 127
#define MELEE_VITA_PROFILER_HOST_B 0
#define MELEE_VITA_PROFILER_HOST_C 0
#define MELEE_VITA_PROFILER_HOST_D 1
#endif
#ifndef MELEE_VITA_PROFILER_PORT
#define MELEE_VITA_PROFILER_PORT 18195
#endif

#define PROFILER_RING_CAPACITY 2048u
#define PROFILER_NAME_CAPACITY 48u
#define PROFILER_NAME_TEXT_BYTES 2048u
#define PROFILER_STREAM_BUFFER_BYTES 8192u
#define PROFILER_NET_MEMORY_BYTES (1024u * 1024u)
#define PROFILER_DRAIN_EVENTS 128u
#define PROFILER_DURATION_COUNT 34u
#define PROFILER_RECONNECT_DELAY_US 500000u
#define PROFILER_STATUS_PATH "ux0:data/melee/profiler-live-status.txt"

static const char* const s_duration_names[] = {
    "duration.gobj_render_us", "duration.present_us",
    "duration.render_wait_us", "duration.copy_texture_us",
    "duration.texture_upload_us", "duration.display_list_hash_us",
    "duration.display_list_build_us", "duration.draw_setup_us",
    "duration.gpu_submit_us", "duration.immediate_us",
    "duration.audio_mix_us", "duration.render_thread_us",
    "duration.vertex_program_us", "duration.fragment_program_us",
    "duration.fragment_patch_us", "duration.render_queue_push_us",
    "duration.resolve_textures_us", "duration.uniform_copy_us",
    "duration.bump_hash_us", "duration.bump_fill_us",
    "duration.bump_build_us", "duration.bump_draw_us",
    "duration.cpu_display_decode_us", "duration.cpu_display_submit_us",
    "duration.rt_scene_begin_us", "duration.rt_copy_us",
    "duration.rt_present_us",
    "duration.game_update_us", "state.scene_kind", "state.stage_kind",
    "state.fighters", "duration.rt_end_scene_us",
    "duration.rt_gpu_finish_us", "duration.rt_swap_us",
};
_Static_assert(
    sizeof(s_duration_names) / sizeof(s_duration_names[0]) ==
        PROFILER_DURATION_COUNT,
    "profiler duration names must match the game zone count");

static uint8_t s_net_memory[PROFILER_NET_MEMORY_BYTES]
    __attribute__((aligned(64)));
static struct vp_slot s_slots[PROFILER_RING_CAPACITY];
static struct vp_context s_context;
static struct vp_name_entry s_name_entries[PROFILER_NAME_CAPACITY];
static char s_name_text[PROFILER_NAME_TEXT_BYTES];
static struct vp_name_dictionary s_names;
static uint8_t s_stream_buffer[PROFILER_STREAM_BUFFER_BYTES];
static struct vp_stream_writer_v2 s_writer;
static struct vp_vita_tcp_sce_net_backend s_backend =
    VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
static struct vp_vita_tcp_sink s_sink;
static uint32_t s_duration_ids[
    sizeof(s_duration_names) / sizeof(s_duration_names[0])];
static uint32_t s_draw_calls_id;
static uint32_t s_triangles_id;
static uint32_t s_fragment_metric_ids[6];
static uint32_t s_queue_metric_ids[5];
static uint32_t s_pending_duration_us[PROFILER_DURATION_COUNT];
static uint32_t s_frame_id;
static SceUID s_thread = -1;
static volatile int s_running;
static volatile int s_accepting;
static int s_context_initialized;
static int s_names_initialized;

static void report_status(const char* stage, int result, int detail)
{
    FILE* file = fopen(PROFILER_STATUS_PATH, "a");
    if (file == NULL) return;
    fprintf(file, "%llu %s result=0x%08X detail=0x%08X\n",
            (unsigned long long) sceKernelGetSystemTimeWide(), stage,
            (unsigned int) result, (unsigned int) detail);
    fclose(file);
}

static void stop_network(int netctl, int net, int module)
{
    if (netctl) sceNetCtlTerm();
    if (net) (void) sceNetTerm();
    if (module) (void) sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

static int wait_for_network(void)
{
    while (s_running) {
        int state = SCE_NETCTL_STATE_DISCONNECTED;
        if (sceNetCtlInetGetState(&state) >= 0 &&
            state == SCE_NETCTL_STATE_CONNECTED)
            return 0;
        sceKernelDelayThread(100000);
    }
    return -1;
}

static void close_sink(void)
{
    for (unsigned int attempt = 0u; attempt < 4u; ++attempt) {
        if (vp_vita_tcp_sink_close(&s_sink) == VP_RESULT_OK) return;
        sceKernelDelayThread(10000);
    }
}

static int profiler_thread(SceSize argc, void* argv)
{
    struct vp_vita_tcp_sink_config sink_config;
    struct vp_stream_writer_config writer_config;
    struct vp_stream_v2_session session;
    SceNetInitParam net_init;
    int module_loaded = 0;
    int net_initialized = 0;
    int netctl_initialized = 0;
    int sink_initialized = 0;
    int writer_streaming = 0;
    uint64_t last_stats_us = 0u;
    int result;
    (void) argc;
    (void) argv;

    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    report_status("sysmodule_load", result, 0);
    if (result < 0) goto cleanup;
    module_loaded = 1;
    memset(&net_init, 0, sizeof(net_init));
    net_init.memory = s_net_memory;
    net_init.size = sizeof(s_net_memory);
    result = sceNetInit(&net_init);
    report_status("net_init", result, 0);
    if (result < 0) goto cleanup;
    net_initialized = 1;
    result = sceNetCtlInit();
    report_status("netctl_init", result, 0);
    if (result < 0) goto cleanup;
    netctl_initialized = 1;
    result = wait_for_network();
    report_status("network_ready", result, 0);
    if (result < 0) goto cleanup;

    while (s_running) {
        struct vp_vita_tcp_sink_stats sink_stats;
        writer_streaming = 0;
        sink_initialized = 0;
        last_stats_us = 0u;
        memset(&s_backend, 0, sizeof(s_backend));
        vp_vita_tcp_sink_config_init(&sink_config);
        sink_config.endpoint.ipv4[0] = MELEE_VITA_PROFILER_HOST_A;
        sink_config.endpoint.ipv4[1] = MELEE_VITA_PROFILER_HOST_B;
        sink_config.endpoint.ipv4[2] = MELEE_VITA_PROFILER_HOST_C;
        sink_config.endpoint.ipv4[3] = MELEE_VITA_PROFILER_HOST_D;
        sink_config.endpoint.port = MELEE_VITA_PROFILER_PORT;
        result = vp_vita_tcp_sce_net_ops_init(
            &s_backend, &sink_config.ops);
        report_status("tcp_backend", result, 0);
        if (result != VP_RESULT_OK) goto reconnect;
        sink_config.ops_user = &s_backend;
        memset(&s_sink, 0, sizeof(s_sink));
        result = vp_vita_tcp_sink_init(&s_sink, &sink_config);
        report_status("tcp_sink_init", result, 0);
        if (result != VP_RESULT_OK) goto reconnect;
        sink_initialized = 1;
        result = vp_vita_tcp_sink_connect(&s_sink);
        memset(&sink_stats, 0, sizeof(sink_stats));
        (void) vp_vita_tcp_sink_get_stats(&s_sink, &sink_stats);
        report_status("tcp_connect", result, sink_stats.last_native_error);
        if (result != VP_RESULT_OK) goto reconnect;

        memset(&writer_config, 0, sizeof(writer_config));
        writer_config.context = &s_context;
        writer_config.names = &s_names;
        writer_config.write = vp_vita_tcp_sink_write;
        writer_config.write_user = &s_sink;
        writer_config.dictionary_buffer = s_stream_buffer;
        writer_config.dictionary_buffer_capacity = sizeof(s_stream_buffer);
        memset(&s_writer, 0, sizeof(s_writer));
        result = vp_stream_writer_init_v2(&s_writer, &writer_config);
        report_status("writer_init", result, 0);
        if (result != VP_RESULT_OK) goto reconnect;

        memset(&session, 0, sizeof(session));
        session.session_id = (uint64_t) sceKernelGetSystemTimeWide();
        session.process_id = (uint32_t) sceKernelGetProcessId();
        session.flags = VP_STREAM_V2_SESSION_PROCESS_ID |
                        VP_STREAM_V2_SESSION_TIMER_SOURCE |
                        VP_STREAM_V2_SESSION_TIMER_UNIT;
        session.timer_source = "sceKernelGetSystemTimeWide";
        session.timer_unit = "microseconds";
        result = vp_stream_writer_begin_v2(
            &s_writer, session.session_id, &session);
        report_status("stream_begin", result, 0);
        if (result != VP_RESULT_OK) goto reconnect;
        writer_streaming = 1;

        while (s_running) {
            size_t drained = 0u;
            result = vp_stream_writer_drain_v2(
                &s_writer, PROFILER_DRAIN_EVENTS, &drained);
            if (result != VP_RESULT_OK) break;
            const uint64_t now_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            if (last_stats_us == 0u ||
                now_us - last_stats_us >= 1000000u) {
                struct vp_vita_memory_snapshot memory;
                (void) vp_vita_record_memory(&s_context, &memory);
                result = vp_stream_writer_write_stats_v2(&s_writer);
                if (result != VP_RESULT_OK) break;
                last_stats_us = now_us;
            }
            if (drained == 0u) sceKernelDelayThread(2000);
        }
        report_status("stream_end", result, 0);

reconnect:
        if (writer_streaming) {
            if (!s_running) {
                for (;;) {
                    size_t drained = 0u;
                    if (vp_stream_writer_drain_v2(
                            &s_writer, PROFILER_DRAIN_EVENTS, &drained) !=
                            VP_RESULT_OK || drained == 0u)
                        break;
                }
                (void) vp_stream_writer_write_stats_v2(&s_writer);
                (void) vp_stream_writer_close_v2(&s_writer);
            }
            writer_streaming = 0;
        }
        if (sink_initialized) {
            close_sink();
            sink_initialized = 0;
        }
        if (s_running) sceKernelDelayThread(PROFILER_RECONNECT_DELAY_US);
    }

cleanup:
    s_accepting = 0;
    if (sink_initialized) close_sink();
    stop_network(netctl_initialized, net_initialized, module_loaded);
    return sceKernelExitDeleteThread(0);
}

int melee_vita_profiler_start(void)
{
    struct vp_name_dictionary_config config;
    int result;
    {
        FILE* file = fopen(PROFILER_STATUS_PATH, "w");
        if (file != NULL) {
            fputs("Melee Vita live profiler startup\n", file);
            fclose(file);
        }
    }
    memset(s_pending_duration_us, 0, sizeof(s_pending_duration_us));
    memset(&config, 0, sizeof(config));
    config.entries = s_name_entries;
    config.entry_capacity = PROFILER_NAME_CAPACITY;
    config.text = s_name_text;
    config.text_capacity = sizeof(s_name_text);
    result = vp_name_dictionary_init(&s_names, &config);
    if (result != VP_RESULT_OK) return result;
    s_names_initialized = 1;
    for (unsigned int i = 0u;
         i < sizeof(s_duration_names) / sizeof(s_duration_names[0]); ++i) {
        result = vp_name_dictionary_register(
            &s_names, s_duration_names[i], &s_duration_ids[i]);
        if (result != VP_RESULT_OK) goto failure;
    }
    result = vp_name_dictionary_register(
        &s_names, "melee.frame", &s_frame_id);
    if (result == VP_RESULT_OK)
        result = vp_name_dictionary_register(
            &s_names, "render.draw_calls", &s_draw_calls_id);
    if (result == VP_RESULT_OK)
        result = vp_name_dictionary_register(
            &s_names, "render.triangles", &s_triangles_id);
    static const char* const fragment_metric_names[6] = {
        "render.fragment_stages", "render.texture_samples",
        "render.zero_alpha_discard_draws",
        "render.blended_draws", "render.alpha_test_draws",
        "render.depth_texture_draws",
    };
    for (unsigned int i = 0u; result == VP_RESULT_OK && i < 6u; ++i)
        result = vp_name_dictionary_register(
            &s_names, fragment_metric_names[i], &s_fragment_metric_ids[i]);
    static const char* const queue_metric_names[5] = {
        "render.command_count", "render.command_bytes",
        "render.gpu_arena_bytes", "render.copy_passes",
        "render.scene_begins",
    };
    for (unsigned int i = 0u; result == VP_RESULT_OK && i < 5u; ++i)
        result = vp_name_dictionary_register(
            &s_names, queue_metric_names[i], &s_queue_metric_ids[i]);
    if (result != VP_RESULT_OK ||
        vp_name_dictionary_seal(&s_names) != VP_RESULT_OK)
        goto failure;
    result = vp_vita_init(&s_context, s_slots, PROFILER_RING_CAPACITY);
    if (result != VP_RESULT_OK) goto failure;
    s_context_initialized = 1;
    s_running = 1;
    s_accepting = 1;
    s_thread = sceKernelCreateThread(
        "melee_profiler", profiler_thread, 0x10000100, 128u * 1024u,
        0, 0, NULL);
    report_status("thread_create", s_thread, 0);
    if (s_thread < 0) {
        result = s_thread;
        goto failure;
    }
    result = sceKernelStartThread(s_thread, 0, NULL);
    report_status("thread_start", result, 0);
    if (result < 0) goto failure;
    return 0;

failure:
    s_accepting = 0;
    s_running = 0;
    if (s_thread >= 0) {
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_context_initialized) {
        vp_deinit(&s_context);
        s_context_initialized = 0;
    }
    if (s_names_initialized) {
        vp_name_dictionary_deinit(&s_names);
        s_names_initialized = 0;
    }
    return result;
}

void melee_vita_profiler_stop(void)
{
    s_accepting = 0;
    s_running = 0;
    if (s_thread >= 0) {
        (void) sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        s_thread = -1;
    }
    if (s_context_initialized) {
        vp_deinit(&s_context);
        s_context_initialized = 0;
    }
    if (s_names_initialized) {
        vp_name_dictionary_deinit(&s_names);
        s_names_initialized = 0;
    }
}

void melee_vita_profiler_record_duration(unsigned int zone,
                                         uint64_t elapsed_us)
{
    if (!s_accepting ||
        zone >= sizeof(s_duration_ids) / sizeof(s_duration_ids[0]))
        return;
    if (elapsed_us > UINT32_MAX) elapsed_us = UINT32_MAX;
    (void) __atomic_fetch_add(
        &s_pending_duration_us[zone], (uint32_t) elapsed_us,
        __ATOMIC_RELAXED);
}

void melee_vita_profiler_mark_frame(unsigned int draw_calls,
                                    unsigned int triangles)
{
    if (!s_accepting) return;
    (void) vp_counter(&s_context, s_draw_calls_id, draw_calls);
    (void) vp_counter(&s_context, s_triangles_id, triangles);
    for (unsigned int zone = 0u; zone < PROFILER_DURATION_COUNT; ++zone) {
        const uint32_t elapsed_us = __atomic_exchange_n(
            &s_pending_duration_us[zone], 0u, __ATOMIC_RELAXED);
        if (elapsed_us != 0u) {
            (void) vp_counter(
                &s_context, s_duration_ids[zone], (int64_t) elapsed_us);
        }
    }
    (void) vp_frame_mark(&s_context, s_frame_id);
}

void melee_vita_profiler_fragment_metrics(
    unsigned int stages, unsigned int texture_samples,
    unsigned int zero_alpha_discard_draws,
    unsigned int blended_draws, unsigned int alpha_test_draws,
    unsigned int depth_texture_draws)
{
    const unsigned int values[6] = {
        stages, texture_samples, zero_alpha_discard_draws,
        blended_draws, alpha_test_draws,
        depth_texture_draws,
    };
    if (!s_accepting) return;
    for (unsigned int i = 0u; i < 6u; ++i)
        (void) vp_counter(
            &s_context, s_fragment_metric_ids[i], values[i]);
}

void melee_vita_profiler_queue_metrics(
    unsigned int command_count, unsigned int command_bytes,
    unsigned int gpu_arena_bytes, unsigned int copy_passes,
    unsigned int scene_begins)
{
    const unsigned int values[5] = {
        command_count, command_bytes, gpu_arena_bytes,
        copy_passes, scene_begins,
    };
    if (!s_accepting) return;
    for (unsigned int i = 0u; i < 5u; ++i)
        (void) vp_counter(
            &s_context, s_queue_metric_ids[i], values[i]);
}

#endif
