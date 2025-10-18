#include <iostream>

#include <manapihttp/ManapiInitTools.hpp>
#include <manapihttp/ManapiEventLoop.hpp>
#include <manapihttp/ManapiTime.hpp>
#include <manapihttp/std/ManapiScopePtr.hpp>

#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>

#include "ManapiQt.hpp"

int main(int argc, char *argv[]) {
    manapi::init_tools::log_trace_init(manapi::debug::LOG_TRACE_HIGH);

    auto ctx = manapi::async::context::create(0).unwrap();

    ctx->run ([&] (const std::function<void()>& cb) -> void {
        auto event_dispatcher = manapi::qt::event_dispatcher::create().unwrap();
        QCoreApplication::setEventDispatcher(event_dispatcher);

        QApplication app (argc, argv);
        QWidget window;

        manapi::scope_ptr vbox (new QVBoxLayout);
        manapi::scope_ptr label (new QLabel);

        vbox->addWidget(label.release());

        window.setLayout(vbox.release());
        window.show();

        manapi::async::current()->eventloop()->custom_event_loop([&app, event_dispatcher] ()
            -> void {
            QApplication::exec();
            event_dispatcher->unsubscribe();
        });

        manapi::async::current()->timerpool()->append_interval_sync(200,
            [label = label.get()] (const manapi::timer &) -> void {
            label->setText(QString::fromStdString(std::format("{:}", manapi::time::current_time())));
        });

        cb ();
    });

    return 0;
}