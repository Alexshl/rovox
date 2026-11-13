// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Latvian wording of the activity log summary. Spread into the Latvian catalog (lv.ts).
import type { ukLog } from './uk.log';

export const lvLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'mainīts: {what}',
  'log.kind.track.one': '{n} celiņš',
  'log.kind.track.few': '{n} celiņi',
  'log.kind.track.many': '{n} celiņi',
  'log.kind.track.other': '{n} celiņi',
  'log.kind.item.one': '{n} klips',
  'log.kind.item.few': '{n} klipi',
  'log.kind.item.many': '{n} klipi',
  'log.kind.item.other': '{n} klipi',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} marķieris',
  'log.kind.marker.few': '{n} marķieri',
  'log.kind.marker.many': '{n} marķieri',
  'log.kind.marker.other': '{n} marķieri',
  'log.kind.tempo.one': '{n} tempa punkts',
  'log.kind.tempo.few': '{n} tempa punkti',
  'log.kind.tempo.many': '{n} tempa punkti',
  'log.kind.tempo.other': '{n} tempa punkti',
  'log.kind.other.one': '{n} objekts',
  'log.kind.other.few': '{n} objekti',
  'log.kind.other.many': '{n} objekti',
  'log.kind.other.other': '{n} objekti',
};
