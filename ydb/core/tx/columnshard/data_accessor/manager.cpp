#include "manager.h"

#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>

#include <ydb/library/accessor/positive_integer.h>

namespace NKikimr::NOlap::NDataAccessorControl {

void TLocalManager::DrainQueue() {
    std::optional<TActorId> lastOwner;
    std::optional<TInternalPathId> lastTable;
    std::shared_ptr<IGranuleDataAccessor> lastDataAccessor = nullptr;
    TPositiveControlInteger countToFlight;

    const auto maxPortionsMetadataAsk = NYDBTest::TControllers::GetColumnShardController()->GetLimitForPortionsMetadataAsk();
    while (PortionsAskInFlight + countToFlight < maxPortionsMetadataAsk && !PortionsAsk.empty()) {
        THashMap<TActorId, THashMap<TInternalPathId, TPortionsByConsumer>> portionsToAsk;

        while (PortionsAskInFlight + countToFlight < 1000 && !PortionsAsk.empty()) {
            auto [owner, tableId, portionToAsk] = PortionsAsk.front();
            auto p = portionToAsk.ExtractPortion();
            const TString consumerId = portionToAsk.GetConsumerId();
            PortionsAsk.pop_front();

            // Check if we need to update our cached accessor
            if (!lastOwner || *lastOwner != owner || !lastTable || *lastTable != tableId) {
                lastOwner = owner;
                lastTable = tableId;
                lastDataAccessor = nullptr; // Reset cached accessor

                auto it = Managers.find(owner);
                if (it != Managers.end()) {
                    auto iit = it->second.find(tableId);
                    if (iit != it->second.end()) {
                        lastDataAccessor = iit->second;
                    }
                }
            }

            auto it = RequestsByPortion.find(owner);
            if (it == RequestsByPortion.end()) {
                continue;
            }

            auto iit = it->second.find(std::pair{tableId, p->GetPortionId()});
            if (iit == it->second.end()) {
                continue;
            }

            if (!lastDataAccessor) {
                // No accessor available, mark all requests as failed
                for (auto&& i : iit->second) {
                    if (!i->IsFetched() && !i->IsAborted()) {
                        i->AddError(p->GetPathId(), "path id absent");
                    }
                }
                it->second.erase(iit);
                if (it->second.empty()) {
                    RequestsByPortion.erase(it);
                }
            } else {
                bool toAsk = false;
                for (auto&& i : iit->second) {
                    if (!i->IsFetched() && !i->IsAborted()) {
                        toAsk = true;
                        break;
                    }
                }

                if (!toAsk) {
                    it->second.erase(iit);
                    if (it->second.empty()) {
                        RequestsByPortion.erase(it);
                    }
                } else {
                    portionsToAsk[owner][tableId].UpsertConsumer(consumerId).AddPortion(p);
                    ++countToFlight;
                }
            }
        }

        for (auto& [owner, portionsByTable] : portionsToAsk) {
            for (auto& [tableId, portions] : portionsByTable) {
                auto managerIt = Managers.find(owner);
                if (managerIt == Managers.end()) {
                    continue; // Skip if owner not found
                }

                auto tableIt = managerIt->second.find(tableId);
                if (tableIt == managerIt->second.end()) {
                    continue; // Skip if table not found
                }

                auto& manager = tableIt->second;
                auto dataAnalyzed = manager->AnalyzeData(portions);

                for (auto&& accessor : dataAnalyzed.GetCachedAccessors()) {
                    auto it = RequestsByPortion.find(owner);
                    if (it == RequestsByPortion.end()) {
                        continue;
                    }

                    auto iit = it->second.find(std::pair{tableId, accessor.GetPortionInfo().GetPortionId()});
                    if (iit == it->second.end()) {
                        continue;
                    }

                    for (auto&& i : iit->second) {
                        Counters.ResultFromCache->Add(1);
                        if (!i->IsFetched() && !i->IsAborted()) {
                            i->AddAccessor(accessor);
                        }
                    }

                    it->second.erase(iit);
                    if (it->second.empty()) {
                        RequestsByPortion.erase(it);
                    }
                    --countToFlight;
                }

                if (!dataAnalyzed.GetPortionsToAsk().IsEmpty()) {
                    THashMap<TInternalPathId, TPortionsByConsumer> portionsToAskImpl;
                    Counters.ResultAskDirectly->Add(dataAnalyzed.GetPortionsToAsk().GetPortionsCount());
                    portionsToAskImpl.emplace(tableId, dataAnalyzed.DetachPortionsToAsk());
                    manager->AskData(std::move(portionsToAskImpl), std::make_shared<TCallbackWrapper>(AccessorCallback, owner));
                }
            }
        }
    }

    PortionsAskInFlight.Add(countToFlight);
    Counters.FetchingCount->Set(PortionsAskInFlight);
    Counters.QueueSize->Set(PortionsAsk.size());
}

void TLocalManager::ResizeCache() {
    if (Managers.empty()) {
        return; // Nothing to resize
    }

    auto size = TotalMemorySize / Managers.size();
    for (auto& [_, managerMap] : Managers) {
        for (auto& [__, manager] : managerMap) {
            if (manager) {
                manager->Resize(size);
            }
        }
    }
}

void TLocalManager::DoAskData(const std::shared_ptr<TDataAccessorsRequest>& request, const TActorId& owner) {
    AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "ask_data")("request", request->DebugString());

