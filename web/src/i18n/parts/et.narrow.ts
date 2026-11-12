// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Estonian strings of the narrow-screen layout. Spread into the Estonian catalog (et.ts).
import type { ukNarrow } from './uk.narrow';

export const etNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Menüü',
  'narrow.agents': 'Agendid',
  'narrow.agentsActive': 'Agendid: üks agent on aktiivne',
  'narrow.agentsClose': 'Sulge agentide paneel',
  'narrow.more': 'Veel',
  'narrow.moreTitle': 'Lisatoimingud',
  'narrow.sheetClose': 'Sulge',
  'narrow.mixerHandle': 'Mikser (lohista ülespoole)',
  'narrow.trackFx': 'Raja {name} efektid',
  'narrow.agentChanged.one': 'Agent muutis {n} rada',
  'narrow.agentChanged.few': 'Agent muutis {n} rada',
  'narrow.agentChanged.many': 'Agent muutis {n} rada',
  'narrow.agentChanged.other': 'Agent muutis {n} rada',
  'narrow.agentUndo': 'Võta tagasi',
  'narrow.agentAccept': 'Nõustu',
};
