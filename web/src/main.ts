// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import '@fontsource/barlow-condensed/latin-500.css';
import '@fontsource/barlow-condensed/latin-600.css';
import '@fontsource/ibm-plex-mono/latin-400.css';
import '@fontsource/ibm-plex-mono/latin-500.css';
import '@fontsource/ibm-plex-mono/cyrillic-400.css';
import '@fontsource/ibm-plex-sans/latin-400.css';
import '@fontsource/ibm-plex-sans/latin-500.css';
import '@fontsource/ibm-plex-sans/latin-600.css';
import '@fontsource/ibm-plex-sans/cyrillic-400.css';
import '@fontsource/ibm-plex-sans/cyrillic-500.css';
import '@fontsource/ibm-plex-sans/cyrillic-600.css';
import './styles/tokens.css';
import './styles/base.css';

import { clear } from './lib/dom';
import { App } from './store/session';
import { Shell, loginView } from './views/shell';

const root = document.getElementById('root')!;
const app = new App();
let shell: Shell | undefined;

function mount(): void {
  clear(root);
  if (app.needsLogin) {
    root.append(loginView(app));
    return;
  }
  shell ??= new Shell(app);
  root.append(shell.el);
}

let wasLogin = false;
app.changed.on(() => {
  if (app.needsLogin !== wasLogin) {
    wasLogin = app.needsLogin;
    mount();
  }
});

mount();
void app.start();

// debugging and end-to-end checks read the live state from here
(window as unknown as { __rovox: unknown }).__rovox = {
  app,
  get audio() {
    return app.audio.stats;
  },
  get fps() {
    return shell?.arrangeView.fps ?? 0;
  },
  get visibleItems() {
    return shell?.arrangeView.visibleItemCount() ?? 0;
  },
};
