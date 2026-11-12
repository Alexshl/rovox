// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Russian strings of the narrow-screen layout. Spread into the Russian catalog (ru.ts).
import type { ukNarrow } from './uk.narrow';

export const ruNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Меню',
  'narrow.agents': 'Агенты',
  'narrow.agentsActive': 'Агенты: есть активный агент',
  'narrow.agentsClose': 'Закрыть панель агентов',
  'narrow.more': 'Ещё',
  'narrow.moreTitle': 'Дополнительные действия',
  'narrow.sheetClose': 'Закрыть',
  'narrow.mixerHandle': 'Микшер (потяните вверх)',
  'narrow.trackFx': 'Эффекты дорожки {name}',
  'narrow.agentChanged.one': 'Агент изменил {n} дорожку',
  'narrow.agentChanged.few': 'Агент изменил {n} дорожки',
  'narrow.agentChanged.many': 'Агент изменил {n} дорожек',
  'narrow.agentChanged.other': 'Агент изменил {n} дорожки',
  'narrow.agentUndo': 'Отменить',
  'narrow.agentAccept': 'Принять',
};
