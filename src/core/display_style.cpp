#include "core/display_style.h"

#include "core/map_label.h"

#include <QApplication>
#include <QDockWidget>
#include <QEvent>
#include <QFontDatabase>
#include <QLayout>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QTabBar>

#include <algorithm>

// The resource lives in the static core library. Initialize it before the first font load.
static void initializeFontResources()
{
    Q_INIT_RESOURCE(swak_fonts);
}

namespace swak {
namespace {
QString loadFamily(const QString &path)
{
    const int id = QFontDatabase::addApplicationFont(path);
    const auto families = id < 0 ? QStringList{} : QFontDatabase::applicationFontFamilies(id);
    // Some files also name a weight family, such as "Rajdhani Medium". The shortest name is
    // the shared family of all weights.
    const auto shortest = std::min_element(
        families.cbegin(), families.cend(), [](const QString &a, const QString &b) {
            return a.size() < b.size();
        });
    return shortest == families.cend() ? QString{} : *shortest;
}
QString color(QRgb value, int alpha = 255)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(qRed(value))
        .arg(qGreen(value))
        .arg(qBlue(value))
        .arg(alpha);
}
QPalette modernPalette()
{
    QPalette palette;
    const QColor background(modern::Background), panel(modern::Panel), raised(modern::Raised),
        accent(modern::Accent), text(modern::Text), dim(modern::Dim);
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        palette.setColor(group, QPalette::Window, panel);
        palette.setColor(group, QPalette::WindowText, disabled ? dim : text);
        palette.setColor(group, QPalette::Base, background);
        palette.setColor(group, QPalette::AlternateBase, raised);
        palette.setColor(group, QPalette::Text, disabled ? dim : text);
        palette.setColor(group, QPalette::PlaceholderText, dim);
        palette.setColor(group, QPalette::Button, raised);
        palette.setColor(group, QPalette::ButtonText, disabled ? dim : text);
        palette.setColor(group, QPalette::BrightText, QColor(modern::Alert));
        palette.setColor(group, QPalette::Highlight, disabled ? raised : accent.darker(160));
        palette.setColor(group, QPalette::HighlightedText, QColor(Qt::white));
        palette.setColor(group, QPalette::ToolTipBase, panel);
        palette.setColor(group, QPalette::ToolTipText, text);
        palette.setColor(group, QPalette::Link, accent);
        palette.setColor(group, QPalette::LinkVisited, accent.darker(130));
        palette.setColor(group, QPalette::Light, raised.lighter(150));
        palette.setColor(group, QPalette::Midlight, raised.lighter(120));
        palette.setColor(group, QPalette::Mid, raised);
        palette.setColor(group, QPalette::Dark, background);
        palette.setColor(group, QPalette::Shadow, QColor(Qt::black));
    }
    return palette;
}
QString modernStyleSheet()
{
    // Named colors keep the rules readable. Fonts are not in the style sheet: Qt keeps fonts
    // from a removed style sheet on some widgets, so DisplayStyle sets them on each widget.
    QString sheet = QStringLiteral(
        "QMainWindow, QDialog { background: @background; }"
        "QMainWindow::separator { background: @line; width: 1px; height: 1px; }"
        "QDockWidget { color: @text; }"
        "QDockWidget::title { background: transparent; border-bottom: 1px solid @line;"
        " padding: 6px 10px; text-align: left; }"
        // Panes over the map are glass: the map panels paint the rounded background.
        "QMainWindow#mapPanels QDockWidget > QWidget,"
        " QMainWindow#mapPanels QScrollArea#paneScroll,"
        " QMainWindow#mapPanels QScrollArea#paneScroll > QWidget > QWidget"
        " { background: transparent; }"
        "QMenuBar { background: @background; color: @text; border-bottom: 1px solid @line; }"
        "QMenuBar::item { padding: 4px 10px; background: transparent; }"
        "QMenuBar::item:selected { color: @accent; background: @panel; }"
        "QMenu { background: @panel; color: @text; border: 1px solid @line; padding: 4px; }"
        "QMenu::item { padding: 4px 22px 4px 22px; }"
        "QMenu::item:selected { background: @selection; color: white; }"
        "QMenu::separator { height: 1px; background: @line; margin: 4px 8px; }"
        "QToolBar { background: @background; border: none; border-bottom: 1px solid @line;"
        " spacing: 4px; padding: 2px; }"
        // The command dock is the main toolbar at the bottom of the map.
        "QToolBar#mainToolbar[commandDock=\"true\"] { background: @glass; border: 1px solid"
        " @glow; border-radius: 14px; padding: 6px 10px; spacing: 6px; }"
        "QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 3px; }"
        "QToolButton:hover { border-color: @glow; background: @panel; }"
        "QToolButton:checked { border-color: @accent; background: @selection; }"
        "QPushButton { background: @raised; color: @text; border: 1px solid @line;"
        " border-radius: 6px; padding: 4px 12px; }"
        "QPushButton:hover { border-color: @glow; }"
        "QPushButton:pressed, QPushButton:checked { background: @selection;"
        " border-color: @accent; }"
        "QPushButton:disabled { color: @dim; border-color: @panel; }"
        "QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
        " background: @background; color: @text; border: 1px solid @line; border-radius: 4px;"
        " padding: 3px; selection-background-color: @selection; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus,"
        " QDoubleSpinBox:focus, QComboBox:focus { border-color: @accent; }"
        "QComboBox QAbstractItemView { background: @panel; border: 1px solid @line;"
        " selection-background-color: @selection; }"
        "QTableView, QTreeView, QListView { background: @background; alternate-background-color:"
        " @raised; gridline-color: @line; border: 1px solid @line;"
        " selection-background-color: @selection; }"
        "QHeaderView::section { background: @panel; color: @dim; border: none;"
        " border-bottom: 1px solid @line; padding: 4px; }"
        "QTabWidget::pane { border: 1px solid @line; }"
        "QTabBar::tab { background: @panel; color: @dim; padding: 5px 12px;"
        " border: 1px solid @line; border-bottom: none; }"
        "QTabBar::tab:selected { color: @accent; border-top: 2px solid @accent; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
        "QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }"
        "QScrollBar::handle { background: @line; border-radius: 4px; min-height: 24px;"
        " min-width: 24px; }"
        "QScrollBar::handle:hover { background: @glow; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
        "QToolTip { background: @panel; color: @text; border: 1px solid @accent;"
        " padding: 4px; }"
        "QStatusBar { background: @background; color: @dim; border-top: 1px solid @line; }"
        "QCheckBox::indicator, QRadioButton::indicator { width: 13px; height: 13px;"
        " border: 1px solid @glow; background: @background; }"
        "QRadioButton::indicator { border-radius: 7px; }"
        "QCheckBox::indicator:checked, QRadioButton::indicator:checked {"
        " background: @accent; }");
    // Replace longer names first, so that no name is a prefix of a replaced one.
    for (const auto &[name, value] :
         {std::pair{QStringLiteral("@background"), color(modern::Background)},
          std::pair{QStringLiteral("@selection"), color(modern::Accent, 90)},
          std::pair{QStringLiteral("@raised"), color(modern::Raised)},
          std::pair{QStringLiteral("@accent"), color(modern::Accent)},
          std::pair{QStringLiteral("@panel"), color(modern::Panel)},
          std::pair{QStringLiteral("@glass"), color(modern::Panel, 215)},
          std::pair{QStringLiteral("@line"), color(modern::Accent, 70)},
          std::pair{QStringLiteral("@glow"), color(modern::Accent, 150)},
          std::pair{QStringLiteral("@text"), color(modern::Text)},
          std::pair{QStringLiteral("@dim"), color(modern::Dim)}}) {
        sheet.replace(name, value);
    }
    return sheet;
}
} // namespace

