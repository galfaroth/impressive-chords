#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "expressive_chords_data.h"
#include "expressive_chords_params.h"

#define MAX_PENDING 64

static const host_api_v1_t *g_host = NULL;

typedef struct {
    uint8_t status;
    uint8_t note;
    uint8_t velocity;
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
    
    // Active notes tracking (note number -> chord index triggered it)
    // For each input note, we store up to 12 triggered notes.
    int active_notes[128][12];
    int active_counts[128];
    
    pending_note_t pending[MAX_PENDING];
    int pending_count;
} ec_instance_t;

static void* ec_create_instance(const char *module_dir, const char *config_json) {
    ec_instance_t *inst = calloc(1, sizeof(ec_instance_t));
    if (!inst) return NULL;
    
    inst->preset_idx = 0;
    inst->base_note = 48; // Default to C2
    inst->transpose = 0;
    inst->invert = 0;
    inst->strum = 0;
    inst->tilt = 0;
    inst->articulate = 1;
    inst->pending_count = 0;
    
    return inst;
}

static void ec_destroy_instance(void *instance) {
    free(instance);
}

static void queue_note(ec_instance_t *inst, uint8_t status, uint8_t note, uint8_t velocity, int delay_samples) {
    if (inst->pending_count >= MAX_PENDING) return;
    pending_note_t *p = &inst->pending[inst->pending_count++];
    p->status = status;
    p->note = note;
    p->velocity = velocity;
    p->delay_samples = delay_samples;
}

static int ec_process_midi(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out) {
    ec_instance_t *inst = (ec_instance_t *)instance;
    if (!inst || in_len < 3) return 0;

    uint8_t status = in_msg[0];
    uint8_t note = in_msg[1];
    uint8_t vel = in_msg[2];
    
    uint8_t type = status & 0xF0;
    uint8_t chan = status & 0x0F;

    if (type == 0x90 && vel > 0) { // Note On
        int chord_idx = note - inst->base_note;
        if (chord_idx >= 0 && chord_idx < 48) {
            const preset_def_t *preset = &g_presets[inst->preset_idx];
            const chord_def_t *chord = &preset->chords[chord_idx];
            
            int out_count = 0;
            inst->active_counts[note] = 0;
            
            int N = chord->count;
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
            }

            int sample_rate = g_host ? g_host->sample_rate : 44100;
            int strum_samples = (inst->strum * sample_rate) / 1000;

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
                            
                            inst->active_notes[note][inst->active_counts[note]++] = out_note;
                            out_count++;
                        }
                    } else {
                        queue_note(inst, status, out_note, new_vel, delay);
                        // Still need to track for note off!
                        inst->active_notes[note][inst->active_counts[note]++] = out_note;
                    }
                }
            }
            return out_count;
        }
    }
    else if (type == 0x80 || (type == 0x90 && vel == 0)) { // Note Off
        int count = inst->active_counts[note];
        if (count > 0) {
            int out_count = 0;
            for (int i = 0; i < count; i++) {
                if (out_count >= max_out) break;
                
                out_msgs[out_count][0] = (type == 0x90) ? status : (0x80 | chan);
                out_msgs[out_count][1] = inst->active_notes[note][i];
                out_msgs[out_count][2] = vel;
                out_lens[out_count] = 3;
                out_count++;
            }
            inst->active_counts[note] = 0; // Clear
            return out_count;
        }
    }

    // Pass through for everything else
    if (max_out > 0) {
        memcpy(out_msgs[0], in_msg, in_len);
        out_lens[0] = in_len;
        return 1;
    }
    
    return 0;
}

static int ec_tick(void *instance, int frames, int sample_rate, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    ec_instance_t *inst = (ec_instance_t *)instance;
    if (!inst || inst->pending_count == 0) return 0;

    int count = 0;
    int i = 0;
    while (i < inst->pending_count) {
        pending_note_t *p = &inst->pending[i];
        p->delay_samples -= frames;

        if (p->delay_samples <= 0 && count < max_out) {
            out_msgs[count][0] = p->status;
            out_msgs[count][1] = p->note;
            out_msgs[count][2] = p->velocity;
            out_lens[count] = 3;
            count++;

            // Remove from queue
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

static void ec_set_param(void *instance, const char *key, const char *val) {
    ec_instance_t *inst = (ec_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "preset") == 0) {
        for (int i = 0; i < NUM_PRESETS; i++) {
            if (strcmp(g_presets[i].name, val) == 0) {
                inst->preset_idx = i;
                break;
            }
        }
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
}

static int ec_get_param(void *instance, const char *key, char *buf, int buf_len) {
    ec_instance_t *inst = (ec_instance_t *)instance;
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
    else if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, buf_len, "%s", g_chain_params);
    }
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = ec_create_instance,
    .destroy_instance = ec_destroy_instance,
    .process_midi = ec_process_midi,
    .tick = ec_tick,
    .set_param = ec_set_param,
    .get_param = ec_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
