#include "figatree.h"

#include <math.h>
#include <string.h>

enum {
    OP_CON = 1,
    OP_LIN = 2,
    OP_SPL0 = 3,
    OP_SPL = 4,
    OP_SLP = 5,
    OP_KEY = 6,
};

struct track_state {
    const unsigned char* ad;
    const unsigned char* end;
    uint8_t frac_value;
    uint8_t frac_slope;
    uint8_t state;
    uint8_t flags;
    uint8_t op;
    uint8_t op_intrp;
    uint16_t nb_pack;
    uint16_t fterm;
    float time;
    float p0, p1, d0, d1;
};

static uint16_t be16(const unsigned char* p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

static int in_data(const void* pointer, size_t size,
                   const unsigned char* data, uint32_t data_size)
{
    if (pointer == NULL) return 0;
    const unsigned char* p = pointer;
    return p >= data && size <= data_size && p <= data + data_size - size;
}

static int take(struct track_state* s, uint32_t count,
                const unsigned char** out)
{
    if ((size_t) (s->end - s->ad) < count) return 0;
    *out = s->ad;
    s->ad += count;
    return 1;
}

/* Packed FObj samples are little-endian even though their descriptors and
 * every ordinary DAT structure are big-endian. This matches parseFloat in
 * the original HSD FObj interpreter. */
static int parse_float(struct track_state* s, uint8_t frac, float* out)
{
    const unsigned char* p;
    const uint32_t format = frac & 0xe0u;
    const uint32_t shift = frac & 0x1fu;
    float numerator;
    if (format == 0u) {
        if (!take(s, 4u, &p)) return 0;
        const uint32_t bits = (uint32_t) p[0] | (uint32_t) p[1] << 8 |
                              (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
        memcpy(out, &bits, sizeof(*out));
        return isfinite(*out);
    }
    if (format == 0x60u || format == 0x80u) {
        if (!take(s, 1u, &p)) return 0;
        numerator = format == 0x60u ? (float) (int8_t) p[0] : (float) p[0];
    } else if (format == 0x20u || format == 0x40u) {
        if (!take(s, 2u, &p)) return 0;
        const uint16_t value = (uint16_t) p[0] | (uint16_t) p[1] << 8;
        numerator = format == 0x20u ? (float) (int16_t) value : (float) value;
    } else {
        return 0;
    }
    *out = numerator / (float) (UINT32_C(1) << shift);
    return 1;
}

static int parse_pack(struct track_state* s, uint32_t* count)
{
    const unsigned char* p;
    if (!take(s, 1u, &p)) return 0;
    uint8_t d = p[0];
    uint32_t value = ((d >> 4) & 7u) + 1u;
    uint32_t shift = 3u;
    while ((d & 0x80u) != 0u) {
        if (!take(s, 1u, &p) || shift >= 32u) return 0;
        d = p[0];
        value += (uint32_t) (d & 0x7fu) << shift;
        shift += 7u;
    }
    *count = value;
    return 1;
}

static int parse_wait(struct track_state* s, uint16_t* wait)
{
    const unsigned char* p;
    uint32_t value = 0u, shift = 0u;
    uint8_t d;
    do {
        if (!take(s, 1u, &p) || shift >= 21u) return 0;
        d = p[0];
        value |= (uint32_t) (d & 0x7fu) << shift;
        shift += 7u;
    } while ((d & 0x80u) != 0u);
    if (value > UINT16_MAX) return 0;
    *wait = (uint16_t) value;
    return 1;
}

static void launch_key(struct track_state* s)
{
    if ((s->flags & 0x40u) != 0u) {
        s->op_intrp = s->op;
        s->flags &= (uint8_t) ~0x40u;
        s->flags |= 0x80u;
        s->p0 = s->p1;
    }
}

static int load_data(struct track_state* s)
{
    if (s->ad >= s->end) return 6;
    s->op_intrp = s->op;
    if (s->nb_pack == 0u) {
        s->op = s->ad[0] & 0x0fu;
        uint32_t count;
        if (!parse_pack(s, &count) || count > UINT16_MAX) return -1;
        s->nb_pack = (uint16_t) count;
    }
    --s->nb_pack;
    const uint8_t initial = s->state == 1u;
    switch (s->op) {
    case OP_CON:
    case OP_LIN:
        s->p0 = s->p1;
        if (!parse_float(s, s->frac_value, &s->p1)) return -1;
        if (s->op_intrp != OP_SLP) {
            s->d0 = s->d1;
            s->d1 = 0.0f;
        }
        break;
    case OP_SPL0:
        s->p0 = s->p1;
        s->d0 = s->d1;
        if (!parse_float(s, s->frac_value, &s->p1)) return -1;
        s->d1 = 0.0f;
        break;
    case OP_SPL:
        s->p0 = s->p1;
        s->d0 = s->d1;
        if (!parse_float(s, s->frac_value, &s->p1) ||
            !parse_float(s, s->frac_slope, &s->d1))
            return -1;
        break;
    case OP_SLP:
        s->d0 = s->d1;
        if (!parse_float(s, s->frac_slope, &s->d1)) return -1;
        return s->state;
    case OP_KEY:
        launch_key(s);
        if (!parse_float(s, s->frac_value, &s->p1)) return -1;
        s->flags |= 0x40u;
        break;
    default:
        return -1;
    }
    return initial ? 3 : 4;
}

static float hermite(float reciprocal_term, float time, float p0, float p1,
                     float d0, float d1)
{
    const float time2 = time * time;
    const float reciprocal2 = reciprocal_term * reciprocal_term;
    const float t2_over_t = time2 * reciprocal_term;
    const float t3_over_t2 = reciprocal2 * (time2 * time);
    const float two_t3 = 2.0f * t3_over_t2 * reciprocal_term;
    const float three_t2 = 3.0f * time2 * reciprocal2;
    return d1 * (t3_over_t2 - t2_over_t) +
           d0 * (time + ((t3_over_t2 - t2_over_t) - t2_over_t)) +
           p0 * (1.0f + (two_t3 - three_t2)) +
           p1 * (-two_t3 + three_t2);
}

static int current_value(struct track_state* s, float* value)
{
    switch (s->op_intrp) {
    case OP_KEY:
        if ((s->flags & 0x80u) == 0u) return 0;
        *value = s->p0;
        s->flags &= (uint8_t) ~0x80u;
        return 1;
    case OP_CON:
        *value = s->time >= s->fterm ? s->p1 : s->p0;
        return 1;
    case OP_LIN:
        if ((s->flags & 0x20u) != 0u) {
            s->flags &= (uint8_t) ~0x20u;
            if (s->fterm != 0u)
                s->d0 = (s->p1 - s->p0) / s->fterm;
            else {
                s->d0 = 0.0f;
                s->p0 = s->p1;
            }
        }
        *value = s->d0 * s->time + s->p0;
        return 1;
    case OP_SPL0:
    case OP_SPL:
    case OP_SLP:
        *value = s->fterm != 0u
                     ? hermite(1.0f / s->fterm, s->time, s->p0, s->p1,
                               s->d0, s->d1)
                     : s->p1;
        return 1;
    default:
        return 0;
    }
}

static int sample_track(const unsigned char* track,
                        const unsigned char* data, uint32_t data_size,
                        float frame, uint8_t* object_type, float* value)
{
    const uint16_t length = be16(track);
    void* stream_pointer = NULL;
    memcpy(&stream_pointer, track + 8u, sizeof(stream_pointer));
    if (!in_data(stream_pointer, length, data, data_size)) return 0;
    struct track_state s;
    memset(&s, 0, sizeof(s));
    s.ad = stream_pointer;
    s.end = s.ad + length;
    s.frac_value = track[5];
    s.frac_slope = track[6];
    s.state = 1u;
    s.time = (float) (int16_t) be16(track + 2u) + frame;
    *object_type = track[4];

    float carried_term = 0.0f;
    int has_value = 0;
    for (uint32_t steps = 0; steps < 4096u; ++steps) {
        switch (s.state) {
        case 1:
        case 2: {
            const int next = load_data(&s);
            if (next < 0) return 0;
            s.state = (uint8_t) next;
            break;
        }
        case 3:
            if ((s.flags & 0x80u) != 0u && current_value(&s, value))
                has_value = 1;
            if (s.ad >= s.end) {
                s.state = 6u;
            } else {
                if (!parse_wait(&s, &s.fterm)) return 0;
                s.flags |= 0x20u;
                s.state = 2u;
            }
            break;
        case 4:
            if (s.fterm <= s.time) {
                carried_term = s.fterm;
                s.time -= s.fterm;
                s.state = 3u;
            } else {
                return current_value(&s, value);
            }
            break;
        case 5:
            s.state = 4u;
            break;
        case 6:
            s.time += carried_term;
            launch_key(&s);
            if (current_value(&s, value)) return 1;
            return has_value;
        default:
            return 0;
        }
    }
    return 0;
}

int melee_vita_figatree_sample(
    const void* tree_pointer, const unsigned char* data, uint32_t data_size,
    float frame, struct melee_vita_joint_pose* poses, uint32_t pose_capacity,
    uint32_t* joint_count, uint32_t* track_count)
{
    const unsigned char* tree = tree_pointer;
    if (!in_data(tree, 0x14u, data, data_size)) return -1;
    void* nodes_pointer = NULL;
    void* tracks_pointer = NULL;
    memcpy(&nodes_pointer, tree + 0x0cu, sizeof(nodes_pointer));
    memcpy(&tracks_pointer, tree + 0x10u, sizeof(tracks_pointer));
    const int8_t* nodes = nodes_pointer;
    const unsigned char* tracks = tracks_pointer;
    if (!in_data(nodes, 1u, data, data_size) ||
        !in_data(tracks, 1u, data, data_size))
        return -2;
    memset(poses, 0, (size_t) pose_capacity * sizeof(*poses));
    uint32_t joints = 0u, tracks_seen = 0u;
    while (joints < pose_capacity) {
        if (!in_data(nodes + joints, 1u, data, data_size)) return -3;
        const int count = nodes[joints];
        if (count == -1) break;
        if (count < 0 || count > 10) return -4;
        for (int i = 0; i < count; ++i, ++tracks_seen) {
            const unsigned char* track = tracks + tracks_seen * 12u;
            if (!in_data(track, 12u, data, data_size)) return -5;
            uint8_t type;
            float value;
            if (!sample_track(track, data, data_size, frame, &type, &value) ||
                !isfinite(value))
                return -6;
            if (type >= 1u && type <= 10u && type != 4u) {
                poses[joints].value_mask |= (uint16_t) (1u << (type - 1u));
                if (type <= 3u)
                    poses[joints].rotation[type - 1u] = value;
                else if (type <= 7u)
                    poses[joints].translation[type - 5u] = value;
                else
                    poses[joints].scale[type - 8u] =
                        fabsf(value) < 1.0e-3f ? 1.0e-3f : value;
            }
        }
        ++joints;
    }
    if (joints == pose_capacity || tracks_seen == 0u) return -7;
    *joint_count = joints;
    *track_count = tracks_seen;
    return 0;
}
