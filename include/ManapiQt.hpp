#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <QSocketNotifier>
#include <QCoreApplication>
#include <QEvent>
#include <QThread>
#include <QVariant>

#include <manapihttp/ManapiEventLoop.hpp>
#include <manapihttp/ManapiTimerPool.hpp>
#include <manapihttp/std/ManapiRef.hpp>
#include <manapihttp/std/ManapiAsyncContext.hpp>

namespace manapi::qt {
    class event_dispatcher;

    struct poller_data_t {
        uint32_t refcnt;
        event_dispatcher *context;
        QSocketNotifier *read_notifier;
        QSocketNotifier *write_notifier;
        manapi::ev::shared_io watcher;
        int flags;
    };

    struct timer_info_t {
        Qt::TimerType type;
        QAbstractEventDispatcherV2::Duration interval;
    };

    struct timer_data_t {
        uint32_t refcnt;
        event_dispatcher *context;
        std::map<Qt::TimerId, timer_info_t> ids;
        QObject *parent;
    };

    class event_dispatcher final : public QAbstractEventDispatcherV2 {
        Q_DISABLE_COPY(event_dispatcher)
    public:
        event_dispatcher ();

        event_dispatcher (QObject* parent);

        ~event_dispatcher() override;

        static manapi::error::status_or<event_dispatcher *> create () MANAPIHTTP_NOEXCEPT;

        void unsubscribe () MANAPIHTTP_NOEXCEPT;

        bool processEvents(QEventLoop::ProcessEventsFlags flags) override;

        void registerSocketNotifier(QSocketNotifier* notifier) override;

        void unregisterSocketNotifier(QSocketNotifier* notifier) override;

        void registerTimer(Qt::TimerId timerId, Duration interval, Qt::TimerType timerType, QObject *object) override;

        bool unregisterTimer(Qt::TimerId timerId) override;

        QList<TimerInfoV2> timersForObject(QObject *object) const override;

        MANAPIHTTP_NODISCARD Duration remainingTime(Qt::TimerId timerId) const override;

        bool unregisterTimers(QObject *object) override;

        void wakeUp() override;

        void interrupt() override;

        void startingUp() override;

        void closingDown() override;

        void enableSocketNotifier(QSocketNotifier* notifier);

        void disableSocketNotifier(QSocketNotifier* notifier);
    private:
        MANAPIHTTP_NODISCARD static int qtouv(QSocketNotifier::Type qtEventType) ;

        manapi::ev::shared_async m_wakeupHandle;

        std::uint64_t m_processedCallbacks;

        std::map<Qt::TimerId, std::pair<manapi::timer, manapi::reference<timer_data_t>>> m_timers;

        std::set<poller_data_t *> m_pollers;

        std::atomic<int> flags;
    };
}

Q_DECLARE_METATYPE(manapi::qt::poller_data_t*);

inline manapi::error::status_or<manapi::qt::event_dispatcher *> manapi::qt::event_dispatcher::create() MANAPIHTTP_NOEXCEPT {
    try {
        return new manapi::qt::event_dispatcher();
    }
    catch (std::exception const &e) {
        manapi_log_trace(manapi::debug::LOG_TRACE_MEDIUM, "%s due to %s", "qt:event dispatcher failed", e.what());
        return manapi::error::status_internal("qt:event dispatcher failed");
    }
}

inline manapi::qt::event_dispatcher::event_dispatcher() : event_dispatcher(nullptr) {
}

inline manapi::qt::event_dispatcher::event_dispatcher(QObject* parent)
    : QAbstractEventDispatcherV2(parent) {
    this->m_wakeupHandle = manapi::async::current()->eventloop()->create_watcher_async(nullptr).unwrap();
    this->flags.fetch_or(0b1);
    this->m_processedCallbacks = 0;
}

inline manapi::qt::event_dispatcher::~event_dispatcher() = default;

inline void manapi::qt::event_dispatcher::unsubscribe() MANAPIHTTP_NOEXCEPT {
    if (this->flags & 0b1) {
        this->flags.fetch_xor(0b1);
    }
    manapi::async::current()->eventloop()->stop_watcher(std::move(this->m_wakeupHandle));
    while (!this->m_pollers.empty()) {
        auto it = this->m_pollers.begin();
        manapi::reference<poller_data_t> data (*it);
        manapi::async::current()->eventloop()->stop_watcher(std::move(data->watcher));
        this->m_pollers.erase(it);
    }
}

inline void manapi::qt::event_dispatcher::interrupt() {
}

inline void manapi::qt::event_dispatcher::startingUp() {
}

inline void manapi::qt::event_dispatcher::closingDown() {
    delete this;
}

