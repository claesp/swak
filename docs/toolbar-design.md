# Compact toolbar design

The main toolbar uses 24-pixel vector icons, separators, and tooltips. Plugin names no longer occupy horizontal space. Text remains in the menu actions and accessible button names. Marker and Area share their QAction objects between the menus and toolbar. Their checked state indicates an active mode.

Qt supports icon-only buttons, action sharing, separators, and overflow menus. These features avoid a second custom toolbar layout. The provider button also uses an icon with a dropdown arrow. Its tooltip names the selected provider. Labels above groups reduce horizontal width but add a permanent row. The application uses tooltips instead to preserve map height.

KDE guidance recommends recognizable imagery when a button shows no text. The chosen symbols use a trash can, pin, area outline, globe, pencil, and movement arrows. Existing marker symbols retain their colors and geometry in object menus. The object chooser contains no heading, numbering, or type suffix. Tooltips retain type and source details.

Sources: [Qt 6.8 QToolBar](https://doc.qt.io/qt-6.8/qtoolbar.html), [Qt QToolButton](https://doc.qt.io/qt-6.8/qtoolbutton.html), [KDE icon guidance](https://develop.kde.org/hig/icons/), and [Lucide](https://lucide.dev/).
