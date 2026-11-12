// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Lithuanian strings of the narrow-screen layout. Spread into the Lithuanian catalog (lt.ts).
import type { ukNarrow } from './uk.narrow';

export const ltNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Meniu',
  'narrow.agents': 'Agentai',
  'narrow.agentsActive': 'Agentai: yra aktyvus agentas',
  'narrow.agentsClose': 'Uždaryti agentų skydelį',
  'narrow.more': 'Daugiau',
  'narrow.moreTitle': 'Papildomi veiksmai',
  'narrow.sheetClose': 'Uždaryti',
  'narrow.mixerHandle': 'Mikšeris (patraukite aukštyn)',
  'narrow.trackFx': 'Takelio {name} efektai',
  'narrow.agentChanged.one': 'Agentas pakeitė {n} takelį',
  'narrow.agentChanged.few': 'Agentas pakeitė {n} takelius',
  'narrow.agentChanged.many': 'Agentas pakeitė {n} takelio',
  'narrow.agentChanged.other': 'Agentas pakeitė {n} takelių',
  'narrow.agentUndo': 'Anuliuoti',
  'narrow.agentAccept': 'Priimti',
};