inline bool manapi::qt::event_dispatcher::processEvents(QEventLoop::ProcessEventsFlags flags) {
    auto &ev = manapi::async::current()->eventloop();
    bool const active = ev->is_active();

    // we are awake!
    emit awake();

    // zero out processed callbacks
    this->m_processedCallbacks = 0;

    // time to send posted events
    QCoreApplication::sendPostedEvents();

    // will we block on libuv run?
    const bool willWait = (flags & QEventLoop::WaitForMoreEvents);

    manapi::sys_error::status status;
    // run libuv poll depending on willWait value
    if (!active) {
        if (willWait) {
            // we will block! signalize it
            emit aboutToBlock();

            status = ev->run(manapi::ev::RUN_ONCE);
        } else {
            // run loop once, do not block on no events
            status = ev->run(manapi::ev::RUN_NOWAIT);
        }

        if (!status) {
            if (status.code() != manapi::ERR_ABORTED) {
                status.unwrap();
            }
            QCoreApplication::quit();
        }
    }

    // return true if we processed something
    return this->m_processedCallbacks > 0;
}

inline void manapi::qt::event_dispatcher::registerSocketNotifier(QSocketNotifier* notifier) {
    // transform QSocketNotifier::Type to uv_poll_event
    int events = qtouv(notifier->type());
    if (events == -1) {
        return;
    }

    auto &ev = manapi::async::current()->eventloop();

    manapi::reference<poller_data_t> ref;

    auto const sock = static_cast<manapi::socket_t>(notifier->socket());
    auto fit = notifier->property("socketData");
    if(fit.isNull()) {
        ref.reset(new poller_data_t{});
        ref->context = this;
        manapi::ev::shared_io watcher;
        try {
            this->m_pollers.insert(ref.get());

            watcher = ev->create_watcher_socket(
                sock, [ref] (const manapi::ev::shared_io &, int status, int events) mutable -> void {
                    ref->context->m_processedCallbacks++;

                    // send required events
                    if (events & manapi::ev::READ) {
                        QEvent e(QEvent::SockAct);
                        QCoreApplication::sendEvent(ref->read_notifier, &e);
                    }

                    if (events & manapi::ev::WRITE) {
                        QEvent e(QEvent::SockAct);
                        QCoreApplication::sendEvent(ref->write_notifier, &e);
                    }
                }).unwrap();
        }
        catch (...) {
            this->m_pollers.erase(ref.get());
            std::rethrow_exception(std::current_exception());
        }

        ref->watcher = std::move(watcher);

        // attach our custom data to QSocketNotifier
        if (!notifier->setProperty("socketData", QVariant::fromValue(ref.get()))) {
            manapi_log_trace(manapi::debug::LOG_TRACE_MEDIUM, "notifier->setProperty with socketData returned false");
        }

    }
    else {
        ref.reset(fit.value<poller_data_t *>());
    }

    // setup read and write notifiers
    if (events & manapi::ev::READ) {
        ref->read_notifier = notifier;
        ref->flags |= manapi::ev::READ;
    }

    // set notifiers
    if (events & manapi::ev::WRITE) {
        ref->write_notifier = notifier;
        ref->flags |= manapi::ev::WRITE;
    }

    if (auto rhs = ref->watcher->start(ref->flags)) {
        manapi_log_error("%s due to %s", "qt:socket watcher failed", manapi::ev::strerror(rhs));
        throw std::runtime_error ("qt:socket watcher failed");
    }
}

inline void manapi::qt::event_dispatcher::unregisterSocketNotifier(QSocketNotifier* notifier) {
    // transform QSocketNotifier::Type to uv_poll_event
    int events = qtouv(notifier->type());
    if (events == -1) {
        return;
    }

    auto fit = notifier->property("socketData");
    assert(!fit.isNull());
    // get our data from libuv handler and actualize events flags
    auto data = fit.value<poller_data_t*>();

    if (data->flags & events & manapi::ev::READ) {
        data->flags ^= manapi::ev::READ;
    }

    if (data->flags & events & manapi::ev::WRITE) {
        data->flags ^= manapi::ev::WRITE;
    }

    // no event types left? schedule deletion of libuv's poller
    if (data->flags == 0) {
        manapi::async::current()->eventloop()->stop_watcher(std::move(data->watcher));
        // we can delete this now
        notifier->setProperty("socketData", QVariant());
        this->m_pollers.erase(data);
    }
    else {
        if (auto rhs = data->watcher->start(data->flags)) {
            manapi_log_error("%s due to %s", "qt:socket watcher failed", manapi::ev::strerror(rhs));
            throw std::runtime_error ("qt:socket watcher failed");
        }
    }
}

inline void manapi::qt::event_dispatcher::enableSocketNotifier(QSocketNotifier* notifier) {
    // transform QSocketNotifier::Type to uv_poll_event
    int events = qtouv(notifier->type());
    if (events == -1) {
        return;
    }

    // get our data from libuv' handler and actualize events flags
    auto fit = notifier->property("socketData");
    assert(!fit.isNull());
    auto data = fit.value<poller_data_t*>();
    data->flags |= events;
    // start libuv's polling
    if (auto rhs = data->watcher->start(data->flags)) {
        manapi_log_error("%s due to %s", "qt:socket watcher failed", manapi::ev::strerror(rhs));
        throw std::runtime_error ("qt:socket watcher failed");
    }
}

