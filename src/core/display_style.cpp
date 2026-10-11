#include "core/display_style.h"

#include "core/map_label.h"

#include <QApplication>
#include <QFontDatabase>
#include <QStyle>
#include <QStyleFactory>

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
QString modernStyleSheet(const DisplayFonts &fonts, double pointSize)
{
    const auto line = color(modern::Accent, 70);
    const auto glow = color(modern::Accent, 150);
    const auto accent = color(modern::Accent);
    const auto panel = color(modern::Panel);
    const auto raised = color(modern::Raised);
    const auto background = color(modern::Background);
    const auto text = color(modern::Text);
    const auto dim = color(modern::Dim);
    const auto title =
        fonts.title.isEmpty() ? QString{} : QStringLiteral("font-family: \"%1\";").arg(fonts.title);
    const auto data =
        fonts.data.isEmpty() ? QString{} : QStringLiteral("font-family: \"%1\";").arg(fonts.data);
    // With an application style sheet, QApplication::setFont does not reach every widget.
    // The base rule sets the interface face. More specific rules override it.
    const auto base = fonts.interface.isEmpty()
                          ? QString{}
                          : QStringLiteral("QWidget { font-family: \"%1\"; font-weight: 600;"
                                           " font-size: %2pt; }")
                                .arg(fonts.interface)
                                .arg(pointSize, 0, 'f', 1);
    // Pane titles use the wide title face, as film interfaces do.
    const auto paneTitle =
        fonts.title.isEmpty()
            ? QString{}
            : QStringLiteral("QDockWidget { font-family: \"%1\"; font-weight: 400;"
                             " font-size: %2pt; }")
                  .arg(fonts.title)
                  .arg(std::max(pointSize - 3, 7.0), 0, 'f', 1);
    return base + paneTitle +
           QStringLiteral(
               "QMainWindow, QDialog { background: %1; }"
               "QMainWindow::separator { background: %2; width: 1px; height: 1px; }"
               "QDockWidget { color: %3; }"
               "QDockWidget::title { background: %4; border-bottom: 1px solid %2;"
               " padding: 5px 8px; text-align: left; }"
               "QDockWidget QLabel#paneTitle, QDialog > QLabel#dialogTitle { %5 }"
               "QMenuBar { background: %1; color: %3; border-bottom: 1px solid %2; }"
               "QMenuBar::item { padding: 4px 10px; background: transparent; }"
               "QMenuBar::item:selected { color: %6; background: %4; }"
               "QMenu { background: %4; color: %3; border: 1px solid %2; padding: 4px; }"
               "QMenu::item { padding: 4px 22px 4px 22px; }"
               "QMenu::item:selected { background: %7; color: white; }"
               "QMenu::separator { height: 1px; background: %2; margin: 4px 8px; }"
               "QToolBar { background: %1; border: none; border-bottom: 1px solid %2;"
               " spacing: 4px; padding: 2px; }"
               "QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 3px; }"
               "QToolButton:hover { border-color: %8; background: %4; }"
               "QToolButton:checked { border-color: %6; background: %7; }"
               "QPushButton { background: %9; color: %3; border: 1px solid %2;"
               " border-radius: 6px; padding: 4px 12px; }"
               "QPushButton:hover { border-color: %8; }"
               "QPushButton:pressed, QPushButton:checked { background: %7; border-color: %6; }"
               "QPushButton:disabled { color: %10; border-color: %4; }"
               "QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
               " background: %1; color: %3; border: 1px solid %2; border-radius: 4px;"
               " padding: 3px; selection-background-color: %7; }"
               "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus,"
               " QDoubleSpinBox:focus, QComboBox:focus { border-color: %6; }"
               "QComboBox QAbstractItemView { background: %4; border: 1px solid %2;"
               " selection-background-color: %7; }"
               "QTableView, QTreeView, QListView { background: %1; alternate-background-color:"
               " %9; gridline-color: %2; border: 1px solid %2; selection-background-color: %7; }"
               "QHeaderView::section { background: %4; color: %10; border: none;"
               " border-bottom: 1px solid %2; padding: 4px; }"
               "QTabWidget::pane { border: 1px solid %2; }"
               "QTabBar::tab { background: %4; color: %10; padding: 5px 12px;"
               " border: 1px solid %2; border-bottom: none; }"
               "QTabBar::tab:selected { color: %6; border-top: 2px solid %6; }"
               "QScrollBar:vertical { background: %1; width: 8px; margin: 0; }"
               "QScrollBar:horizontal { background: %1; height: 8px; margin: 0; }"
               "QScrollBar::handle { background: %2; border-radius: 4px; min-height: 24px;"
               " min-width: 24px; }"
               "QScrollBar::handle:hover { background: %8; }"
               "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
               "QToolTip { background: %4; color: %3; border: 1px solid %6; padding: 4px; }"
               "QStatusBar { background: %1; color: %10; border-top: 1px solid %2; }"
               "QStatusBar QLabel { %11 }"
               "QCheckBox::indicator, QRadioButton::indicator { width: 13px; height: 13px;"
               " border: 1px solid %8; background: %1; }"
               "QRadioButton::indicator { border-radius: 7px; }"
               "QCheckBox::indicator:checked, QRadioButton::indicator:checked {"
               " background: %6; }")
               .arg(background,
                    line,
                    text,
                    panel,
                    title,
                    accent,
                    color(modern::Accent, 90),
                    glow,
                    raised)
               .arg(dim, data);
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
        if (!fonts.interface.isEmpty()) {
            QFont font(fonts.interface);
            font.setPointSizeF(pointSize);
            font.setWeight(QFont::DemiBold);
            QApplication::setFont(font);
        }
        qApp->setStyleSheet(modernStyleSheet(fonts, pointSize));
        setMapLabelStyle(MapLabelStyle::Modern);
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
    qApp->setStyleSheet(styleSheet_);
    if (auto *style = QStyleFactory::create(styleName_)) {
        QApplication::setStyle(style);
    }
    QApplication::setPalette(palette_);
    QApplication::setFont(font_);
}
} // namespace swak
