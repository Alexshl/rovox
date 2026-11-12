// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// What a drop or a file input hands over, flattened for the uploader: {file, relpath} per file (folders walked with webkitGetAsEntry),
// filtered by extension, with the number of skipped files.

export interface DropItem {
  file: File;
  /** Path inside the dropped folder ("ZOOM0001.zprj/REC01.WAV"); the bare file name for a loose file. Never starts with "/". */
  relpath: string;
}

export interface Flattened {
  items: DropItem[];
  /** Files filtered out because they are not audio / REAPER / R20 project files. */
  skipped: number;
  /** Top-level folders in the selection. */
  folders: number;
  /** Names of files that were in the selection twice (same name, size and date): kept once. */
  duplicates: string[];
  /** Name of the first top-level folder, else of the first file ('' for nothing). */
  name: string;
}

const AUDIO_EXT = ['wav', 'aif', 'aiff', 'flac', 'mp3'];
/** Audio plus what an import needs besides audio: a REAPER project and the Zoom R20 project data file. */
const ACCEPT_EXT = [...AUDIO_EXT, 'rpp', 'r20prj'];

/** The value of `accept` for the file input. */
export const FILE_ACCEPT = '.wav,.aif,.aiff,.flac,.mp3,.rpp';

export function extOf(name: string): string {
  const i = name.lastIndexOf('.');
  return i < 0 ? '' : name.slice(i + 1).toLowerCase();
}

export function isImportable(name: string): boolean {
  return ACCEPT_EXT.includes(extOf(name));
}

export function isRpp(name: string): boolean {
  return extOf(name) === 'rpp';
}

/** iPad / iPhone Safari cannot pick a folder: the folder button is hidden there. */
export function folderPickerSupported(nav: Pick<Navigator, 'userAgent' | 'platform' | 'maxTouchPoints'> = navigator): boolean {
  const ios = /iPad|iPhone|iPod/.test(nav.userAgent) || (nav.platform === 'MacIntel' && nav.maxTouchPoints > 1);
  return !ios;
}

function cleanRel(rel: string): string {
  return rel
    .split('/')
    .filter((s) => s !== '' && s !== '.' && s !== '..')
    .join('/');
}

function build(raw: DropItem[], folderNames: Set<string>): Flattened {
  const items: DropItem[] = [];
  const seen = new Set<string>();
  const duplicates: string[] = [];
  let skipped = 0;
  for (const it of raw) {
    if (!isImportable(it.file.name)) {
      skipped++;
      continue;
    }
    const rel = cleanRel(it.relpath) || it.file.name;
    const fp = `${rel}|${it.file.size}|${it.file.lastModified}`;
    if (seen.has(fp)) {
      duplicates.push(it.file.name);
      continue;
    }
    seen.add(fp);
    items.push({ file: it.file, relpath: rel });
  }
  const first = [...folderNames][0] ?? items[0]?.file.name ?? raw[0]?.file.name ?? '';
  return { items, skipped, folders: folderNames.size, duplicates, name: first };
}

/** Files from `<input type=file>` (with `webkitdirectory` every file carries webkitRelativePath). */
export function flattenFiles(files: ArrayLike<File>): Flattened {
  const raw: DropItem[] = [];
  const folders = new Set<string>();
  for (let i = 0; i < files.length; i++) {
    const f = files[i]!;
    const rel = (f as File & { webkitRelativePath?: string }).webkitRelativePath || f.name;
    if (rel.includes('/')) folders.add(rel.split('/')[0]!);
    raw.push({ file: f, relpath: rel });
  }
  return build(raw, folders);
}

function entryFile(e: FileSystemFileEntry): Promise<File> {
  return new Promise((resolve, reject) => e.file(resolve, reject));
}

/** readEntries returns at most ~100 entries per call: read until it answers with an empty list. */
async function readAll(reader: FileSystemDirectoryReader): Promise<FileSystemEntry[]> {
  const out: FileSystemEntry[] = [];
  for (;;) {
    const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => reader.readEntries(resolve, reject));
    if (!batch.length) return out;
    out.push(...batch);
  }
}

async function walk(entry: FileSystemEntry, out: DropItem[]): Promise<void> {
  if (entry.isFile) {
    const file = await entryFile(entry as FileSystemFileEntry);
    out.push({ file, relpath: entry.fullPath.replace(/^\/+/, '') || file.name });
  } else if (entry.isDirectory) {
    for (const child of await readAll((entry as FileSystemDirectoryEntry).createReader())) await walk(child, out);
  }
}

/** Entries of a drop (folders walked recursively). Loose files get their bare name as relpath, files of a folder "folder/…". */
export async function flattenEntries(entries: FileSystemEntry[]): Promise<Flattened> {
  const raw: DropItem[] = [];
  const folders = new Set<string>();
  for (const e of entries) {
    if (e.isDirectory) folders.add(e.name);
    await walk(e, raw);
  }
  return build(raw, folders);
}

/** Everything a drop carries. Entries are read first (the DataTransfer is only valid during the event); without webkitGetAsEntry the plain files are used. */
export async function collectDrop(dt: DataTransfer): Promise<Flattened> {
  const entries: FileSystemEntry[] = [];
  const plain: File[] = [];
  const items = dt.items ? [...dt.items] : [];
  for (const it of items) {
    if (it.kind !== 'file') continue;
    const entry = typeof it.webkitGetAsEntry === 'function' ? it.webkitGetAsEntry() : null;
    if (entry) entries.push(entry);
    else {
      const f = it.getAsFile();
      if (f) plain.push(f);
    }
  }
  if (!items.length) plain.push(...(dt.files ? [...dt.files] : []));
  if (!entries.length) return flattenFiles(plain);
  const flat = await flattenEntries(entries);
  if (!plain.length) return flat;
  const extra = flattenFiles(plain);
  return { items: [...flat.items, ...extra.items], skipped: flat.skipped + extra.skipped, folders: flat.folders, duplicates: [...flat.duplicates, ...extra.duplicates], name: flat.name || extra.name };
}

/** For the drag-over card: how many files and folders the drag carries (folders are known only where the browser exposes entries during dragover). */
export function dragSummary(dt: DataTransfer): { files: number; folders: number } {
  let files = 0;
  let folders = 0;
  for (const it of dt.items ? [...dt.items] : []) {
    if (it.kind !== 'file') continue;
    const entry = typeof it.webkitGetAsEntry === 'function' ? it.webkitGetAsEntry() : null;
    if (entry?.isDirectory) folders++;
    else files++;
  }
  return { files, folders };
}

/** True when the drag carries files (not text or a link). */
export function hasFiles(dt: DataTransfer | null): boolean {
  return !!dt && [...(dt.types ?? [])].includes('Files');
}
