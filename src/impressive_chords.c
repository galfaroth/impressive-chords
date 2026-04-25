#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "impressive_chords_data.h"
#include "impressive_chords_params.h"

#define MAX_PENDING 256
#define MAX_ACTIVE_PER_NOTE 32
#define RELEASE_STAGGER_SAMPLES 64

/* JSON helpers for state parsing */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len < 1) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return 0;
    int len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len);
    out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

static const host_api_v1_t *g_host = NULL;

typedef struct {
    uint8_t status;
    uint8_t note;
    uint8_t velocity;
    uint8_t input_note;
    int delay_samples;
} pending_note_t;

typedef struct {
    int preset_idx;
    int base_note;
    int transpose;
    int invert;
    int strum;
    int tilt;
    int articulate;
    int length;
    
    // Retriggering
    int retrigger;
    int timing; // 0: Straight, 1: Dotted, 2: Triplet
    int clock_counter;
    int clocks_per_retrigger;
    int held_note;
    uint8_t held_velocity;
    char last_val[32];
    
    // New features
    int choke;
    int notes;
    int fit;
    int gate;
    int samples_since_last_clock;
    int samples_per_clock;
    
    // Currently-audible voices per input note (populated when note-on actually fires)
    uint8_t active_notes[128][MAX_ACTIVE_PER_NOTE];
    int active_counts[128];

    // Pad currently held per input note
    uint8_t pad_held[128];

    pending_note_t pending[MAX_PENDING];
    int pending_count;
} ic_instance_t;

static void* ic_create_instance(const char *module_dir, const char *config_json) {
    ic_instance_t *inst = calloc(1, sizeof(ic_instance_t));
    if (!inst) return NULL;
    
    inst->preset_idx = 0;
    inst->base_note = 48; // Default to C2
    inst->transpose = 0;
    inst->invert = 0;
    inst->strum = 0;
    inst->tilt = 0;
    inst->articulate = 1;
    inst->length = 200; // Default 200ms
    inst->gate = 0;
    inst->retrigger = 0;
    inst->timing = 0;
    inst->clock_counter = 0;
    inst->clocks_per_retrigger = 0;
    inst->held_note = -1;
    inst->held_velocity = 0;
    inst->pending_count = 0;
    
    return inst;
}

static void ic_destroy_instance(void *instance) {
    free(instance);
}

static int queue_note(ic_instance_t *inst, uint8_t status, uint8_t note,
                      uint8_t velocity, uint8_t input_note, int delay_samples) {
    if (inst->pending_count >= MAX_PENDING) return 0;
    pending_note_t *p = &inst->pending[inst->pending_count++];
    p->status = status;
    p->note = note;
    p->velocity = velocity;
    p->input_note = input_note;
    p->delay_samples = delay_samples;
    return 1;
}

static inline void active_append(ic_instance_t *inst, uint8_t input_note, uint8_t voice_note) {
    if (input_note >= 128) return;
    int n = inst->active_counts[input_note];
    if (n < MAX_ACTIVE_PER_NOTE) {
        inst->active_notes[input_note][n] = voice_note;
        inst->active_counts[input_note] = n + 1;
    }
}

static inline int active_remove_one(ic_instance_t *inst, uint8_t input_note, uint8_t voice_note) {
    if (input_note >= 128) return 0;
    int n = inst->active_counts[input_note];
    for (int i = 0; i < n; i++) {
        if (inst->active_notes[input_note][i] == voice_note) {
            for (int j = i; j < n - 1; j++) {
                inst->active_notes[input_note][j] = inst->active_notes[input_note][j + 1];
            }
            inst->active_counts[input_note] = n - 1;
            return 1;
        }
    }
    return 0;
}

