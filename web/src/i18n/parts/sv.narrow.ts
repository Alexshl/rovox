// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Swedish strings of the narrow-screen layout. Spread into the Swedish catalog (sv.ts).
import type { ukNarrow } from './uk.narrow';

export const svNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Meny',
  'narrow.agents': 'Agenter',
  'narrow.agentsActive': 'Agenter: en agent är aktiv',
  'narrow.agentsClose': 'Stäng agentpanelen',
  'narrow.more': 'Mer',
  'narrow.moreTitle': 'Fler åtgärder',
  'narrow.sheetClose': 'Stäng',
  'narrow.mixerHandle': 'Mixer (dra uppåt)',
  'narrow.trackFx': 'Effekter för spåret {name}',
  'narrow.agentChanged.one': 'Agenten ändrade {n} spår',
  'narrow.agentChanged.few': 'Agenten ändrade {n} spår',
  'narrow.agentChanged.many': 'Agenten ändrade {n} spår',
  'narrow.agentChanged.other': 'Agenten ändrade {n} spår',
  'narrow.agentUndo': 'Ångra',
  'narrow.agentAccept': 'Godkänn',
};
