// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Lithuanian strings of the project folders. Spread into the Lithuanian catalog (lt.ts).
import type { ukFolders } from './uk.folders';

export const ltFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ Naujas aplankas',
  'folders.tree': 'Projektai ir aplankai',
  'folders.path': 'Vieta',
  'folders.all': 'Visi projektai',
  'folders.tests': 'Bandomieji takeliai',
  'folders.more': 'Veiksmai: {name}',
  'folders.emptyFolder': 'Tuščias aplankas',
  'folders.projects.one': '{n} projektas',
  'folders.projects.few': '{n} projektai',
  'folders.projects.many': '{n} projekto',
  'folders.projects.other': '{n} projektų',
  'folders.folders.one': '{n} aplankas',
  'folders.folders.few': '{n} aplankai',
  'folders.folders.many': '{n} aplanko',
  'folders.folders.other': '{n} aplankų',
  'folders.menu.open': 'Atidaryti',
  'folders.menu.move': 'Perkelti į aplanką…',
  'folders.menu.newSub': 'Naujas poaplankis',
  'folders.menu.rename': 'Pervadinti',
  'folders.menu.moveFolder': 'Perkelti į…',
  'folders.menu.delete': 'Ištrinti',
  'folders.menu.notEmpty': 'ne tuščias',
  'folders.menu.expand': 'Išskleisti',
  'folders.menu.collapse': 'Suskleisti',
  'folders.newTitle': 'Naujas aplankas',
  'folders.newSubTitle': 'Naujas aplankas aplanke „{name}“',
  'folders.nameLabel': 'Aplanko pavadinimas',
  'folders.create': 'Sukurti',
  'folders.renameTitle': 'Pervadinti „{name}“',
  'folders.moveTitle': 'Perkelti „{name}“',
  'folders.moveTargets': 'Paskirtis',
  'folders.moveTop': 'Viršutinis lygis',
  'folders.moveHere': 'dabar čia',
  'folders.moveOk': 'Perkelti',
  'folders.moved': '„{name}“ perkeltas: {folder}',
  'menu.file.moveToFolder': 'Perkelti projektą į aplanką…',
};