static int trigger_chord(ic_instance_t *inst, uint8_t status, uint8_t note, uint8_t vel, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int chord_idx = note - inst->base_note;
    if (chord_idx < 0 || chord_idx >= 48) return 0;

    const preset_def_t *preset = &g_presets[inst->preset_idx];
    const chord_def_t *chord = &preset->chords[chord_idx];
    
    int out_count = 0;
    
    if (inst->choke) {
        uint8_t chan = status & 0x0F;
        for (int n = 0; n < 128; n++) {
            for (int i = 0; i < inst->active_counts[n]; i++) {
                int active_note = inst->active_notes[n][i];
                if (out_count < max_out) {
                    out_msgs[out_count][0] = 0x80 | chan;
                    out_msgs[out_count][1] = active_note;
                    out_msgs[out_count][2] = 0;
                    out_lens[out_count] = 3;
                    out_count++;
                }
            }
            inst->active_counts[n] = 0;
        }
        inst->pending_count = 0; // Cancel all pending strums and note-offs
    }
    
    int N = chord->count;
    if (inst->notes > 0 && inst->notes < N) {
        N = inst->notes;
    }
    int order[12];
    for (int i = 0; i < N; i++) order[i] = i;

    // Articulate implies different strum order
    if (inst->articulate == 2) { // Outside-In
        int left = 0;
        int right = N - 1;
        for (int i = 0; i < N; i++) {
            if (i % 2 == 0) order[i] = left++;
            else order[i] = right--;
        }
    } else if (inst->articulate == 3) { // High to Low
        for (int i = 0; i < N; i++) order[i] = N - 1 - i;
    } else if (inst->articulate == 4) { // Even/Odd
        int idx = 0;
        for (int i = 0; i < N; i += 2) order[idx++] = i;
        for (int i = 1; i < N; i += 2) order[idx++] = i;
    } else if (inst->articulate == 5) { // Inside-Out
        int left = (N - 1) / 2;
        int right = left + 1;
        for (int i = 0; i < N; i++) {
            if (i % 2 == 0) {
                if (left >= 0) order[i] = left--;
                else order[i] = right++;
            } else {
                if (right < N) order[i] = right++;
                else order[i] = left--;
            }
        }
    } else if (inst->articulate == 6) { // Odd/Even
        int idx = 0;
        for (int i = 1; i < N; i += 2) order[idx++] = i;
        for (int i = 0; i < N; i += 2) order[idx++] = i;
    }

    int sample_rate = g_host ? g_host->sample_rate : 44100;
    int strum_samples = (inst->strum * sample_rate) / 1000;
    int length_samples = (inst->length * sample_rate) / 1000;
    
    if (inst->fit && inst->retrigger > 0 && inst->samples_per_clock > 0) {
        int retrigger_length_samples = (inst->clocks_per_retrigger * inst->samples_per_clock) / 4;
        if (N > 0) {
            strum_samples = retrigger_length_samples / N;
            length_samples = (strum_samples * 8) / 10; // 80% of strum step
        }
    }
    uint8_t chan = status & 0x0F;

    for (int i = 0; i < N; i++) {
        int idx = order[i]; // Play order index
        int out_note = chord->notes[idx] + inst->transpose;
        
        // Inversion
        if (inst->invert > 0 && idx < inst->invert) {
            out_note += 12;
        } else if (inst->invert < 0 && idx >= N + inst->invert) {
            out_note -= 12;
        }
        
        // Tilt implies velocity ramp
        int new_vel = vel;
        if (inst->tilt != 0 && N > 1) {
            float pos = (2.0f * idx / (N - 1)) - 1.0f; // Use original position for tilt
            new_vel = vel + (int)(vel * (inst->tilt / 100.0f) * pos);
            if (new_vel < 1) new_vel = 1;
            if (new_vel > 127) new_vel = 127;
        }

        int delay = i * strum_samples; // Use play order for delay

        if (out_note >= 0 && out_note <= 127) {
            if (delay == 0 || strum_samples == 0) {
                if (out_count < max_out) {
                    out_msgs[out_count][0] = status;
                    out_msgs[out_count][1] = out_note;
                    out_msgs[out_count][2] = new_vel;
                    out_lens[out_count] = 3;
                    
                    active_append(inst, note, (uint8_t)out_note);
                    out_count++;
                }
            } else {
                queue_note(inst, status, out_note, new_vel, note, delay);
                // Don't append to active_notes yet — tick adds on fire
            }
            // No paired note-off queued here: pad release emits offs
            // for all currently-audible voices; for strum voices that
            // fire post-release, tick queues a note-off at length_samples
            // when the note-on actually fires.
        }
    }
    return out_count;
}

