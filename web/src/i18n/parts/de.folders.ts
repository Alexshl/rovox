// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// German strings of the project folders. Spread into the German catalog (de.ts).
import type { ukFolders } from './uk.folders';

export const deFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ Neuer Ordner',
  'folders.tree': 'Projekte und Ordner',
  'folders.path': 'Ort',
  'folders.all': 'Alle Projekte',
  'folders.tests': 'Testspuren',
  'folders.more': 'Aktionen: {name}',
  'folders.emptyFolder': 'Leerer Ordner',
  'folders.projects.one': '{n} Projekt',
  'folders.projects.few': '{n} Projekte',
  'folders.projects.many': '{n} Projekte',
  'folders.projects.other': '{n} Projekte',
  'folders.folders.one': '{n} Ordner',
  'folders.folders.few': '{n} Ordner',
  'folders.folders.many': '{n} Ordner',
  'folders.folders.other': '{n} Ordner',
  'folders.menu.open': 'Öffnen',
  'folders.menu.move': 'In Ordner verschieben…',
  'folders.menu.newSub': 'Neuer Unterordner',
  'folders.menu.rename': 'Umbenennen',
  'folders.menu.moveFolder': 'Verschieben nach…',
  'folders.menu.delete': 'Löschen',
  'folders.menu.notEmpty': 'nicht leer',
  'folders.menu.expand': 'Aufklappen',
  'folders.menu.collapse': 'Zuklappen',
  'folders.newTitle': 'Neuer Ordner',
  'folders.newSubTitle': 'Neuer Ordner in „{name}“',
  'folders.nameLabel': 'Ordnername',
  'folders.create': 'Erstellen',
  'folders.renameTitle': '„{name}“ umbenennen',
  'folders.moveTitle': '„{name}“ verschieben',
  'folders.moveTargets': 'Ziel',
  'folders.moveTop': 'Oberste Ebene',
  'folders.moveHere': 'aktuell hier',
  'folders.moveOk': 'Verschieben',
  'folders.moved': '„{name}“ verschoben nach {folder}',
  'menu.file.moveToFolder': 'Projekt in Ordner verschieben…',
};
