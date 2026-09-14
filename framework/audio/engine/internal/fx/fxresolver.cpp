/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include "fxresolver.h"

#include <algorithm>

#include "audio/common/audiosanitizer.h"

#include "log.h"

using namespace muse::async;
using namespace muse::audio;
using namespace muse::audio::fx;

//! The lists come per fx type (one resolver each); the chain's order runs across types, and the
//! chain node processes the list in order.
static void sortByChainOrder(std::vector<IFxProcessorPtr>& fxList)
{
    std::stable_sort(fxList.begin(), fxList.end(), [](const IFxProcessorPtr& a, const IFxProcessorPtr& b) {
        return a->params().chainOrder < b->params().chainOrder;
    });
}

std::vector<IFxProcessorPtr> FxResolver::resolveMasterFxList(const AudioFxChain& fxChain, const OutputSpec& outputSpec)
{
    ONLY_AUDIO_ENGINE_THREAD;

    TRACEFUNC;

    std::lock_guard lock(m_mutex);

    std::vector<IFxProcessorPtr> result;

    for (const auto& resolver : m_resolvers) {
        AudioFxChain fxChainByType;

        for (const auto& fx : fxChain) {
            if (resolver.first == fx.second.type() || fx.second.type() == AudioFxType::Undefined) {
                fxChainByType.insert(fx);
            }
        }

        if (fxChainByType.empty()) {
            continue;
        }

        std::vector<IFxProcessorPtr> fxList = resolver.second->resolveMasterFxList(std::move(fxChainByType), outputSpec);
        result.insert(result.end(), fxList.begin(), fxList.end());
    }

    sortByChainOrder(result);

    return result;
}

std::vector<IFxProcessorPtr> FxResolver::resolveFxList(const TrackId trackId, const AudioFxChain& fxChain, const OutputSpec& outputSpec)
{
    ONLY_AUDIO_ENGINE_THREAD;

    TRACEFUNC;

    std::lock_guard lock(m_mutex);

    std::vector<IFxProcessorPtr> result;

    for (const auto& resolver : m_resolvers) {
        AudioFxChain fxChainByType;

        for (const auto& fx : fxChain) {
            if (resolver.first == fx.second.type() || fx.second.type() == AudioFxType::Undefined) {
                fxChainByType.insert(fx);
            }
        }

        if (fxChainByType.empty()) {
            continue;
        }

        std::vector<IFxProcessorPtr> fxList = resolver.second->resolveFxList(trackId, std::move(fxChainByType), outputSpec);
        result.insert(result.end(), fxList.begin(), fxList.end());
    }

    sortByChainOrder(result);

    return result;
}

AudioResourceMetaList FxResolver::resolveAvailableResources() const
{
    ONLY_AUDIO_ENGINE_THREAD;

    TRACEFUNC;

    std::lock_guard lock(m_mutex);

    AudioResourceMetaList result;

    for (const auto& pair : m_resolvers) {
        const AudioResourceMetaList& resolvedResources = pair.second->resolveResources();
        result.insert(result.end(), resolvedResources.begin(), resolvedResources.end());
    }

    return result;
}

void FxResolver::registerResolver(const AudioFxType type, IResolverPtr resolver)
{
    ONLY_AUDIO_MAIN_OR_ENGINE_THREAD;

    std::lock_guard lock(m_mutex);

    m_resolvers.insert_or_assign(type, std::move(resolver));
}

void FxResolver::clearAllFx()
{
    ONLY_AUDIO_MAIN_OR_ENGINE_THREAD;

    std::lock_guard lock(m_mutex);

    for (auto it = m_resolvers.begin(); it != m_resolvers.end(); ++it) {
        it->second->clearAllFx();
    }
}