    for (auto&& pathId : request->GetPathIds()) {
        auto it = Managers.find(owner);
        if (it == Managers.end()) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "owner_not_found")("owner", owner);
            request->AddError(pathId, "Owner not found");
            continue;
        }

        auto iit = it->second.find(pathId);
        if (iit == it->second.end()) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "table_not_found")("pathId", pathId);
            request->AddError(pathId, "Table not found");
            continue;
        }

        auto portions = request->StartFetching(pathId);
        for (auto&& [portionId, portion] : portions) {
            auto& ownerRequests = RequestsByPortion[owner];
            auto requestKey = std::pair{pathId, portionId};
            auto itRequest = ownerRequests.find(requestKey);

            if (itRequest == ownerRequests.end()) {
                ownerRequests[requestKey].push_back(request);
                PortionsAsk.emplace_back(std::tuple{owner, pathId,
                    TPortionToAsk{portion, request->GetAbortionFlag(), request->GetConsumer()}});
                Counters.AskNew->Add(1);
            } else {
                itRequest->second.push_back(request);
                Counters.AskDuplication->Add(1);
            }
        }
    }

    DrainQueue();
}

void TLocalManager::DoRegisterController(std::unique_ptr<IGranuleDataAccessor>&& controller, const bool update, const TActorId& owner) {
    const auto pathId = controller->GetPathId();

    if (update) {
        auto it = Managers.find(owner);
        if (it != Managers.end()) {
            auto iit = it->second.find(pathId);
            if (iit != it->second.end()) {
                // Clean up the old controller explicitly
                iit->second.reset();
                iit->second = std::move(controller);
            } else {
                it->second[pathId] = std::move(controller);
            }
        } else {
            Managers[owner][pathId] = std::move(controller);
        }
    } else {
        auto& ownerMap = Managers[owner];
        if (ownerMap.find(pathId) != ownerMap.end()) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "controller_already_exists")("pathId", pathId);
            return;
        }
        ownerMap[pathId] = std::move(controller);
    }

    ResizeCache();
}

void TLocalManager::DoAddPortion(const TPortionDataAccessor& accessor, const TActorId& owner) {
    const auto pathId = accessor.GetPortionInfo().GetPathId();
    const auto portionId = accessor.GetPortionInfo().GetPortionId();

    auto it = Managers.find(owner);
    if (it == Managers.end()) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "owner_not_found_add_portion")("owner", owner);
        return;
    }

    auto iit = it->second.find(pathId);
    if (iit == it->second.end()) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "table_not_found_add_portion")("pathId", pathId);
        return;
    }

    iit->second->ModifyPortions({accessor}, {});

    auto reqIt = RequestsByPortion.find(owner);
    if (reqIt != RequestsByPortion.end()) {
        auto portionKey = std::pair{pathId, portionId};
        auto iit = reqIt->second.find(portionKey);

        if (iit != reqIt->second.end()) {
            bool wasInFlight = false;
            for (auto&& i : iit->second) {
                if (!i->IsFetched() && !i->IsAborted()) {
                    i->AddAccessor(accessor);
                    wasInFlight = true;
                }
            }

            reqIt->second.erase(iit);
            if (reqIt->second.empty()) {
                RequestsByPortion.erase(reqIt);
            }

            if (wasInFlight) {
                PortionsAskInFlight.Add(-1); // Decrement only if this portion was in flight
            }
        }
    }

    DrainQueue();
}

} // namespace NKikimr::NOlap::NDataAccessorControl
