// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Ukrainian (source) wording of the activity log summary: what a change did, counted per kind. Spread into uk.ts.
export const ukLog = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'змінено: {what}',
  'log.kind.track.one': '{n} доріжка',
  'log.kind.track.few': '{n} доріжки',
  'log.kind.track.many': '{n} доріжок',
  'log.kind.track.other': '{n} доріжки',
  'log.kind.item.one': '{n} кліп',
  'log.kind.item.few': '{n} кліпи',
  'log.kind.item.many': '{n} кліпів',
  'log.kind.item.other': '{n} кліпа',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} маркер',
  'log.kind.marker.few': '{n} маркери',
  'log.kind.marker.many': '{n} маркерів',
  'log.kind.marker.other': '{n} маркера',
  'log.kind.tempo.one': '{n} точка темпу',
  'log.kind.tempo.few': '{n} точки темпу',
  'log.kind.tempo.many': '{n} точок темпу',
  'log.kind.tempo.other': '{n} точки темпу',
  'log.kind.other.one': '{n} об’єкт',
  'log.kind.other.few': '{n} об’єкти',
  'log.kind.other.many': '{n} об’єктів',
  'log.kind.other.other': '{n} об’єкта',
} as const;
