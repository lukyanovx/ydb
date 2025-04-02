#pragma once
#include "events.h"
#include "request.h"

#include "abstract/collector.h"
#include "ydb/core/protos/config.pb.h"

#include <ydb/core/tx/columnshard/common/path_id.h>

#include <ydb/services/bg_tasks/abstract/interface.h>

namespace NKikimr::NOlap::NDataAccessorControl {

class TAccessorSignals: public NColumnShard::TCommonCountersOwner {
private:
    using TBase = NColumnShard::TCommonCountersOwner;

public:
    const NMonitoring::TDynamicCounters::TCounterPtr QueueSize;
    const NMonitoring::TDynamicCounters::TCounterPtr FetchingCount;
    const NMonitoring::TDynamicCounters::TCounterPtr AskNew;
    const NMonitoring::TDynamicCounters::TCounterPtr AskDuplication;
    const NMonitoring::TDynamicCounters::TCounterPtr ResultFromCache;
    const NMonitoring::TDynamicCounters::TCounterPtr ResultAskDirectly;

    TAccessorSignals()
        : TBase("AccessorsFetching")
        , QueueSize(TBase::GetValue("Queue/Count"))
        , FetchingCount(TBase::GetValue("Fetching/Count"))
        , AskNew(TBase::GetDeriviative("Ask/Fault/Count"))
        , AskDuplication(TBase::GetDeriviative("Ask/Duplication/Count"))
        , ResultFromCache(TBase::GetDeriviative("ResultFromCache/Count"))
        , ResultAskDirectly(TBase::GetDeriviative("ResultAskDirectly/Count")) {
    }
};

class IDataAccessorsManager {
private:
    virtual void DoAskData(const std::shared_ptr<TDataAccessorsRequest>& request, const TActorId& owner) = 0;
    virtual void DoRegisterController(std::unique_ptr<IGranuleDataAccessor>&& controller, const bool update, const TActorId& owner) = 0;
    virtual void DoUnregisterController(const TInternalPathId pathId, const TActorId& owner) = 0;
    virtual void DoAddPortion(const TPortionDataAccessor& accessor, const TActorId& owner) = 0;
    virtual void DoRemovePortion(const TPortionInfo::TConstPtr& portion, const TActorId& owner) = 0;
    virtual void DoClearCache(const TActorId& owner) = 0;
    const NActors::TActorId TabletActorId;

public:
    const NActors::TActorId& GetTabletActorId() const {
        return TabletActorId;
    }

    IDataAccessorsManager(const NActors::TActorId& tabletActorId)
        : TabletActorId(tabletActorId) {
    }

    virtual ~IDataAccessorsManager() = default;

    void AddPortion(const TPortionDataAccessor& accessor, std::optional<TActorId> owner = std::nullopt) {
        DoAddPortion(accessor, owner.value_or(TabletActorId));
    }

    void RemovePortion(const TPortionInfo::TConstPtr& portion, std::optional<TActorId> owner = std::nullopt) {
        DoRemovePortion(portion, owner.value_or(TabletActorId));
    }

    void AskData(const std::shared_ptr<TDataAccessorsRequest>& request, std::optional<TActorId> owner = std::nullopt) {
        AFL_VERIFY(request);
        AFL_VERIFY(request->HasSubscriber());
        return DoAskData(request, owner.value_or(TabletActorId));
    }

    void RegisterController(std::unique_ptr<IGranuleDataAccessor>&& controller, const bool update, std::optional<TActorId> owner = std::nullopt) {
        AFL_VERIFY(controller);
        return DoRegisterController(std::move(controller), update, owner.value_or(TabletActorId));
    }

    void UnregisterController(const TInternalPathId pathId, std::optional<TActorId> owner = std::nullopt) {
        return DoUnregisterController(pathId, owner.value_or(TabletActorId));
    }

