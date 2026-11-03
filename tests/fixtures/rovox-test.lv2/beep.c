// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Test fixture: a deterministic LV2 instrument. Every MIDI note-on starts a sine (the note's frequency, velocity-scaled) with a linear decay
// of 150 ms; there is no state, no threads and no sample loading, so it cannot crash and renders the same way every time.
// Only the LV2 ABI that is needed is declared here (no lv2 headers needed).
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef void* LV2_Handle;
typedef struct LV2_Descriptor
{
    const char* URI;
    LV2_Handle (*instantiate) (const struct LV2_Descriptor*, double, const char*, const void* const*);
    void (*connect_port) (LV2_Handle, uint32_t, void*);
    void (*activate) (LV2_Handle);
    void (*run) (LV2_Handle, uint32_t);
    void (*deactivate) (LV2_Handle);
    void (*cleanup) (LV2_Handle);
    const void* (*extension_data) (const char*);
} LV2_Descriptor;

typedef struct { const char* URI; void* data; } LV2_Feature;
typedef struct { void* handle; uint32_t (*map) (void* handle, const char* uri); } LV2_URID_Map;
typedef struct { uint32_t size, type; } LV2_Atom;
typedef struct { LV2_Atom atom; uint32_t unit, pad; } LV2_Atom_Sequence;     // events follow
typedef struct { int64_t frames; LV2_Atom body; } LV2_Atom_Event;           // MIDI bytes follow

#define VOICES 16

typedef struct
{
    const LV2_Atom_Sequence* events;
    float* out[2];
    double rate;
    uint32_t midiEvent;
    double phase[VOICES], inc[VOICES];
    double amp[VOICES];
    int left[VOICES];       // samples left; 0 = free
    int next;
} Instance;

static LV2_Handle instantiate (const LV2_Descriptor* d, double rate, const char* bundle, const void* const* features)
{
    (void) d; (void) bundle;
    Instance* i = (Instance*) calloc (1, sizeof (Instance));
    i->rate = rate;

    for (int f = 0; features != NULL && features[f] != NULL; ++f)
    {
        const LV2_Feature* feature = (const LV2_Feature*) features[f];

        if (strcmp (feature->URI, "http://lv2plug.in/ns/ext/urid#map") == 0)
        {
            const LV2_URID_Map* map = (const LV2_URID_Map*) feature->data;
            i->midiEvent = map->map (map->handle, "http://lv2plug.in/ns/ext/midi#MidiEvent");
        }
    }

    return i;
}

static void connect_port (LV2_Handle h, uint32_t port, void* data)
{
    Instance* i = (Instance*) h;

    if (port == 0)  i->events = (const LV2_Atom_Sequence*) data;
    if (port == 1)  i->out[0] = (float*) data;
    if (port == 2)  i->out[1] = (float*) data;
}

static void activate (LV2_Handle h)
{
    Instance* i = (Instance*) h;
    memset (i->left, 0, sizeof (i->left));
}

static void deactivate (LV2_Handle h)   { (void) h; }
static void cleanup (LV2_Handle h)      { free (h); }
static const void* extension_data (const char* uri) { (void) uri; return NULL; }

static void start (Instance* i, int note, int velocity)
{
    const int v = i->next++ % VOICES;
    i->phase[v] = 0.0;
    i->inc[v] = 2.0 * M_PI * 440.0 * pow (2.0, (note - 69) / 12.0) / i->rate;
    i->amp[v] = 0.4 * velocity / 127.0;
    i->left[v] = (int) (0.15 * i->rate);
}

static void mix (Instance* i, uint32_t from, uint32_t to)
{
    for (int v = 0; v < VOICES; ++v)
        for (uint32_t s = from; s < to && i->left[v] > 0; ++s, --i->left[v])
        {
            const float x = (float) (sin (i->phase[v]) * i->amp[v] * i->left[v] / (0.15 * i->rate));
            i->phase[v] += i->inc[v];
            i->out[0][s] += x;
            i->out[1][s] += x;
        }
}

static void run (LV2_Handle h, uint32_t n)
{
    Instance* i = (Instance*) h;

    if (i->out[0] == NULL || i->out[1] == NULL)
        return;

    memset (i->out[0], 0, n * sizeof (float));
    memset (i->out[1], 0, n * sizeof (float));
    uint32_t done = 0;

    if (i->events != NULL)
    {
        const uint8_t* p = (const uint8_t*) (i->events + 1);
        const uint8_t* end = (const uint8_t*) &i->events->unit + i->events->atom.size;

        while (p + sizeof (LV2_Atom_Event) <= end)
        {
            const LV2_Atom_Event* e = (const LV2_Atom_Event*) p;
            const uint8_t* midi = (const uint8_t*) (e + 1);
            uint32_t at = e->frames < 0 ? 0 : (uint32_t) e->frames;

            if (at > n)
                at = n;

            if (e->body.type == i->midiEvent && e->body.size >= 3 && (midi[0] & 0xf0) == 0x90 && midi[2] > 0)
            {
                if (at > done)
                {
                    mix (i, done, at);
                    done = at;
                }

                start (i, midi[1], midi[2]);
            }

            p += sizeof (LV2_Atom_Event) + ((e->body.size + 7u) & ~7u);
        }
    }

    mix (i, done, n);
}

static const LV2_Descriptor descriptor = { "urn:rovox:test:beep", instantiate, connect_port, activate, run, deactivate, cleanup, extension_data };

const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
