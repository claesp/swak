#include "core/quit_signals.h"

#include <QSocketNotifier>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace swak {
namespace {
// The write end of the pipe of the active object, or -1.
std::atomic<int> signalPipe{-1};
static_assert(std::atomic<int>::is_always_lock_free);

void writeSignal(int signal)
{
    const int saved = errno;
    const int fd = signalPipe.load();
    if (fd >= 0) {
        const auto byte = static_cast<unsigned char>(signal);
        // A full pipe already holds a quit request, so a failed write loses nothing.
        static_cast<void>(::write(fd, &byte, 1));
    }
    errno = saved;
}
} // namespace
QuitSignals::QuitSignals(QObject *parent) : QObject(parent)
{
    if (signalPipe.load() >= 0 || ::pipe2(pipe_.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        pipe_ = {-1, -1};
        return;
    }
    struct sigaction action{};
    action.sa_handler = writeSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    signalPipe.store(pipe_[1]);
    if (::sigaction(SIGTERM, &action, &previousTerminate_) != 0) {
        signalPipe.store(-1);
        return;
    }
    if (::sigaction(SIGINT, &action, &previousInterrupt_) != 0) {
        ::sigaction(SIGTERM, &previousTerminate_, nullptr);
        signalPipe.store(-1);
        return;
    }
    notifier_ = new QSocketNotifier(pipe_[0], QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &QuitSignals::read);
    active_ = true;
}
QuitSignals::~QuitSignals()
{
    if (active_) {
        ::sigaction(SIGTERM, &previousTerminate_, nullptr);
        ::sigaction(SIGINT, &previousInterrupt_, nullptr);
        signalPipe.store(-1);
    }
    delete notifier_;
    for (const auto fd : pipe_) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
}
bool QuitSignals::active() const
{
    return active_;
}
void QuitSignals::read()
{
    // Read a bounded number of bytes per notification. The notifier reports the rest.
    std::array<unsigned char, 64> bytes{};
    const auto count = ::read(pipe_[0], bytes.data(), bytes.size());
    for (ssize_t index = 0; index < count; ++index) {
        emit requested(bytes.at(static_cast<std::size_t>(index)));
    }
}
} // namespace swak
