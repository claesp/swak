#pragma once
#include "core/modern_colors.h"

#include <QFont>
#include <QObject>
#include <QPalette>
#include <QRgb>
#include <QString>
#include <QWidget>

#include <optional>

namespace swak {
// Standard follows the desktop style. Modern is a dark interface in the style of film user
// interfaces: glass panels, cyan accents, condensed and wide display fonts, and a map HUD.
enum class DisplayMode
{
    Standard,
    Modern
};
QString displayModeId(DisplayMode mode);
std::optional<DisplayMode> displayModeFromId(const QString &id);

// The families of the bundled fonts. A family is empty when its font cannot be loaded.
struct DisplayFonts
{
    // Wide display face for titles: Michroma.
    QString title;
    // Semi-condensed face for interface text: Rajdhani.
    QString interface;
    // Monospace face for coordinates, times, and tags: JetBrains Mono.
    QString data;
};
// Loads the bundled fonts on the first call. Call it on the interface thread.
const DisplayFonts &displayFonts();

// Applies the display mode to the application: style, palette, font, and style sheet. Standard
// restores the values from before the first change. The destructor also restores them, so a
// closed window does not leave its look on the application.
class DisplayStyle final : public QObject
{
    Q_OBJECT
public:
    explicit DisplayStyle(QObject *parent = nullptr);
    ~DisplayStyle() override;
    DisplayStyle(const DisplayStyle &) = delete;
    DisplayStyle &operator=(const DisplayStyle &) = delete;
    DisplayMode mode() const
    {
        return mode_;
    }
    void setMode(DisplayMode mode);
    // A font of the data face at a pixel size, or the fixed system font without that face.
    static QFont dataFont(int pixelSize);
    // A font of the title face at a pixel size with wide letter spacing.
    static QFont titleFont(int pixelSize);
    // Marks widgets whose font Modern set. Standard resets exactly these widgets.
    static constexpr const char *FontProperty = "swakDisplayFont";
signals:
    void changed(swak::DisplayMode mode);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void restore();
    void applyFont(QWidget *widget) const;
    DisplayMode mode_ = DisplayMode::Standard;
    bool saved_ = false;
    QString styleName_;
    QPalette palette_;
    QFont font_;
    QString styleSheet_;
    QFont interfaceFont_;
    QFont paneFont_;
    QFont statusFont_;
};
} // namespace swak
