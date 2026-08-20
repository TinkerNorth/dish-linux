// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QObject>
#include <QSocketNotifier>

#include <initializer_list>
#include <memory>

namespace dish::util {

// Delivers a Unix signal as a Qt signal on the event-loop thread. A handler may
// only call async-signal-safe functions, so the installed handler write()s one
// byte to a pipe and every decision happens back here.
class UnixSignalWatcher : public QObject {
    Q_OBJECT
  public:
    // Takes ownership of readFd. Each byte read is reported as the signal
    // number it stands for, which is why signals above 255 are not watchable.
    explicit UnixSignalWatcher(int readFd, QObject* parent = nullptr);
    ~UnixSignalWatcher() override;

  Q_SIGNALS:
    void signalled(int number);

  private:
    friend std::unique_ptr<UnixSignalWatcher> installSignalWatcher(std::initializer_list<int>);

    void drain();

    int readFd_;
    int writeFd_ = -1;
    bool restoresHandlers_ = false;
    std::unique_ptr<QSocketNotifier> notifier_;
};

// Installs a handler for each number that feeds the returned watcher, and
// restores the default disposition when that watcher dies. Null if the pipe
// cannot be created, or if a watcher is already live — handlers are
// process-wide, so a second one would silently steal the first's signals.
std::unique_ptr<UnixSignalWatcher> installSignalWatcher(std::initializer_list<int> numbers);

} // namespace dish::util