inline void manapi::qt::event_dispatcher::disableSocketNotifier(QSocketNotifier* notifier) {
    int events = qtouv(notifier->type());
    if (events == -1) {
        return;
    }

    // get our data from libuv's handler and actualize events flags
    auto fit = notifier->property("socketData");
    assert(!fit.isNull());
    auto data = fit.value<poller_data_t*>();
    if (data->flags & events & manapi::ev::READ) {
        data->flags ^= manapi::ev::READ;
    }
    if (data->flags & events & manapi::ev::WRITE) {
        data->flags ^= manapi::ev::WRITE;
    }
    // if no flags are set stop polling
    // some are set? start libuv's polling with remaining ones
    if (data->flags) {
        if (auto rhs = data->watcher->start(data->flags)) {
            manapi_log_error("%s due to %s", "qt:socket watcher failed", manapi::ev::strerror(rhs));
            throw std::runtime_error ("qt:socket watcher failed");
        }
    }
    else {
        if (auto rhs = data->watcher->stop()) {
            manapi_log_error("%s due to %s", "qt:socket watcher failed", manapi::ev::strerror(rhs));
            throw std::runtime_error ("qt:socket watcher failed");
        }
    }
}

inline QAbstractEventDispatcher::Duration manapi::qt::event_dispatcher::remainingTime(Qt::TimerId timerId) const {
    auto it = this->m_timers.find(timerId);
    if (it != this->m_timers.end()) {
        auto &t = it->second.first;
        // in millseconds
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.remaning());
    }

    // non existent timer
    return QAbstractEventDispatcher::Duration(0);
}

inline void manapi::qt::event_dispatcher::registerTimer(Qt::TimerId timerId, Duration interval, Qt::TimerType timerType, QObject *object) {
    auto fit = object->property("timerData");
    manapi::reference<timer_data_t> ref;
    if (fit.isNull()) {
        ref.reset(new timer_data_t{});
        ref->context = this;
        ref->parent = object;
    }
    else {
        ref.reset(fit.value<timer_data_t *>());
    }

    auto const duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();

    timer_info_t info{};
    info.interval = interval;
    info.type = timerType;
    auto res = ref->ids.insert({timerId, info}).second;
    assert(res);

    manapi::timer timer;

    try {
        timer = manapi::async::current()->timerpool()->append_interval_sync(duration_ms,
            manapi::TIMER_POOR,
            [ref, timerId] (const manapi::timer &)
            mutable -> void {
                ref->context->m_processedCallbacks++;
                QTimerEvent e (timerId);
                QCoreApplication::sendEvent(ref->parent, &e);
                // if (ref->ids.size() == 1) {
                //     ref->parent->setProperty("timerData", QVariant());
                // }
                // ref->ids.erase(timerId);
                // ref->context->m_timers.erase(timerId);
        }).unwrap();

        res = this->m_timers.emplace(timerId,
            std::make_pair(std::move(timer), ref)).second;
        assert(res);
    }
    catch (...) {
        ref->ids.erase(timerId);
        if (timer) {
            timer.stop();
        }
        std::rethrow_exception(std::current_exception());
    }
}

inline bool manapi::qt::event_dispatcher::unregisterTimer(Qt::TimerId timerId) {
    if (manapi::async::context_exists()) {
        auto it = this->m_timers.find(timerId);
        if (it != this->m_timers.end()) {
            if (it->second.second->ids.size() == 1) {
                it->second.second->parent->setProperty("timerData", QVariant());
            }
            it->second.second->ids.erase(timerId);
            it->second.first.stop();
            this->m_timers.erase(it);

            return true;
        }
    }
    return false;
}

inline QList<QAbstractEventDispatcher::TimerInfoV2> manapi::qt::event_dispatcher::timersForObject(QObject* object) const {
    QList<QAbstractEventDispatcher::TimerInfoV2> res;

    auto fit = object->property("timerData");
    if (!fit.isNull()) {
        auto data = fit.value<timer_data_t *>();
        for (const auto &timer : data->ids) {
            res.append({ std::chrono::duration_cast<std::chrono::nanoseconds>(timer.second.interval), timer.first, timer.second.type });
        }
    }

    return res;
}

inline bool manapi::qt::event_dispatcher::unregisterTimers(QObject* object) {
    auto fit = object->property("timerData");
    if (fit.isNull()) {
        return false;
    }
    manapi::reference<timer_data_t> data( fit.value<timer_data_t *>());

    while (!data->ids.empty()) {
        auto it = data->ids.begin();
        this->unregisterTimer(it->first);
    }

    return true;
}

inline void manapi::qt::event_dispatcher::wakeUp() {
    if (this->flags & 0b1) {
        this->m_wakeupHandle->send();
    }
}

inline int manapi::qt::event_dispatcher::qtouv(QSocketNotifier::Type qtEventType) {
    switch (qtEventType) {
    case QSocketNotifier::Read:
        return manapi::ev::READ;
    case QSocketNotifier::Write:
        return manapi::ev::WRITE;
    default:
        qCritical() << "Unsupported QSocketNotifier type.";
        return -1;
    }
}