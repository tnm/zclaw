(function () {
  var THEME_STORAGE_KEY = 'zclaw_docs_theme';
  var THEME_ORDER = ['kr', 'light', 'dark'];
  var page = document.querySelector('.page');
  var sidebar = document.querySelector('.sidebar');
  var topbar = document.querySelector('.topbar');
  var menuButton = document.querySelector('.menu-toggle');
  var links = Array.prototype.slice.call(document.querySelectorAll('.chapter-list a'));
  var current = window.location.pathname.split('/').pop() || 'index.html';
  var themeSwitchers = [];
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

  function normalizeTheme(theme) {
    return THEME_ORDER.indexOf(theme) >= 0 ? theme : 'kr';
  }

  function currentTheme() {
    return normalizeTheme(document.documentElement.getAttribute('data-theme'));
  }

  function nextTheme(theme) {
    var normalized = normalizeTheme(theme);
    var index = THEME_ORDER.indexOf(normalized);
    return THEME_ORDER[(index + 1) % THEME_ORDER.length];
  }

  function themeLabel(theme) {
    if (theme === 'kr') {
      return 'K&R';
    }
    if (theme === 'light') {
      return 'Day';
    }
    return 'Dusk';
  }

  function updateThemeSwitchers(theme) {
    var normalized = normalizeTheme(theme);
    themeSwitchers.forEach(function (switcher) {
      var options = Array.prototype.slice.call(switcher.querySelectorAll('.theme-choice'));
      options.forEach(function (option) {
        var optionTheme = option.getAttribute('data-theme');
        var active = optionTheme === normalized;
        option.classList.toggle('is-active', active);
        option.setAttribute('aria-pressed', active ? 'true' : 'false');
      });

      var next = nextTheme(normalized);
      switcher.setAttribute('title', 'Theme style. D cycles to ' + themeLabel(next));
      switcher.setAttribute('aria-label', 'Theme style selector');
    });
  }

  function applyTheme(theme) {
    var normalized = normalizeTheme(theme);
    document.documentElement.setAttribute('data-theme', normalized);
    updateThemeSwitchers(normalized);
  }

  function setTheme(theme) {
    var normalized = normalizeTheme(theme);
    applyTheme(normalized);
    saveTheme(normalized);
  }

  function toggleTheme() {
    setTheme(nextTheme(currentTheme()));
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

  function utilityLink(label, className, href) {
    var link = document.createElement('a');
    link.className = className ? 'utility-btn utility-link ' + className : 'utility-btn utility-link';
    link.href = href;
    link.textContent = label;
    return link;
  }

  function setButtonLabel(button, label) {
    button.textContent = label;
  }

  function createThemeSwitcher(className) {
    var switcher = document.createElement('div');
    switcher.className = className ? 'theme-switcher ' + className : 'theme-switcher';
    switcher.setAttribute('role', 'group');
    switcher.setAttribute('aria-label', 'Theme styles');

    THEME_ORDER.forEach(function (theme) {
      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'theme-choice';
      button.textContent = themeLabel(theme);
      button.setAttribute('data-theme', theme);
      button.setAttribute('aria-pressed', 'false');
      button.setAttribute('title', 'Set theme: ' + themeLabel(theme));
      button.addEventListener('click', function () {
        setTheme(theme);
      });
      switcher.appendChild(button);
    });

    themeSwitchers.push(switcher);
    return switcher;
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
      '      <tr><th><kbd>D</kbd></th><td>Cycle K&amp;R, Day, and Dusk modes</td></tr>' +
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
    var themeSwitcherTop = createThemeSwitcher('theme-switcher-top');
    var keysButtonTop = utilityButton('', 'keys-toggle', toggleShortcutPanel);
    setButtonLabel(keysButtonTop, 'Keys');
    keysButtonTop.setAttribute('title', 'Show keyboard shortcuts (?)');
    keysButtonTop.setAttribute('aria-label', 'Show keyboard shortcuts');

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

      topbarActions.appendChild(themeSwitcherTop);
      topbarActions.appendChild(keysButtonTop);
    }

    if (sidebar) {
      var themeSwitcherSide = createThemeSwitcher('theme-switcher-side');
      var readmeButtonSide = utilityLink('README (good for agents)', 'readme-link', 'reference/README_COMPLETE.md');
      var keysButtonSide = utilityButton('', '', toggleShortcutPanel);
      setButtonLabel(keysButtonSide, 'Shortcuts');

      var sidebarUtilities = document.createElement('div');
      sidebarUtilities.className = 'sidebar-utilities';
      sidebarUtilities.appendChild(themeSwitcherSide);
      sidebarUtilities.appendChild(readmeButtonSide);
      sidebarUtilities.appendChild(keysButtonSide);
      sidebar.appendChild(sidebarUtilities);
    }
  }

  markCurrentChapter();
  applyTheme(readStoredTheme() || 'kr');
  addUtilityButtons();
  updateMenuButtonState();
  updateThemeSwitchers(currentTheme());

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
