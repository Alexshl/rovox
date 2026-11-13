// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Swedish wording of the activity log summary. Spread into the Swedish catalog (sv.ts).
import type { ukLog } from './uk.log';

export const svLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'ändrat: {what}',
  'log.kind.track.one': '{n} spår',
  'log.kind.track.few': '{n} spår',
  'log.kind.track.many': '{n} spår',
  'log.kind.track.other': '{n} spår',
  'log.kind.item.one': '{n} klipp',
  'log.kind.item.few': '{n} klipp',
  'log.kind.item.many': '{n} klipp',
  'log.kind.item.other': '{n} klipp',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} markör',
  'log.kind.marker.few': '{n} markörer',
  'log.kind.marker.many': '{n} markörer',
  'log.kind.marker.other': '{n} markörer',
  'log.kind.tempo.one': '{n} tempopunkt',
  'log.kind.tempo.few': '{n} tempopunkter',
  'log.kind.tempo.many': '{n} tempopunkter',
  'log.kind.tempo.other': '{n} tempopunkter',
  'log.kind.other.one': '{n} objekt',
  'log.kind.other.few': '{n} objekt',
  'log.kind.other.many': '{n} objekt',
  'log.kind.other.other': '{n} objekt',
};
