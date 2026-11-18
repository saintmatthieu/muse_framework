/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace muse::audio {
/**
 * Optionally implemented by an IAudioDriver (cast to it): how close its callbacks come to
 * starving the device, and the margin it keeps against that, chosen among levels, each adding
 * latency. Thread-safe; one reader takes the windows.
 */
class IAudioOutputMonitor
{
public:
    virtual ~IAudioOutputMonitor() = default;

    /**
     * What happened since the last window was taken.
     */
    struct Window {
        uint32_t callbacks = 0;
        /**
         * Callbacks that wrote after the device needed their frames: what the device played
         * there was silence.
         */
        uint32_t underruns = 0;
        /**
         * Times the device's position stopped while time went on, where the platform tells
         * the position: silence played in place of the stream's frames, whoever was late.
         */
        uint32_t clockStalls = 0;
        /**
         * How much earlier than needed the callbacks' frames were ready, in ms (mean and least).
         * Negative: an underrun.
         */
        double headroomMeanMs = 0.0;
        double headroomMinMs = 0.0;
        /**
         * Rendering time over the period, in % (mean and most).
         */
        double loadMeanPercent = 0.0;
        double loadMaxPercent = 0.0;
        /**
         * From a callback to its first frame being played, in ms, last measured (0: unknown).
         */
        double outputLatencyMs = 0.0;
        /**
         * The loudest sample written (linear).
         */
        float outputPeak = 0.0f;
    };

    virtual Window takeWindow() = 0;

    /**
     * The device's period, in ms (0 while not opened).
     */
    virtual double periodMs() const = 0;

    /**
     * The margins the driver can keep, least first, three at most: each how much is kept queued
     * for the device, in ms. Empty while not opened.
     */
    virtual std::vector<double> marginLevelsMs() const = 0;
    virtual size_t marginLevel() const = 0;
    /**
     * To be called while nothing sounds: the change may be heard (another driver may have to
     * reopen the device for it).
     */
    virtual void setMarginLevel(size_t level) = 0;

    /**
     * For testing the detection: the next callback starts this much later.
     */
    virtual void delayNextCallback(double ms) = 0;
};
}
