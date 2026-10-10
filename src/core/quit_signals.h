#pragma once
#include <QObject>

#include <array>
#include <csignal>

class QSocketNotifier;

namespace swak {
// Reports SIGTERM and SIGINT on the interface thread, so the application can quit through its
// event loop. Normal shutdown then saves the preferences and stops the workers.
//
// The signal handler only writes one byte to a pipe. Only one object can be active at a time.
// The destructor restores the previous handlers.
class QuitSignals final : public QObject
{
    Q_OBJECT
public:
    explicit QuitSignals(QObject *parent = nullptr);
    ~QuitSignals() override;
    QuitSignals(const QuitSignals &) = delete;
    QuitSignals &operator=(const QuitSignals &) = delete;
    // True when the handlers are installed.
    bool active() const;
signals:
    void requested(int signal);

private:
    void read();
    std::array<int, 2> pipe_{-1, -1};
    QSocketNotifier *notifier_ = nullptr;
    struct sigaction previousTerminate_{};
    struct sigaction previousInterrupt_{};
    bool active_ = false;
};
} // namespace swak