static int ic_process_midi(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out) {
    ic_instance_t *inst = (ic_instance_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0];
    
    // Handle MIDI clock messages when retrigger is enabled
    if (status == 0xF8) {  // Timing clock
        inst->samples_per_clock = inst->samples_since_last_clock;
        inst->samples_since_last_clock = 0;

        if (inst->retrigger > 0 && inst->held_note >= 0) {
            inst->clock_counter += 4; // 4x multiplier for higher resolution
            if (inst->clock_counter >= inst->clocks_per_retrigger) {
                inst->clock_counter = 0;
                // Retrigger chord!
                uint8_t note_on_status = 0x90; // Assume channel 0
                return trigger_chord(inst, note_on_status, inst->held_note, inst->held_velocity, out_msgs, out_lens, max_out);
            }
        }
        return 0;  // Don't pass clock through
    }

    if (in_len < 3) return 0;

    uint8_t note = in_msg[1];
    uint8_t vel = in_msg[2];
    
    uint8_t type = status & 0xF0;

    if (type == 0x90 && vel > 0) { // Note On
        inst->pad_held[note] = 1;
        inst->held_note = note;
        inst->held_velocity = vel;
        inst->clock_counter = 0;
        return trigger_chord(inst, status, note, vel, out_msgs, out_lens, max_out);
    }
    else if (type == 0x80 || (type == 0x90 && vel == 0)) { // Note Off
        inst->pad_held[note] = 0;
        if (note == inst->held_note) {
            inst->held_note = -1;
            inst->clock_counter = 0;
        }
        
        int n_active = inst->active_counts[note];
        if (n_active == 0) return 0;

        uint8_t direct_emit[MAX_ACTIVE_PER_NOTE];
        int n_direct = 0;
        uint8_t chan = status & 0x0F;

        for (int i = 0; i < n_active; i++) {
            uint8_t voice = inst->active_notes[note][i];
            int ok = 0;
            if (i > 0) {
                ok = queue_note(inst, 0x80 | chan, voice, 0, note, i * RELEASE_STAGGER_SAMPLES);
            }
            if (!ok) {
                if (n_direct < MAX_ACTIVE_PER_NOTE) {
                    direct_emit[n_direct++] = voice;
                }
            }
        }

        int out_count = 0;
        for (int i = 0; i < n_direct && out_count < max_out; i++) {
            out_msgs[out_count][0] = 0x80 | chan;
            out_msgs[out_count][1] = direct_emit[i];
            out_msgs[out_count][2] = 0;
            out_lens[out_count] = 3;
            out_count++;
            active_remove_one(inst, note, direct_emit[i]);
        }
        return out_count;
    }

    // Pass through for everything else
    if (max_out > 0) {
        memcpy(out_msgs[0], in_msg, in_len);
        out_lens[0] = in_len;
        return 1;
    }
    
    return 0;
}

static int ic_tick(void *instance, int frames, int sample_rate, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    ic_instance_t *inst = (ic_instance_t *)instance;
    if (!inst) return 0;

    inst->samples_since_last_clock += frames;

    if (inst->pending_count == 0) return 0;

    int count = 0;
    int i = 0;
    while (i < inst->pending_count) {
        pending_note_t *p = &inst->pending[i];
        p->delay_samples -= frames;

        if (p->delay_samples <= 0) {
            uint8_t ptype = p->status & 0xF0;
            int is_off = (ptype == 0x80) || (ptype == 0x90 && p->velocity == 0);

            int emit = 0;
            if (is_off) {
                if (!inst->pad_held[p->input_note]) {
                    emit = active_remove_one(inst, p->input_note, p->note);
                }
            } else {
                emit = 1;
                active_append(inst, p->input_note, p->note);

                if (!inst->pad_held[p->input_note]) {
                    int length_samples = (inst->length * sample_rate) / 1000;
                    queue_note(inst, 0x80 | (p->status & 0x0F),
                               p->note, 0, p->input_note, length_samples);
                }
            }

            if (emit && count < max_out) {
                out_msgs[count][0] = p->status;
                out_msgs[count][1] = p->note;
                out_msgs[count][2] = p->velocity;
                out_lens[count] = 3;
                count++;
            } else if (emit) {
                i++;
                continue;
            }

            for (int j = i; j < inst->pending_count - 1; j++) {
                inst->pending[j] = inst->pending[j + 1];
            }
            inst->pending_count--;
        } else {
            i++;
        }
    }

    return count;
}

