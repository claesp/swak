#pragma once
#include "core/store.h"
#include <QMainWindow>

namespace swak {
class Window final : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(QWidget *parent = nullptr);
    ~Window() override;
    Store &store() { return store_; }
private:
    Store store_;
};
}
