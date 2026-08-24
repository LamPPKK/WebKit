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

#include <cstddef>
#include <vector>

namespace XanhProcessRecovery {

inline constexpr std::size_t maximumFrameStatesToInspect = 4096;

template<typename FrameState>
bool canSafelyRestoreFrameTree(const FrameState& root)
{
    std::vector<const FrameState*> pending { &root };
    std::size_t inspected = 0;
    while (!pending.empty()) {
        auto* state = pending.back();
        pending.pop_back();
        if (++inspected > maximumFrameStatesToInspect || state->httpBody)
            return false;
        if (state->children.size() > maximumFrameStatesToInspect - inspected - pending.size())
            return false;
        for (const auto& child : state->children)
            pending.push_back(child.ptr());
    }
    return true;
}

} // namespace XanhProcessRecovery