QString displayModeId(DisplayMode mode)
{
    switch (mode) {
    case DisplayMode::Standard:
        return QStringLiteral("standard");
    case DisplayMode::Modern:
        return QStringLiteral("modern");
    }
    return QStringLiteral("standard");
}
std::optional<DisplayMode> displayModeFromId(const QString &id)
{
    if (id == u"standard") {
        return DisplayMode::Standard;
    }
    if (id == u"modern") {
        return DisplayMode::Modern;
    }
    return std::nullopt;
}
const DisplayFonts &displayFonts()
{
    static const DisplayFonts fonts = [] {
        initializeFontResources();
        DisplayFonts result;
        result.title = loadFamily(QStringLiteral(":/fonts/Michroma-Regular.ttf"));
        result.interface = loadFamily(QStringLiteral(":/fonts/Rajdhani-Medium.ttf"));
        // The second weight joins the same family.
        static_cast<void>(loadFamily(QStringLiteral(":/fonts/Rajdhani-SemiBold.ttf")));
        result.data = loadFamily(QStringLiteral(":/fonts/JetBrainsMono-Variable.ttf"));
        return result;
    }();
    return fonts;
}
DisplayStyle::DisplayStyle(QObject *parent) : QObject(parent) {}
DisplayStyle::~DisplayStyle()
{
    restore();
}
QFont DisplayStyle::dataFont(int pixelSize)
{
    const auto &family = displayFonts().data;
    QFont font =
        family.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont(family);
    font.setPixelSize(std::max(pixelSize, 6));
    font.setStyleHint(QFont::Monospace);
    return font;
}
QFont DisplayStyle::titleFont(int pixelSize)
{
    const auto &family = displayFonts().title;
    QFont font = family.isEmpty() ? QApplication::font() : QFont(family);
    font.setPixelSize(std::max(pixelSize, 6));
    font.setLetterSpacing(QFont::PercentageSpacing, 135);
    return font;
}
void DisplayStyle::setMode(DisplayMode mode)
{
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    if (mode == DisplayMode::Standard) {
        restore();
    } else {
        if (!saved_) {
            styleName_ = QApplication::style()->name();
            palette_ = QApplication::palette();
            font_ = QApplication::font();
            styleSheet_ = qApp->styleSheet();
            saved_ = true;
        }
        const auto &fonts = displayFonts();
        if (auto *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
            QApplication::setStyle(fusion);
        }
        QApplication::setPalette(modernPalette());
        // Rajdhani is small for its point size. A larger size keeps text readable.
        const double pointSize = std::max(font_.pointSizeF(), 9.0) + 2;
        interfaceFont_ = fonts.interface.isEmpty() ? font_ : QFont(fonts.interface);
        interfaceFont_.setPointSizeF(pointSize);
        interfaceFont_.setWeight(QFont::DemiBold);
        paneFont_ = fonts.title.isEmpty() ? interfaceFont_ : QFont(fonts.title);
        paneFont_.setPointSizeF(std::max(pointSize - 3, 7.0));
        statusFont_ = fonts.data.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                           : QFont(fonts.data);
        statusFont_.setPointSizeF(std::max(pointSize - 2, 7.0));
        QApplication::setFont(interfaceFont_);
        qApp->setStyleSheet(modernStyleSheet());
        setMapLabelStyle(MapLabelStyle::Modern);
        // Set the fonts on existing widgets, and on new widgets when Qt polishes them.
        qApp->installEventFilter(this);
        for (auto *widget : QApplication::allWidgets()) {
            applyFont(widget);
        }
    }
    // Layouts cache their minimum sizes. A font change does not always reach the layouts of
    // the parent widgets, so recompute all of them.
    for (auto *widget : QApplication::allWidgets()) {
        if (auto *layout = widget->layout()) {
            layout->invalidate();
        }
        widget->updateGeometry();
    }
    emit changed(mode_);
}
void DisplayStyle::restore()
{
    setMapLabelStyle(MapLabelStyle::Standard);
    if (!saved_) {
        return;
    }
    saved_ = false;
    qApp->removeEventFilter(this);
    qApp->setStyleSheet(styleSheet_);
    if (auto *style = QStyleFactory::create(styleName_)) {
        QApplication::setStyle(style);
    }
    QApplication::setPalette(palette_);
    QApplication::setFont(font_);
    for (auto *widget : QApplication::allWidgets()) {
        // An empty font clears the font that Modern set, so the widget inherits again.
        if (widget->property(FontProperty).toBool()) {
            widget->setProperty(FontProperty, {});
            widget->setFont(QFont());
        }
        // When Qt removes a style sheet, it gives each styled widget the palette that the widget
        // had when Qt styled it, which is the Modern palette. An empty palette makes the widget
        // inherit the application palette again. Widgets with their own palette keep it.
        // Tab bars set the palettes of their scroll buttons themselves.
        if (!widget->testAttribute(Qt::WA_SetPalette) &&
            !qobject_cast<QTabBar *>(widget->parentWidget())) {
            widget->setPalette(QPalette());
        }
    }
}
bool DisplayStyle::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Polish && object->isWidgetType()) {
        applyFont(static_cast<QWidget *>(object));
    }
    return false;
}
void DisplayStyle::applyFont(QWidget *widget) const
{
    // Keep the fonts that the application chose on purpose, such as larger titles.
    if (widget->property(FontProperty).toBool() || widget->testAttribute(Qt::WA_SetFont)) {
        return;
    }
    const QFont *font = &interfaceFont_;
    if (qobject_cast<QDockWidget *>(widget)) {
        font = &paneFont_;
    } else {
        for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
            if (qobject_cast<QStatusBar *>(parent)) {
                font = &statusFont_;
                break;
            }
        }
    }
    widget->setFont(*font);
    widget->setProperty(FontProperty, true);
}
} // namespace swak
