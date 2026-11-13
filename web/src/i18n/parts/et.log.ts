// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Estonian wording of the activity log summary. Spread into the Estonian catalog (et.ts).
import type { ukLog } from './uk.log';

export const etLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'muudetud: {what}',
  'log.kind.track.one': '{n} rada',
  'log.kind.track.few': '{n} rada',
  'log.kind.track.many': '{n} rada',
  'log.kind.track.other': '{n} rada',
  'log.kind.item.one': '{n} klipp',
  'log.kind.item.few': '{n} klippi',
  'log.kind.item.many': '{n} klippi',
  'log.kind.item.other': '{n} klippi',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} marker',
  'log.kind.marker.few': '{n} markerit',
  'log.kind.marker.many': '{n} markerit',
  'log.kind.marker.other': '{n} markerit',
  'log.kind.tempo.one': '{n} tempopunkt',
  'log.kind.tempo.few': '{n} tempopunkti',
  'log.kind.tempo.many': '{n} tempopunkti',
  'log.kind.tempo.other': '{n} tempopunkti',
  'log.kind.other.one': '{n} objekt',
  'log.kind.other.few': '{n} objekti',
  'log.kind.other.many': '{n} objekti',
  'log.kind.other.other': '{n} objekti',
};
