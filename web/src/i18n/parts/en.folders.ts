// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// English strings of the project folders. Spread into the English catalog (en.ts).
import type { ukFolders } from './uk.folders';

export const enFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ New folder',
  'folders.tree': 'Projects and folders',
  'folders.path': 'Location',
  'folders.all': 'All projects',
  'folders.tests': 'Test tracks',
  'folders.more': 'Actions: {name}',
  'folders.emptyFolder': 'Empty folder',
  'folders.projects.one': '{n} project',
  'folders.projects.few': '{n} projects',
  'folders.projects.many': '{n} projects',
  'folders.projects.other': '{n} projects',
  'folders.folders.one': '{n} folder',
  'folders.folders.few': '{n} folders',
  'folders.folders.many': '{n} folders',
  'folders.folders.other': '{n} folders',
  'folders.menu.open': 'Open',
  'folders.menu.move': 'Move to folder…',
  'folders.menu.newSub': 'New subfolder',
  'folders.menu.rename': 'Rename',
  'folders.menu.moveFolder': 'Move to…',
  'folders.menu.delete': 'Delete',
  'folders.menu.notEmpty': 'not empty',
  'folders.menu.expand': 'Expand',
  'folders.menu.collapse': 'Collapse',
  'folders.newTitle': 'New folder',
  'folders.newSubTitle': 'New folder in “{name}”',
  'folders.nameLabel': 'Folder name',
  'folders.create': 'Create',
  'folders.renameTitle': 'Rename “{name}”',
  'folders.moveTitle': 'Move “{name}”',
  'folders.moveTargets': 'Destination',
  'folders.moveTop': 'Top level',
  'folders.moveHere': 'here now',
  'folders.moveOk': 'Move',
  'folders.moved': '“{name}” moved to {folder}',
  'menu.file.moveToFolder': 'Move Project to Folder…',
};
