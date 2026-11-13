// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// German wording of the activity log summary. Spread into the German catalog (de.ts).
import type { ukLog } from './uk.log';

export const deLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'geändert: {what}',
  'log.kind.track.one': '{n} Spur',
  'log.kind.track.few': '{n} Spuren',
  'log.kind.track.many': '{n} Spuren',
  'log.kind.track.other': '{n} Spuren',
  'log.kind.item.one': '{n} Clip',
  'log.kind.item.few': '{n} Clips',
  'log.kind.item.many': '{n} Clips',
  'log.kind.item.other': '{n} Clips',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} Marker',
  'log.kind.marker.few': '{n} Marker',
  'log.kind.marker.many': '{n} Marker',
  'log.kind.marker.other': '{n} Marker',
  'log.kind.tempo.one': '{n} Tempopunkt',
  'log.kind.tempo.few': '{n} Tempopunkte',
  'log.kind.tempo.many': '{n} Tempopunkte',
  'log.kind.tempo.other': '{n} Tempopunkte',
  'log.kind.other.one': '{n} Objekt',
  'log.kind.other.few': '{n} Objekte',
  'log.kind.other.many': '{n} Objekte',
  'log.kind.other.other': '{n} Objekte',
};
