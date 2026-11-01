// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Tracktion.h"

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace rovox
{

struct ExecutorTimeout : std::runtime_error
{
    ExecutorTimeout() : std::runtime_error ("message thread did not start the task in time") {}
};

/** Runs closures on the JUCE message thread on behalf of transport threads (MCP, WS, HTTP, CLI).
    `timeout` is a start deadline: a task that has not begun by then is dropped and its future carries ExecutorTimeout
    (a task that has begun always runs to completion). Called on the message thread itself, the closure runs inline,
    so a command may submit another command without deadlocking. Requires a running message loop otherwise. */
class MessageThreadExecutor
{
public:
    template <typename F>
    auto run (F&& f, std::chrono::milliseconds timeout = std::chrono::seconds (30)) -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        auto fn = std::make_shared<std::decay_t<F>> (std::forward<F> (f));
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        auto body = [promise, fn, deadline] (bool checkDeadline)
        {
            if (checkDeadline && std::chrono::steady_clock::now() > deadline)
            {
                promise->set_exception (std::make_exception_ptr (ExecutorTimeout()));
                return;
            }

            try
            {
                if constexpr (std::is_void_v<R>)
                {
                    (*fn)();
                    promise->set_value();
                }
                else
                {
                    promise->set_value ((*fn)());
                }
            }
            catch (...)
            {
                promise->set_exception (std::current_exception());
            }
        };

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            body (false);
        else
            juce::MessageManager::callAsync ([body] { body (true); });

        return future;
    }
};

} // namespace rovox
