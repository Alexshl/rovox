// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Test fixture: LV2 effects that pass audio through and kill their host process after a number of run() calls.
//   urn:rovox:test:crash-early  abort() on the 3rd block (the plugin probe finds it)
//   urn:rovox:test:crash-late   SIGSEGV on the 150th block (survives the probe, kills a render)
//   urn:rovox:test:pass         never crashes
// Only the LV2 descriptor ABI is declared here (no lv2 headers needed).
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

typedef struct
{
    const float* in;
    float* out;
    int blocks, crashAt, useAbort;
} Instance;

static LV2_Handle instantiate (const LV2_Descriptor* d, double rate, const char* bundle, const void* const* features)
{
    (void) rate; (void) bundle; (void) features;
    Instance* i = (Instance*) calloc (1, sizeof (Instance));
    i->crashAt = strstr (d->URI, "crash-early") != NULL ? 3 : (strstr (d->URI, "crash-late") != NULL ? 150 : 0);
    i->useAbort = strstr (d->URI, "crash-early") != NULL;
    return i;
}

static void connect_port (LV2_Handle h, uint32_t port, void* data)
{
    Instance* i = (Instance*) h;

    if (port == 0)  i->in = (const float*) data;
    if (port == 1)  i->out = (float*) data;
}

static void activate (LV2_Handle h)     { (void) h; }
static void deactivate (LV2_Handle h)   { (void) h; }
static void cleanup (LV2_Handle h)      { free (h); }
static const void* extension_data (const char* uri) { (void) uri; return NULL; }

static void run (LV2_Handle h, uint32_t n)
{
    Instance* i = (Instance*) h;

    if (i->in != NULL && i->out != NULL)
        memcpy (i->out, i->in, n * sizeof (float));

    if (i->crashAt > 0 && ++i->blocks >= i->crashAt)
    {
        if (i->useAbort)
            abort();

        *(volatile int*) 0 = 1;
    }
}

static const LV2_Descriptor descriptors[3] = {
    { "urn:rovox:test:crash-early", instantiate, connect_port, activate, run, deactivate, cleanup, extension_data },
    { "urn:rovox:test:crash-late",  instantiate, connect_port, activate, run, deactivate, cleanup, extension_data },
    { "urn:rovox:test:pass",        instantiate, connect_port, activate, run, deactivate, cleanup, extension_data },
};

const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index < 3 ? &descriptors[index] : NULL;
}
