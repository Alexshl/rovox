// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Latvian strings of the project folders. Spread into the Latvian catalog (lv.ts).
import type { ukFolders } from './uk.folders';

export const lvFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ Jauna mape',
  'folders.tree': 'Projekti un mapes',
  'folders.path': 'Atrašanās vieta',
  'folders.all': 'Visi projekti',
  'folders.tests': 'Testa celiņi',
  'folders.more': 'Darbības: {name}',
  'folders.emptyFolder': 'Tukša mape',
  'folders.projects.one': '{n} projekts',
  'folders.projects.few': '{n} projekti',
  'folders.projects.many': '{n} projekti',
  'folders.projects.other': '{n} projekti',
  'folders.folders.one': '{n} mape',
  'folders.folders.few': '{n} mapes',
  'folders.folders.many': '{n} mapes',
  'folders.folders.other': '{n} mapes',
  'folders.menu.open': 'Atvērt',
  'folders.menu.move': 'Pārvietot uz mapi…',
  'folders.menu.newSub': 'Jauna apakšmape',
  'folders.menu.rename': 'Pārdēvēt',
  'folders.menu.moveFolder': 'Pārvietot uz…',
  'folders.menu.delete': 'Dzēst',
  'folders.menu.notEmpty': 'nav tukša',
  'folders.menu.expand': 'Izvērst',
  'folders.menu.collapse': 'Sakļaut',
  'folders.newTitle': 'Jauna mape',
  'folders.newSubTitle': 'Jauna mape mapē “{name}”',
  'folders.nameLabel': 'Mapes nosaukums',
  'folders.create': 'Izveidot',
  'folders.renameTitle': 'Pārdēvēt “{name}”',
  'folders.moveTitle': 'Pārvietot “{name}”',
  'folders.moveTargets': 'Mērķis',
  'folders.moveTop': 'Augšējais līmenis',
  'folders.moveHere': 'pašlaik šeit',
  'folders.moveOk': 'Pārvietot',
  'folders.moved': '“{name}” pārvietots: {folder}',
  'menu.file.moveToFolder': 'Pārvietot projektu uz mapi…',
};
