#pragma once
#include <QPointer>

#include <utility>

namespace swak {
// Owns a modal dialog or menu on the heap.
// Qt deletes a child object with its parent. A nested exec() loop can delete that parent.
// A stack object would then get a second delete. This owner deletes only a live object.
// After exec() returns, check the owner before you use the object or its parent again.
template <typename T> class Modal final
{
public:
    template <typename... Args>
    explicit Modal(Args &&...args) : object_(new T(std::forward<Args>(args)...))
    {
    }
    ~Modal()
    {
        delete object_.data();
    }
    Modal(const Modal &) = delete;
    Modal &operator=(const Modal &) = delete;
    Modal(Modal &&) = delete;
    Modal &operator=(Modal &&) = delete;
    T *get() const
    {
        return object_.data();
    }
    T *operator->() const
    {
        return object_.data();
    }
    T &operator*() const
    {
        return *object_;
    }
    explicit operator bool() const
    {
        return !object_.isNull();
    }

private:
    QPointer<T> object_;
};
} // namespace swak