static void update_clocks_per_retrigger(ic_instance_t *inst) {
    if (inst->retrigger == 0) {
        inst->clocks_per_retrigger = 0;
        return;
    }
    
    int base_ticks = 0;
    switch (inst->retrigger) {
        case 1: base_ticks = 384; break; // 1 Bar
        case 2: base_ticks = 192; break; // 1/2 Bar
        case 3: base_ticks = 96;  break; // 1 Beat
        case 4: base_ticks = 48;  break; // 1/2 Beat
        case 5: base_ticks = 24;  break; // 1/4 Beat
        case 6: base_ticks = 12;  break; // 1/8 Beat
        case 7: base_ticks = 6;   break; // 1/16 Beat
        case 8: base_ticks = 3;   break; // 1/32 Beat
        default: base_ticks = 0; break;
    }
    
    if (inst->timing == 1) { // Dotted
        inst->clocks_per_retrigger = (base_ticks * 3) / 2;
    } else if (inst->timing == 2) { // Triplet
        inst->clocks_per_retrigger = (base_ticks * 2) / 3;
    } else { // Straight
        inst->clocks_per_retrigger = base_ticks;
    }
    
    if (inst->clocks_per_retrigger < 1) inst->clocks_per_retrigger = 1;
}

static void ic_set_param(void *instance, const char *key, const char *val) {
    ic_instance_t *inst = (ic_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "preset") == 0) {
        float f = atof(val);
        if (strchr(val, '.') && f >= 0.0f && f <= 1.0f) {
            inst->preset_idx = (int)(f * (NUM_PRESETS - 1) + 0.5f);
        } else {
            // Try to match by name first
            int found = 0;
            for (int i = 0; i < NUM_PRESETS; i++) {
                if (strcmp(g_presets[i].name, val) == 0) {
                    inst->preset_idx = i;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                // Fallback to atoi if it's an index string
                inst->preset_idx = atoi(val);
            }
        }
        if (inst->preset_idx < 0) inst->preset_idx = 0;
        if (inst->preset_idx >= NUM_PRESETS) inst->preset_idx = NUM_PRESETS - 1;
    }
    else if (strcmp(key, "base_note") == 0) {
        inst->base_note = atoi(val);
    }
    else if (strcmp(key, "transpose") == 0) {
        inst->transpose = atoi(val);
    }
    else if (strcmp(key, "invert") == 0) {
        inst->invert = atoi(val);
    }
    else if (strcmp(key, "strum") == 0) {
        inst->strum = atoi(val);
    }
    else if (strcmp(key, "tilt") == 0) {
        inst->tilt = atoi(val);
    }
    else if (strcmp(key, "articulate") == 0) {
        inst->articulate = atoi(val);
    }
    else if (strcmp(key, "length") == 0) {
        inst->length = atoi(val);
    }
    else if (strcmp(key, "retrig") == 0) {
        if (val[0] >= '0' && val[0] <= '8') {
            inst->retrigger = val[0] - '0';
        }
        update_clocks_per_retrigger(inst);
        inst->clock_counter = 0;
    }
    else if (strcmp(key, "timing") == 0) {
        if (strcmp(val, "Straight") == 0) inst->timing = 0;
        else if (strcmp(val, "Dotted") == 0) inst->timing = 1;
        else if (strcmp(val, "Triplet") == 0) inst->timing = 2;
        else if (val[0] >= '0' && val[0] <= '9') {
            inst->timing = atoi(val);
        }
        update_clocks_per_retrigger(inst);
        inst->clock_counter = 0;
    }
    else if (strcmp(key, "choke") == 0) {
        inst->choke = (val[0] == '1');
    }
    else if (strcmp(key, "notes") == 0) {
        inst->notes = atoi(val);
    }
    else if (strcmp(key, "fit") == 0) {
        inst->fit = (val[0] == '1');
    }
    else if (strcmp(key, "gate") == 0) {
        inst->gate = (val[0] == '1');
    }
    else if (strcmp(key, "state") == 0) {
        char preset_str[64];
        if (json_get_string(val, "preset", preset_str, sizeof(preset_str))) {
            float f = atof(preset_str);
            if (strchr(preset_str, '.') && f >= 0.0f && f <= 1.0f) {
                inst->preset_idx = (int)(f * (NUM_PRESETS - 1) + 0.5f);
            } else {
                // Try to match by name first
                int found = 0;
                for (int i = 0; i < NUM_PRESETS; i++) {
                    if (strcmp(g_presets[i].name, preset_str) == 0) {
                        inst->preset_idx = i;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    // Fallback to atoi if it's an index string or prefixed label
                    if (preset_str[0] >= '0' && preset_str[0] <= '9') {
                        inst->preset_idx = atoi(preset_str);
                    }
                }
            }
            if (inst->preset_idx < 0) inst->preset_idx = 0;
            if (inst->preset_idx >= NUM_PRESETS) inst->preset_idx = NUM_PRESETS - 1;
        }
        
        json_get_int(val, "base_note", &inst->base_note);
        json_get_int(val, "transpose", &inst->transpose);
        json_get_int(val, "invert", &inst->invert);
        json_get_int(val, "strum", &inst->strum);
        json_get_int(val, "tilt", &inst->tilt);
        json_get_int(val, "articulate", &inst->articulate);
        json_get_int(val, "length", &inst->length);
        
        json_get_int(val, "retrig", &inst->retrigger);

        json_get_int(val, "timing", &inst->timing);
        json_get_int(val, "choke", &inst->choke);
        json_get_int(val, "notes", &inst->notes);
        json_get_int(val, "fit", &inst->fit);
        json_get_int(val, "gate", &inst->gate);
        
        update_clocks_per_retrigger(inst);
        inst->clock_counter = 0;
    }
}

static int ic_get_param(void *instance, const char *key, char *buf, int buf_len) {
    ic_instance_t *inst = (ic_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%s", g_presets[inst->preset_idx].name);
    }
    else if (strcmp(key, "base_note") == 0) {
        return snprintf(buf, buf_len, "%d", inst->base_note);
    }
    else if (strcmp(key, "transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->transpose);
    }
    else if (strcmp(key, "invert") == 0) {
        return snprintf(buf, buf_len, "%d", inst->invert);
    }
    else if (strcmp(key, "strum") == 0) {
        return snprintf(buf, buf_len, "%d", inst->strum);
    }
    else if (strcmp(key, "tilt") == 0) {
        return snprintf(buf, buf_len, "%d", inst->tilt);
    }
    else if (strcmp(key, "articulate") == 0) {
        return snprintf(buf, buf_len, "%d", inst->articulate);
    }
    else if (strcmp(key, "length") == 0) {
        return snprintf(buf, buf_len, "%d", inst->length);
    }
    else if (strcmp(key, "retrig") == 0) {
        const char *val = "0 Off";
        switch (inst->retrigger) {
            case 1: val = "1 1B"; break;
            case 2: val = "2 1/2B"; break;
            case 3: val = "3 1b"; break;
            case 4: val = "4 1/2b"; break;
            case 5: val = "5 1/4b"; break;
            case 6: val = "6 1/8b"; break;
            case 7: val = "7 1/16b"; break;
            case 8: val = "8 1/32b"; break;
            default: break;
        }
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "timing") == 0) {
        const char *val = "Straight";
        switch (inst->timing) {
            case 1: val = "Dotted"; break;
            case 2: val = "Triplet"; break;
            default: break;
        }
        return snprintf(buf, buf_len, "%s", val);
    }
    else if (strcmp(key, "choke") == 0) {
        return snprintf(buf, buf_len, "%d", inst->choke);
    }
    else if (strcmp(key, "notes") == 0) {
        return snprintf(buf, buf_len, "%d", inst->notes);
    }
    else if (strcmp(key, "fit") == 0) {
        return snprintf(buf, buf_len, "%d", inst->fit);
    }
    else if (strcmp(key, "gate") == 0) {
        return snprintf(buf, buf_len, "%d", inst->gate);
    }
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len, "{\"preset\":\"%s\",\"base_note\":%d,\"transpose\":%d,\"invert\":%d,\"strum\":%d,\"tilt\":%d,\"articulate\":%d,\"length\":%d,\"retrig\":%d,\"timing\":%d,\"choke\":%d,\"notes\":%d,\"fit\":%d,\"gate\":%d}",
            g_presets[inst->preset_idx].name,
            inst->base_note,
            inst->transpose,
            inst->invert,
            inst->strum,
            inst->tilt,
            inst->articulate,
            inst->length,
            inst->retrigger,
            inst->timing,
            inst->choke,
            inst->notes,
            inst->fit,
            inst->gate);
    }
    else if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, buf_len, "%s", g_chain_params);
    }
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = ic_create_instance,
    .destroy_instance = ic_destroy_instance,
    .process_midi = ic_process_midi,
    .tick = ic_tick,
    .set_param = ic_set_param,
    .get_param = ic_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
