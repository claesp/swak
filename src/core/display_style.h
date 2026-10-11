#pragma once
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QRgb>
#include <QString>

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

// Modern colors.
namespace modern {
inline constexpr QRgb Background = 0xff0a0a0f;
inline constexpr QRgb Panel = 0xff12121c;
inline constexpr QRgb Raised = 0xff1b1b28;
inline constexpr QRgb Accent = 0xff00d4ff;
inline constexpr QRgb Text = 0xffe8eaed;
inline constexpr QRgb Dim = 0xff7d838c;
inline constexpr QRgb Alert = 0xffff3b3b;
} // namespace modern

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
signals:
    void changed(swak::DisplayMode mode);

private:
    void restore();
    DisplayMode mode_ = DisplayMode::Standard;
    bool saved_ = false;
    QString styleName_;
    QPalette palette_;
    QFont font_;
    QString styleSheet_;
};
} // namespace swak