    void ClearCache(std::optional<TActorId> owner = std::nullopt) {
        return DoClearCache(owner.value_or(TabletActorId));
    }
};

class TDataAccessorsManagerContainer: public NBackgroundTasks::TControlInterfaceContainer<IDataAccessorsManager> {
private:
    using TBase = NBackgroundTasks::TControlInterfaceContainer<IDataAccessorsManager>;

public:
    using TBase::TBase;
};

class TActorAccessorsManager: public IDataAccessorsManager {
private:
    using TBase = IDataAccessorsManager;
    const NActors::TActorId ActorId;
    std::shared_ptr<NDataAccessorControl::Foo> AccessorsCallback;

    virtual void DoAskData(const std::shared_ptr<TDataAccessorsRequest>& request, const TActorId&) override {
        NActors::TActivationContext::Send(ActorId, std::make_unique<TEvAskServiceDataAccessors>(request, GetTabletActorId()));
    }

    virtual void DoRegisterController(std::unique_ptr<IGranuleDataAccessor>&& controller, const bool update, const TActorId&) override {
        NActors::TActivationContext::Send(ActorId, std::make_unique<TEvRegisterController>(std::move(controller), update, GetTabletActorId()));
    }

    virtual void DoUnregisterController(const TInternalPathId pathId, const TActorId&) override {
        NActors::TActivationContext::Send(ActorId, std::make_unique<TEvUnregisterController>(pathId, GetTabletActorId()));
    }

    virtual void DoAddPortion(const TPortionDataAccessor& accessor, const TActorId&) override {
        NActors::TActivationContext::Send(ActorId, std::make_unique<TEvAddPortion>(accessor, GetTabletActorId()));
    }

    virtual void DoRemovePortion(const TPortionInfo::TConstPtr& portion, const TActorId&) override {
        NActors::TActivationContext::Send(ActorId, std::make_unique<TEvRemovePortion>(portion, GetTabletActorId()));
    }

    virtual void DoClearCache(const TActorId&) override {
        NActors::TActivationContext::Send(ActorId, std::make_unique<TEvClearCache>(GetTabletActorId()));
    }

public:
    TActorAccessorsManager(const NActors::TActorId& actorId, const NActors::TActorId& tabletActorId)
        : TBase(tabletActorId)
        , ActorId(actorId)
        , AccessorsCallback(std::make_shared<TActorAccessorsCallback>(ActorId)) {
        AFL_VERIFY(!!tabletActorId);
    }
};

class TLocalManager: public IDataAccessorsManager {
private:
    using TBase = IDataAccessorsManager;
    using TPortionId = ui64;

    THashMap<TActorId, THashMap<TInternalPathId, std::shared_ptr<IGranuleDataAccessor>>> Managers;
    THashMap<TActorId, THashMap<std::pair<TInternalPathId, TPortionId>, std::vector<std::shared_ptr<TDataAccessorsRequest>>>> RequestsByPortion;
    TAccessorSignals Counters;
    const std::shared_ptr<Foo> AccessorCallback;

    ui64 TotalMemorySize = 1 << 30;

    class TPortionToAsk {
    private:
        TPortionInfo::TConstPtr Portion;
        YDB_READONLY_DEF(std::shared_ptr<const TAtomicCounter>, AbortionFlag);
        YDB_READONLY_DEF(TString, ConsumerId);

    public:
        TPortionToAsk(
            const TPortionInfo::TConstPtr& portion,
            const std::shared_ptr<const TAtomicCounter>& abortionFlag,
            const TString& consumerId)
            : Portion(portion)
            , AbortionFlag(abortionFlag)
            , ConsumerId(consumerId) {
        }

        TPortionInfo::TConstPtr ExtractPortion() {
            auto result = std::move(Portion);
            Portion = nullptr; // Prevent use-after-move
            return result;
        }
    };

    std::deque<std::tuple<TActorId, TInternalPathId, TPortionToAsk>> PortionsAsk;
    TPositiveControlInteger PortionsAskInFlight;

    void DrainQueue();
    void ResizeCache();

