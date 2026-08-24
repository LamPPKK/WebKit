/*
 * Copyright (C) 2026 Xanh Browser contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
 */

#pragma once

namespace XanhProcessRecovery {

enum class Action {
    ignoreRequestedTermination,
    restorePage,
    refuseUnsafeRestore,
    stopAutomaticRecovery,
};

enum class State {
    available,
    recoveryLoading,
    exhausted,
};

class Policy {
public:
    Action processTerminated(bool requestedByClient, bool canSafelyRestore)
    {
        if (requestedByClient)
            return Action::ignoreRequestedTermination;
        if (!canSafelyRestore) {
            m_state = State::exhausted;
            return Action::refuseUnsafeRestore;
        }
        if (m_state != State::available) {
            m_state = State::exhausted;
            return Action::stopAutomaticRecovery;
        }
        m_state = State::recoveryLoading;
        return Action::restorePage;
    }

    void navigationFinished()
    {
        if (m_state == State::recoveryLoading)
            m_state = State::exhausted;
    }

    void userRequestedNavigation()
    {
        m_state = State::available;
    }

    State state() const
    {
        return m_state;
    }

private:
    State m_state { State::available };
};

} // namespace XanhProcessRecovery
