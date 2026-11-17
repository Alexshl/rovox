// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Russian strings of the project folders. Spread into the Russian catalog (ru.ts).
import type { ukFolders } from './uk.folders';

export const ruFolders: Record<keyof typeof ukFolders, string> = {
  'folders.new': '＋ Новая папка',
  'folders.tree': 'Проекты и папки',
  'folders.path': 'Расположение',
  'folders.all': 'Все проекты',
  'folders.tests': 'Тестовые треки',
  'folders.more': 'Действия: {name}',
  'folders.emptyFolder': 'Пустая папка',
  'folders.projects.one': '{n} проект',
  'folders.projects.few': '{n} проекта',
  'folders.projects.many': '{n} проектов',
  'folders.projects.other': '{n} проекта',
  'folders.folders.one': '{n} папка',
  'folders.folders.few': '{n} папки',
  'folders.folders.many': '{n} папок',
  'folders.folders.other': '{n} папки',
  'folders.menu.open': 'Открыть',
  'folders.menu.move': 'Переместить в папку…',
  'folders.menu.newSub': 'Новая подпапка',
  'folders.menu.rename': 'Переименовать',
  'folders.menu.moveFolder': 'Переместить в…',
  'folders.menu.delete': 'Удалить',
  'folders.menu.notEmpty': 'не пустая',
  'folders.menu.expand': 'Развернуть',
  'folders.menu.collapse': 'Свернуть',
  'folders.newTitle': 'Новая папка',
  'folders.newSubTitle': 'Новая папка в «{name}»',
  'folders.nameLabel': 'Название папки',
  'folders.create': 'Создать',
  'folders.renameTitle': 'Переименовать «{name}»',
  'folders.moveTitle': 'Переместить «{name}»',
  'folders.moveTargets': 'Куда переместить',
  'folders.moveTop': 'Верхний уровень',
  'folders.moveHere': 'сейчас здесь',
  'folders.moveOk': 'Переместить',
  'folders.moved': '«{name}» перемещено: {folder}',
  'menu.file.moveToFolder': 'Переместить проект в папку…',
};
