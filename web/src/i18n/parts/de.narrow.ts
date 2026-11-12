// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// German strings of the narrow-screen layout. Spread into the German catalog (de.ts).
import type { ukNarrow } from './uk.narrow';

export const deNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Menü',
  'narrow.agents': 'Agenten',
  'narrow.agentsActive': 'Agenten: ein Agent ist aktiv',
  'narrow.agentsClose': 'Agentenbereich schließen',
  'narrow.more': 'Mehr',
  'narrow.moreTitle': 'Weitere Aktionen',
  'narrow.sheetClose': 'Schließen',
  'narrow.mixerHandle': 'Mixer (nach oben ziehen)',
  'narrow.trackFx': 'Effekte der Spur {name}',
  'narrow.agentChanged.one': 'Agent hat {n} Spur geändert',
  'narrow.agentChanged.few': 'Agent hat {n} Spuren geändert',
  'narrow.agentChanged.many': 'Agent hat {n} Spuren geändert',
  'narrow.agentChanged.other': 'Agent hat {n} Spuren geändert',
  'narrow.agentUndo': 'Rückgängig',
  'narrow.agentAccept': 'Übernehmen',
};
