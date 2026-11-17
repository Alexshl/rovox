// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Estonian strings of the project folders. Spread into the Estonian catalog (et.ts).
import type { ukFolders } from './uk.folders';

export const etFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ Uus kaust',
  'folders.tree': 'Projektid ja kaustad',
  'folders.path': 'Asukoht',
  'folders.all': 'Kõik projektid',
  'folders.tests': 'Testlood',
  'folders.more': 'Toimingud: {name}',
  'folders.emptyFolder': 'Tühi kaust',
  'folders.projects.one': '{n} projekt',
  'folders.projects.few': '{n} projekti',
  'folders.projects.many': '{n} projekti',
  'folders.projects.other': '{n} projekti',
  'folders.folders.one': '{n} kaust',
  'folders.folders.few': '{n} kausta',
  'folders.folders.many': '{n} kausta',
  'folders.folders.other': '{n} kausta',
  'folders.menu.open': 'Ava',
  'folders.menu.move': 'Teisalda kausta…',
  'folders.menu.newSub': 'Uus alamkaust',
  'folders.menu.rename': 'Nimeta ümber',
  'folders.menu.moveFolder': 'Teisalda…',
  'folders.menu.delete': 'Kustuta',
  'folders.menu.notEmpty': 'pole tühi',
  'folders.menu.expand': 'Laienda',
  'folders.menu.collapse': 'Ahenda',
  'folders.newTitle': 'Uus kaust',
  'folders.newSubTitle': 'Uus kaust kaustas „{name}“',
  'folders.nameLabel': 'Kausta nimi',
  'folders.create': 'Loo',
  'folders.renameTitle': 'Nimeta ümber „{name}“',
  'folders.moveTitle': 'Teisalda „{name}“',
  'folders.moveTargets': 'Sihtkoht',
  'folders.moveTop': 'Ülemine tase',
  'folders.moveHere': 'praegu siin',
  'folders.moveOk': 'Teisalda',
  'folders.moved': '„{name}“ teisaldati: {folder}',
  'menu.file.moveToFolder': 'Teisalda projekt kausta…',
};
