// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Latvian strings of the narrow-screen layout. Spread into the Latvian catalog (lv.ts).
import type { ukNarrow } from './uk.narrow';

export const lvNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Izvēlne',
  'narrow.agents': 'Aģenti',
  'narrow.agentsActive': 'Aģenti: ir aktīvs aģents',
  'narrow.agentsClose': 'Aizvērt aģentu paneli',
  'narrow.more': 'Vairāk',
  'narrow.moreTitle': 'Papildu darbības',
  'narrow.sheetClose': 'Aizvērt',
  'narrow.mixerHandle': 'Mikseris (velciet uz augšu)',
  'narrow.trackFx': 'Celiņa {name} efekti',
  'narrow.agentChanged.one': 'Aģents mainīja {n} celiņu',
  'narrow.agentChanged.few': 'Aģents mainīja {n} celiņus',
  'narrow.agentChanged.many': 'Aģents mainīja {n} celiņus',
  'narrow.agentChanged.other': 'Aģents mainīja {n} celiņus',
  'narrow.agentUndo': 'Atsaukt',
  'narrow.agentAccept': 'Pieņemt',
};
