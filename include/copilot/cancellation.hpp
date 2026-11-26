// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace copilot
{

namespace detail
{

struct CancellationState
{
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    uint64_t next_callback_id = 1;
    std::map<uint64_t, std::function<void()>> callbacks;

    void cancel()
    {
        std::map<uint64_t, std::function<void()>> to_run;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cancelled.exchange(true))
                return;
            to_run.swap(callbacks);
        }
        for (auto& [_, callback] : to_run)
        {
            try
            {
                callback();
            }
            catch (...)
            {
            }
        }
    }
};

} // namespace detail

/// Observer side of a cancellation signal. A default-constructed token is never
/// cancelled. Copies share state.
class CancellationToken
{
  public:
    CancellationToken() = default;

    bool is_cancelled() const noexcept
    {
        return state_ && state_->cancelled.load();
    }

    /// False for a default-constructed token, which is never cancelled.
    bool can_be_cancelled() const noexcept
    {
        return static_cast<bool>(state_);
    }

    /// Run `callback` once when the token is cancelled (immediately if it already
    /// is). Returns an id for unregister(); 0 when the callback ran inline or the
    /// token can never be cancelled.
    uint64_t on_cancel(std::function<void()> callback) const
    {
        if (!state_)
            return 0;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->cancelled.load())
            {
                const auto id = state_->next_callback_id++;
                state_->callbacks.emplace(id, std::move(callback));
                return id;
            }
        }
        callback();
        return 0;
    }

    void unregister(uint64_t id) const
    {
        if (!state_ || id == 0)
            return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->callbacks.erase(id);
    }

  private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<detail::CancellationState> state_;
};

/// Owner side of a cancellation signal.
class CancellationSource
{
  public:
    CancellationSource() : state_(std::make_shared<detail::CancellationState>()) {}

    CancellationToken token() const
    {
        return CancellationToken(state_);
    }

    /// Signal cancellation; callbacks run on the calling thread, once.
    void cancel()
    {
        state_->cancel();
    }

    bool is_cancelled() const noexcept
    {
        return state_->cancelled.load();
    }

  private:
    std::shared_ptr<detail::CancellationState> state_;
};

namespace detail
{

/// A CancellationToken cancelled when `token` is stop-requested, for as long as this
/// object lives. Keep it alive for the duration of the call it bounds.
class StopTokenCancellation
{
  public:
    StopTokenCancellation(const StopTokenCancellation&) = delete;
    StopTokenCancellation& operator=(const StopTokenCancellation&) = delete;

    explicit StopTokenCancellation(std::stop_token token)
        : stop_token_(token), callback_(std::move(token), [source = source_]() mutable { source.cancel(); })
    {
    }

    /// A token that can never be cancelled when the stop_token cannot be stopped.
    CancellationToken token() const
    {
        return stop_token_.stop_possible() ? source_.token() : CancellationToken{};
    }

    bool is_cancelled() const noexcept
    {
        return source_.is_cancelled();
    }

    const std::stop_token& stop_token() const noexcept
    {
        return stop_token_;
    }

  private:
    CancellationSource source_;
    std::stop_token stop_token_;
    std::stop_callback<std::function<void()>> callback_;
};

} // namespace detail

} // namespace copilot
