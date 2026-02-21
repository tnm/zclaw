(function () {
  var THEME_STORAGE_KEY = 'zclaw_docs_theme';
  var page = document.querySelector('.page');
  var sidebar = document.querySelector('.sidebar');
  var topbar = document.querySelector('.topbar');
  var menuButton = document.querySelector('.menu-toggle');
  var links = Array.prototype.slice.call(document.querySelectorAll('.chapter-list a'));
  var current = window.location.pathname.split('/').pop() || 'index.html';
  var themeButtons = [];
  var shortcutPanel = null;
  var gPrefixActive = false;
  var gPrefixTimer = null;

  function markCurrentChapter() {
    links.forEach(function (link) {
      var href = link.getAttribute('href');
      if (href === current || (href === 'index.html' && current === '')) {
        link.setAttribute('aria-current', 'page');
      }
    });
  }

  function isMenuOpen() {
    return page && page.classList.contains('nav-open');
  }

  function setMenuOpen(open) {
    if (!page) {
      return;
    }
    if (open) {
      page.classList.add('nav-open');
      updateMenuButtonState();
      return;
    }
    page.classList.remove('nav-open');
    updateMenuButtonState();
  }

  function toggleMenu() {
    setMenuOpen(!isMenuOpen());
  }

  function isEditableTarget(target) {
    if (!target) {
      return false;
    }
    if (target.isContentEditable) {
      return true;
    }

    var tagName = target.tagName;
    return tagName === 'INPUT' || tagName === 'TEXTAREA' || tagName === 'SELECT';
  }

  function readStoredTheme() {
    try {
      return localStorage.getItem(THEME_STORAGE_KEY);
    } catch (error) {
      return null;
    }
  }

  function saveTheme(theme) {
    try {
      localStorage.setItem(THEME_STORAGE_KEY, theme);
    } catch (error) {
      // No-op when storage is unavailable.
    }
  }

  function currentTheme() {
    return document.documentElement.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
  }

  function updateThemeButtons(theme) {
    themeButtons.forEach(function (button) {
      if (theme === 'dark') {
        setButtonFace(button, '☀', 'Day');
      } else {
        setButtonFace(button, '☾', 'Night');
      }
      button.setAttribute('title', 'Toggle dark mode (D)');
      button.setAttribute('aria-label', 'Toggle dark mode');
    });
  }

  function applyTheme(theme) {
    var normalized = theme === 'dark' ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', normalized);
    updateThemeButtons(normalized);
  }

  function toggleTheme() {
    var next = currentTheme() === 'dark' ? 'light' : 'dark';
    applyTheme(next);
    saveTheme(next);
  }

  function chapterIndex() {
    for (var i = 0; i < links.length; i++) {
      if (links[i].getAttribute('aria-current') === 'page') {
        return i;
      }
    }

    for (var j = 0; j < links.length; j++) {
      if (links[j].getAttribute('href') === current) {
        return j;
      }
    }

    return 0;
  }

  function navigateToIndex(index) {
    if (index < 0 || index >= links.length) {
      return;
    }

    var href = links[index].getAttribute('href');
    if (!href || href === current) {
      return;
    }

    window.location.href = href;
  }

  function navigateRelative(delta) {
    navigateToIndex(chapterIndex() + delta);
  }

  function utilityButton(label, className, onClick) {
    var button = document.createElement('button');
    button.type = 'button';
    button.className = className ? 'utility-btn ' + className : 'utility-btn';
    button.textContent = label;
    button.addEventListener('click', onClick);
    return button;
  }

  function setButtonFace(button, icon, label) {
    button.innerHTML =
      '<span class="btn-icon" aria-hidden="true">' + icon + '</span>' +
      '<span class="btn-label">' + label + '</span>';
  }

  function updateMenuButtonState() {
    if (!menuButton) {
      return;
    }

    if (isMenuOpen()) {
      menuButton.textContent = '✕';
      menuButton.setAttribute('aria-label', 'Close chapter menu');
      return;
    }

    menuButton.textContent = '☰';
    menuButton.setAttribute('aria-label', 'Open chapter menu');
  }

  function resetGPrefix() {
    gPrefixActive = false;
    if (gPrefixTimer) {
      clearTimeout(gPrefixTimer);
      gPrefixTimer = null;
    }
  }

  function activateGPrefix() {
    resetGPrefix();
    gPrefixActive = true;
    gPrefixTimer = setTimeout(function () {
      resetGPrefix();
    }, 750);
  }

  function scrollByViewport(multiplier) {
    window.scrollBy({
      top: Math.round(window.innerHeight * multiplier),
      behavior: 'smooth'
    });
  }

  function scrollToTop() {
    window.scrollTo({ top: 0, behavior: 'smooth' });
  }

  function scrollToBottom() {
    window.scrollTo({ top: document.body.scrollHeight, behavior: 'smooth' });
  }

  function ensureShortcutPanel() {
    if (shortcutPanel) {
      return shortcutPanel;
    }

    shortcutPanel = document.createElement('div');
    shortcutPanel.className = 'shortcut-panel';
    shortcutPanel.setAttribute('aria-hidden', 'true');
    shortcutPanel.innerHTML =
      '<div class="shortcut-card" role="dialog" aria-modal="true" aria-label="Keyboard shortcuts">' +
      '  <div class="shortcut-header">' +
      '    <h2>Keyboard Shortcuts</h2>' +
      '    <button type="button" class="utility-btn shortcut-close" aria-label="Close shortcuts">Close</button>' +
      '  </div>' +
      '  <table class="shortcut-table">' +
      '    <tbody>' +
      '      <tr><th><kbd>h</kbd> / <kbd>l</kbd></th><td>Previous or next chapter</td></tr>' +
      '      <tr><th><kbd>j</kbd> / <kbd>k</kbd></th><td>Scroll down or up</td></tr>' +
      '      <tr><th><kbd>gg</kbd> / <kbd>G</kbd></th><td>Top or bottom of page</td></tr>' +
      '      <tr><th><kbd>D</kbd></th><td>Toggle dark mode</td></tr>' +
      '      <tr><th><kbd>M</kbd></th><td>Toggle sidebar menu (mobile)</td></tr>' +
      '      <tr><th><kbd>?</kbd></th><td>Open/close this panel</td></tr>' +
      '      <tr><th><kbd>Esc</kbd></th><td>Close panel and mobile menu</td></tr>' +
      '    </tbody>' +
      '  </table>' +
      '</div>';

    document.body.appendChild(shortcutPanel);

    shortcutPanel.addEventListener('click', function (event) {
      if (event.target === shortcutPanel) {
        setShortcutPanelOpen(false);
      }
    });

    var closeButton = shortcutPanel.querySelector('.shortcut-close');
    if (closeButton) {
      closeButton.addEventListener('click', function () {
        setShortcutPanelOpen(false);
      });
    }

    return shortcutPanel;
  }

  function setShortcutPanelOpen(open) {
    var panel = ensureShortcutPanel();
    panel.classList.toggle('is-open', open);
    panel.setAttribute('aria-hidden', open ? 'false' : 'true');

    if (open) {
      var closeButton = panel.querySelector('.shortcut-close');
      if (closeButton) {
        closeButton.focus();
      }
    }
  }

  function toggleShortcutPanel() {
    var panel = ensureShortcutPanel();
    setShortcutPanelOpen(!panel.classList.contains('is-open'));
  }

  function addUtilityButtons() {
    var themeButtonTop = utilityButton('', 'theme-toggle', toggleTheme);
    var keysButtonTop = utilityButton('', 'keys-toggle', toggleShortcutPanel);
    setButtonFace(keysButtonTop, '⌨', 'Keys');
    keysButtonTop.setAttribute('title', 'Show keyboard shortcuts (?)');
    keysButtonTop.setAttribute('aria-label', 'Show keyboard shortcuts');
    themeButtons.push(themeButtonTop);

    if (topbar) {
      var topbarActions = topbar.querySelector('.topbar-actions');
      if (!topbarActions) {
        topbarActions = document.createElement('div');
        topbarActions.className = 'topbar-actions';
        topbar.appendChild(topbarActions);
      }

      if (menuButton && !topbarActions.contains(menuButton)) {
        topbarActions.appendChild(menuButton);
      }

      topbarActions.appendChild(themeButtonTop);
      topbarActions.appendChild(keysButtonTop);
    }

    if (sidebar) {
      var themeButtonSide = utilityButton('', 'theme-toggle', toggleTheme);
      var keysButtonSide = utilityButton('', '', toggleShortcutPanel);
      setButtonFace(keysButtonSide, '⌨', 'Shortcuts');
      themeButtons.push(themeButtonSide);

      var sidebarUtilities = document.createElement('div');
      sidebarUtilities.className = 'sidebar-utilities';
      sidebarUtilities.appendChild(themeButtonSide);
      sidebarUtilities.appendChild(keysButtonSide);
      sidebar.appendChild(sidebarUtilities);
    }
  }

  markCurrentChapter();
  applyTheme(readStoredTheme() || 'light');
  addUtilityButtons();
  updateMenuButtonState();
  updateThemeButtons(currentTheme());

  if (menuButton && page) {
    menuButton.addEventListener('click', function () {
      toggleMenu();
    });

    document.addEventListener('click', function (event) {
      if (!isMenuOpen()) {
        return;
      }

      var isInsideSidebar = event.target.closest('.sidebar');
      var isButton = event.target.closest('.menu-toggle');
      if (!isInsideSidebar && !isButton) {
        setMenuOpen(false);
      }
    });
  }

  document.addEventListener('keydown', function (event) {
    var key = event.key;

    if (key === 'Escape') {
      resetGPrefix();
      setShortcutPanelOpen(false);
      setMenuOpen(false);
      return;
    }

    if (isEditableTarget(event.target)) {
      return;
    }

    if (event.metaKey || event.ctrlKey || event.altKey) {
      return;
    }

    if (!event.shiftKey && (key === 'g' || key === 'G')) {
      event.preventDefault();
      if (gPrefixActive) {
        resetGPrefix();
        scrollToTop();
      } else {
        activateGPrefix();
      }
      return;
    }

    resetGPrefix();

    if (key === '?' || (event.shiftKey && key === '/')) {
      event.preventDefault();
      toggleShortcutPanel();
      return;
    }

    if (key === 'd' || key === 'D') {
      event.preventDefault();
      toggleTheme();
      return;
    }

    if (key === 'm' || key === 'M') {
      event.preventDefault();
      toggleMenu();
      return;
    }

    if (key === 'h' || key === 'H') {
      event.preventDefault();
      navigateRelative(-1);
      return;
    }

    if (key === 'l' || key === 'L') {
      event.preventDefault();
      navigateRelative(1);
      return;
    }

    if (key === 'j' || key === 'J') {
      event.preventDefault();
      scrollByViewport(0.55);
      return;
    }

    if (key === 'k' || key === 'K') {
      event.preventDefault();
      scrollByViewport(-0.55);
      return;
    }

    if (key === 'G') {
      event.preventDefault();
      scrollToBottom();
    }
  });
})();
