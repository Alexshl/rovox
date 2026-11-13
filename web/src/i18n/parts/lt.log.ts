// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Lithuanian wording of the activity log summary. Spread into the Lithuanian catalog (lt.ts).
import type { ukLog } from './uk.log';

export const ltLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'pakeista: {what}',
  'log.kind.track.one': '{n} takelis',
  'log.kind.track.few': '{n} takeliai',
  'log.kind.track.many': '{n} takelio',
  'log.kind.track.other': '{n} takelių',
  'log.kind.item.one': '{n} klipas',
  'log.kind.item.few': '{n} klipai',
  'log.kind.item.many': '{n} klipo',
  'log.kind.item.other': '{n} klipų',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} žymeklis',
  'log.kind.marker.few': '{n} žymekliai',
  'log.kind.marker.many': '{n} žymeklio',
  'log.kind.marker.other': '{n} žymeklių',
  'log.kind.tempo.one': '{n} tempo taškas',
  'log.kind.tempo.few': '{n} tempo taškai',
  'log.kind.tempo.many': '{n} tempo taško',
  'log.kind.tempo.other': '{n} tempo taškų',
  'log.kind.other.one': '{n} objektas',
  'log.kind.other.few': '{n} objektai',
  'log.kind.other.many': '{n} objekto',
  'log.kind.other.other': '{n} objektų',
};
