// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// English wording of the activity log summary. Spread into the English catalog (en.ts).
import type { ukLog } from './uk.log';

export const enLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'changed: {what}',
  'log.kind.track.one': '{n} track',
  'log.kind.track.few': '{n} tracks',
  'log.kind.track.many': '{n} tracks',
  'log.kind.track.other': '{n} tracks',
  'log.kind.item.one': '{n} clip',
  'log.kind.item.few': '{n} clips',
  'log.kind.item.many': '{n} clips',
  'log.kind.item.other': '{n} clips',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} marker',
  'log.kind.marker.few': '{n} markers',
  'log.kind.marker.many': '{n} markers',
  'log.kind.marker.other': '{n} markers',
  'log.kind.tempo.one': '{n} tempo point',
  'log.kind.tempo.few': '{n} tempo points',
  'log.kind.tempo.many': '{n} tempo points',
  'log.kind.tempo.other': '{n} tempo points',
  'log.kind.other.one': '{n} object',
  'log.kind.other.few': '{n} objects',
  'log.kind.other.many': '{n} objects',
  'log.kind.other.other': '{n} objects',
};
