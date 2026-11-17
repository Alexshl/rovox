// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Swedish strings of the project folders. Spread into the Swedish catalog (sv.ts).
import type { ukFolders } from './uk.folders';

export const svFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ Ny mapp',
  'folders.tree': 'Projekt och mappar',
  'folders.path': 'Plats',
  'folders.all': 'Alla projekt',
  'folders.tests': 'Testspår',
  'folders.more': 'Åtgärder: {name}',
  'folders.emptyFolder': 'Tom mapp',
  'folders.projects.one': '{n} projekt',
  'folders.projects.few': '{n} projekt',
  'folders.projects.many': '{n} projekt',
  'folders.projects.other': '{n} projekt',
  'folders.folders.one': '{n} mapp',
  'folders.folders.few': '{n} mappar',
  'folders.folders.many': '{n} mappar',
  'folders.folders.other': '{n} mappar',
  'folders.menu.open': 'Öppna',
  'folders.menu.move': 'Flytta till mapp…',
  'folders.menu.newSub': 'Ny undermapp',
  'folders.menu.rename': 'Byt namn',
  'folders.menu.moveFolder': 'Flytta till…',
  'folders.menu.delete': 'Ta bort',
  'folders.menu.notEmpty': 'inte tom',
  'folders.menu.expand': 'Expandera',
  'folders.menu.collapse': 'Fäll ihop',
  'folders.newTitle': 'Ny mapp',
  'folders.newSubTitle': 'Ny mapp i ”{name}”',
  'folders.nameLabel': 'Mappnamn',
  'folders.create': 'Skapa',
  'folders.renameTitle': 'Byt namn på ”{name}”',
  'folders.moveTitle': 'Flytta ”{name}”',
  'folders.moveTargets': 'Mål',
  'folders.moveTop': 'Översta nivån',
  'folders.moveHere': 'här nu',
  'folders.moveOk': 'Flytta',
  'folders.moved': '”{name}” flyttat till {folder}',
  'menu.file.moveToFolder': 'Flytta projekt till mapp…',
};
