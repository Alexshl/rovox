// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// English strings of the narrow-screen layout. Spread into the English catalog (en.ts).
import type { ukNarrow } from './uk.narrow';

export const enNarrow: Record<keyof typeof ukNarrow, string> = {
  'narrow.menu': 'Menu',
  'narrow.agents': 'Agents',
  'narrow.agentsActive': 'Agents: an agent is active',
  'narrow.agentsClose': 'Close the agents panel',
  'narrow.more': 'More',
  'narrow.moreTitle': 'More actions',
  'narrow.sheetClose': 'Close',
  'narrow.mixerHandle': 'Mixer (drag up)',
  'narrow.trackFx': 'Effects of track {name}',
  'narrow.agentChanged.one': 'Agent changed {n} track',
  'narrow.agentChanged.few': 'Agent changed {n} tracks',
  'narrow.agentChanged.many': 'Agent changed {n} tracks',
  'narrow.agentChanged.other': 'Agent changed {n} tracks',
  'narrow.agentUndo': 'Undo',
  'narrow.agentAccept': 'Accept',
};
