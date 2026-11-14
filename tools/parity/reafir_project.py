# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Generated one-track project with a ReaFir instance in an exact saved state (RPP chunk), for fixtures that carry "reafir_state".
# ReaFir's mode (EQ / gate / compressor / subtract) and curve are not automatable parameters, they live only in the state blob
# (docs/parity/spec-reafir.md). The blob is the state of a fresh instance with 32-bit fields replaced: {"i48": 1, "f36": -30.0}
# is "integer at byte 48 = 1", "float at byte 36 = -30.0". REAPER renders the project in a new tab (reaper_driver.run_project).
import base64
import os
import struct

HDR = '<VST "VST: ReaFir (FFT EQ+Dynamics Processor) (Cockos)" reafir.vst.dylib 0 "" 1919247986<56535472656672726561666972666674> ""'
L1 = "cmZlcu5e7f4CAAAAAQAAAAAAAAACAAAAAAAAAAIAAAABAAAAAAAAAAIAAAAAAAAAWAAAAAEAAAAAABAA"
L3 = "AFByb2dyYW0gMQAQAAAA"
DEFAULT = ("nBIAAAAAtMIAAMBBABAAAAAAgD8AAAAAAgAAAAIAAADXOb9DAAAAAKUrSEUAAAAAAAAAAAAAAACtVQM8AACAP7OiBDgAAAA+AAAAAAAAAAACAAACAACAPw==")


def state_blob(fields):
    b = bytearray(base64.b64decode(DEFAULT))
    for key, value in fields.items():
        off = int(key[1:])
        b[off:off + 4] = struct.pack("<f" if key[0] == "f" else "<i", value)
    return base64.b64encode(bytes(b)).decode()


def write_rpp(path, signal_wav, length_s, fields, sample_rate=48000):
    blob = state_blob(fields)
    lines = [blob[i:i + 128] for i in range(0, len(blob), 128)]
    rpp = "\n".join(['<REAPER_PROJECT 0.1 "7.81/macOS-arm64" 0', "  SAMPLERATE %d 1 0" % sample_rate, "  <TRACK", "    NAME fir", "    NCHAN 2",
                     "    <FXCHAIN", "      SHOW 0", "      LASTSEL 0", "      DOCKED 0", "      BYPASS 0 0 0", "      " + HDR, "        " + L1]
                    + ["        " + b for b in lines]
                    + ["        " + L3, "      >", "      FLOATPOS 0 0 0 0", "      WAK 0 0", "    >", "    <ITEM", "      POSITION 0",
                       "      LENGTH %.9f" % length_s, "      <SOURCE WAVE", '        FILE "%s"' % signal_wav, "      >", "    >", "  >", ">", ""])
    with open(path, "w") as f:
        f.write(rpp)
    return path
