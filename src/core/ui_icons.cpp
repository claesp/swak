#include "core/ui_icons.h"
#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <algorithm>
namespace swak {
namespace {
#include "core/ui_icon_paths.inc"
class VectorIcon final : public QIconEngine {
public:
    explicit VectorIcon(UiIcon icon) : path_(iconPath(icon)) {}
    VectorIcon(CollectionSymbol symbol, QColor color) : symbol_(std::move(symbol)), color_(std::move(color)) {}
    QIconEngine *clone() const override { return new VectorIcon(*this); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        const auto side = std::min(rect.width(), rect.height());
        painter->translate(rect.center().x() + 0.5 - side / 2.0, rect.center().y() + 0.5 - side / 2.0);
        painter->scale(side / 24.0, side / 24.0);
        painter->setRenderHint(QPainter::Antialiasing);
        if (symbol_) {
            if (mode == QIcon::Disabled) painter->setOpacity(0.4);
            paintCollectionSymbol(*painter, *symbol_, {12, 12}, 8, color_);
        } else {
            const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
            painter->setPen(QPen(QApplication::palette().color(group, mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::WindowText), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush); painter->drawPath(path_);
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap image(size); image.fill(Qt::transparent);
        QPainter painter(&image); paint(&painter, QRect(QPoint{}, size), mode, state); return image;
    }
private:
    QPainterPath path_;
    std::optional<CollectionSymbol> symbol_;
    QColor color_;
};
}
QIcon uiIcon(UiIcon icon) { return QIcon(new VectorIcon(icon)); }
QIcon collectionIcon(const CollectionSymbol &symbol, const QColor &color) { return QIcon(new VectorIcon(symbol, color)); }
}