    virtual void DoAskData(const std::shared_ptr<TDataAccessorsRequest>& request, const TActorId& owner);
    virtual void DoRegisterController(std::unique_ptr<IGranuleDataAccessor>&& controller, const bool update, const TActorId& owner);

    virtual void DoUnregisterController(const TInternalPathId pathId, const TActorId& owner) {
        auto it = Managers.find(owner);
        if (it == Managers.end()) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "unregister_controller_owner_not_found")("owner", owner)("pathId", pathId);
            return;
        }

        if (!it->second.erase(pathId)) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "unregister_controller_path_not_found")("owner", owner)("pathId", pathId);
            return;
        }

        ResizeCache();
    }

    virtual void DoAddPortion(const TPortionDataAccessor& accessor, const TActorId& owner);

    virtual void DoRemovePortion(const TPortionInfo::TConstPtr& portionInfo, const TActorId& owner) {
        try {
            auto manager = getManagerSafe(owner, portionInfo->GetPathId());
            if (manager) {
                manager->ModifyPortions({}, { portionInfo->GetPortionId() });
            }
        } catch (const std::exception& ex) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "remove_portion_failed")
                ("owner", owner)("pathId", portionInfo->GetPathId())("error", ex.what());
        }
    }

    virtual void DoClearCache(const TActorId& owner) {
        if (!Managers.erase(owner)) {
            AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "clear_cache_owner_not_found")("owner", owner);
            return;
        }
        ResizeCache();
    }

    // Safe version that returns nullptr if not found instead of asserting
    std::shared_ptr<IGranuleDataAccessor> getManagerSafe(const TActorId& owner, const TInternalPathId& tableId) {
        auto it = Managers.find(owner);
        if (it == Managers.end()) {
            return nullptr;
        }

        auto iit = it->second.find(tableId);
        if (iit == it->second.end()) {
            return nullptr;
        }

        return iit->second;
    }

    // Original version with assertions - keep for backward compatibility where asserts are expected
    std::shared_ptr<IGranuleDataAccessor> getManager(const TActorId& owner, const TInternalPathId& tableId) {
        auto it = Managers.find(owner);
        AFL_VERIFY(it != Managers.end());
        auto iit = it->second.find(tableId);
        AFL_VERIFY(iit != it->second.end());
        return iit->second;
    }

public:
    class TTestingCallback: public Foo {
    private:
        std::weak_ptr<TLocalManager> Manager;

        virtual void OnAccessorsFetched(std::vector<TPortionDataAccessor>&& accessors, const TActorId& owner) override {
            auto mImpl = Manager.lock();
            if (!mImpl) {
                return;
            }
            for (auto&& i : accessors) {
                mImpl->AddPortion(i,  owner);
            }
        }

    public:
        TTestingCallback() {}

        void InitManager(const std::weak_ptr<TLocalManager>& manager) {
            Manager = manager;
        }
    };

    class TCallbackWrapper: public IAccessorCallback {
        const std::shared_ptr<Foo> Callback;
        TActorId Owner;
    public:
        TCallbackWrapper(const std::shared_ptr<Foo>& callback, const TActorId& owner)
            : Callback(callback), Owner(owner) {}

        void OnAccessorsFetched(std::vector<TPortionDataAccessor>&& accessors) override {
            Callback->OnAccessorsFetched(move(accessors), Owner);
        }
    };

    static std::shared_ptr<TLocalManager> BuildForTests() {
        auto callback = std::make_shared<TTestingCallback>();
        std::shared_ptr<TLocalManager> result = std::make_shared<TLocalManager>(callback);
        callback->InitManager(result);
        return result;
    }

    TLocalManager(const std::shared_ptr<Foo>& callback)
        : TBase(NActors::TActorId())
        , AccessorCallback(callback) {
        if (HasAppData()) {
            TotalMemorySize = AppDataVerified().ColumnShardConfig.GetWritingInFlightRequestsCountLimit();
        }
    }
};

}   // namespace NKikimr::NOlap::NDataAccessorControl
