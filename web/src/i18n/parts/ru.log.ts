// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Russian wording of the activity log summary. Spread into the Russian catalog (ru.ts).
import type { ukLog } from './uk.log';

export const ruLog: Record<keyof typeof ukLog, string> = {
  'log.op.add': '+{what}',
  'log.op.remove': '−{what}',
  'log.op.update': 'изменено: {what}',
  'log.kind.track.one': '{n} дорожка',
  'log.kind.track.few': '{n} дорожки',
  'log.kind.track.many': '{n} дорожек',
  'log.kind.track.other': '{n} дорожки',
  'log.kind.item.one': '{n} клип',
  'log.kind.item.few': '{n} клипа',
  'log.kind.item.many': '{n} клипов',
  'log.kind.item.other': '{n} клипа',
  'log.kind.plugin.one': '{n} FX',
  'log.kind.plugin.few': '{n} FX',
  'log.kind.plugin.many': '{n} FX',
  'log.kind.plugin.other': '{n} FX',
  'log.kind.marker.one': '{n} маркер',
  'log.kind.marker.few': '{n} маркера',
  'log.kind.marker.many': '{n} маркеров',
  'log.kind.marker.other': '{n} маркера',
  'log.kind.tempo.one': '{n} точка темпа',
  'log.kind.tempo.few': '{n} точки темпа',
  'log.kind.tempo.many': '{n} точек темпа',
  'log.kind.tempo.other': '{n} точки темпа',
  'log.kind.other.one': '{n} объект',
  'log.kind.other.few': '{n} объекта',
  'log.kind.other.many': '{n} объектов',
  'log.kind.other.other': '{n} объекта',
};
