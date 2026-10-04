#pragma once
#include <QLabel>
#include <QPainter>

namespace swak {
// Keep the full text available when the window is too narrow.
class SingleLineLabel final : public QLabel
{
public:
    explicit SingleLineLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    SingleLineLabel(const QString &text, QWidget *parent) : SingleLineLabel(parent)
    {
        setText(text);
    }
    QSize minimumSizeHint() const override
    {
        return {0, QLabel::minimumSizeHint().height()};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (toolTip() != text()) {
            setToolTip(text());
        }
        QPainter painter(this);
        painter.setPen(
            palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
        QString line = text();
        line.replace('\n', ' ');
        line.replace('\r', ' ');
        painter.drawText(contentsRect(),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(line, Qt::ElideRight, contentsRect().width()));
    }
};
} // namespace swak
